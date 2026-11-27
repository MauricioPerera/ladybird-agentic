/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/HTML/ActivateTab.h>
#include <LibWebView/Application.h>
#include <LibWebView/Automation/FlowRunner.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/Vault/VaultTypes.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

static constexpr u32 step_poll_interval_ms = 150;

static ErrorOr<URL::URL> validate_automation_url(StringView raw_url)
{
    auto parsed = URL::Parser::basic_parse(raw_url);
    if (!parsed.has_value())
        return Error::from_string_literal("Invalid URL");
    if (parsed->scheme() == "http"sv || parsed->scheme() == "https"sv || *parsed == URL::about_blank())
        return parsed.release_value();
    return Error::from_string_literal("Only http:, https:, and about:blank URLs are allowed in automation flows");
}

static FlowLocator interpolate_locator(FlowLocator const& locator, HashMap<String, String> const& vars)
{
    FlowLocator out = locator;
    if (!out.value.is_empty())
        out.value = interpolate_flow_variables(out.value, vars);
    if (!out.role.is_empty())
        out.role = interpolate_flow_variables(out.role, vars);
    if (out.name.has_value())
        out.name = interpolate_flow_variables(*out.name, vars);
    return out;
}

static String generate_run_id()
{
    return MUST(String::formatted("run-{}-{}", UnixDateTime::now().milliseconds_since_epoch(), get_random<u32>()));
}

FlowRunner::FlowRunner(Application& application, FlowStore& store)
    : m_application(application)
    , m_store(store)
{
}

FlowRunner::~FlowRunner()
{
    cancel_all_runs("Automation runner destroyed"sv);
}

void FlowRunner::cleanup_run_timers(ActiveRun& run)
{
    if (run.step_timer) {
        run.step_timer->stop();
        run.step_timer.clear();
    }
    if (run.nav_wait_id.has_value()) {
        m_application.cancel_automation_navigation_wait(*run.nav_wait_id);
        run.nav_wait_id.clear();
    }
}

ErrorOr<String> FlowRunner::start_run(FlowRunRequest request, Function<void(FlowRunRecord const&)> on_complete)
{
    Optional<FlowDefinition> flow;
    if (request.inline_flow.has_value()) {
        flow = *request.inline_flow;
    } else if (request.flow_id.has_value()) {
        flow = m_store.get_flow(*request.flow_id);
        if (!flow.has_value())
            return Error::from_string_literal("Flow not found");
    } else {
        return Error::from_string_literal("Either 'flow_id' or inline 'flow' must be provided");
    }

    if (flow->steps.is_empty())
        return Error::from_string_literal("Flow has no steps");

    u64 resolved_tab_id = 0;
    bool opened_tab = false;
    if (request.tab_id.has_value()) {
        auto view = m_application.find_automation_view(*request.tab_id, request.require_mcp_permission);
        if (!view.has_value())
            return Error::from_string_literal("Target tab not found or permission denied");
        if (view->url().scheme() == "about"sv && view->url() != URL::about_blank())
            return Error::from_string_literal("Cannot run automation flow inside an internal about: page; select a web tab or open a new tab");
        resolved_tab_id = view->view_id();
    } else if (request.open_new_tab) {
        auto view = m_application.open_automation_tab(Web::HTML::ActivateTab::Yes, request.require_mcp_permission);
        if (!view.has_value())
            return Error::from_string_literal("UI unavailable, tab limit reached, or policy denied opening a new automation tab");
        resolved_tab_id = view->view_id();
        opened_tab = true;
    } else {
        return Error::from_string_literal("No target tab specified and open_new_tab is false");
    }

    auto run_id = generate_run_id();
    auto active = make<ActiveRun>();
    active->run_id = run_id;
    active->flow = flow.release_value();
    active->tab_id = resolved_tab_id;
    active->require_mcp_permission = request.require_mcp_permission;
    active->opened_tab = opened_tab;
    active->close_tab_on_finish = request.close_tab_on_finish;
    active->on_complete = move(on_complete);

    for (auto const& [k, v] : active->flow.variables)
        active->runtime_vars.set(k, v);
    for (auto const& [k, v] : request.variables)
        active->runtime_vars.set(k, v);

    active->record.run_id = run_id;
    active->record.flow_id = active->flow.id;
    active->record.flow_name = active->flow.name;
    active->record.status = "running"_string;
    active->record.tab_id = resolved_tab_id;
    active->record.started_at_ms = UnixDateTime::now().milliseconds_since_epoch();
    active->record.total_steps = active->flow.steps.size();

    active->record.steps.ensure_capacity(active->flow.steps.size());
    for (size_t i = 0; i < active->flow.steps.size(); ++i) {
        auto const& step = active->flow.steps[i];
        FlowStepResult step_result;
        step_result.index = i;
        step_result.action = String::from_utf8_without_validation(flow_action_name(step.action).bytes());
        step_result.label = step.label;
        step_result.status = "pending"_string;
        active->record.steps.unchecked_append(move(step_result));
    }

    auto record_snapshot = active->record;
    m_active_runs.set(run_id, move(active));
    m_store.upsert_run(move(record_snapshot), false);

    Core::deferred_invoke([weak_self = make_weak_ptr<FlowRunner>(), run_id] {
        if (weak_self)
            weak_self->run_next_step(run_id);
    });

    return run_id;
}

bool FlowRunner::cancel_run(StringView run_id)
{
    auto entry = m_active_runs.get(run_id);
    if (!entry.has_value())
        return false;
    fail_run(entry.value()->run_id, "Cancelled by user"_string, "cancelled"sv);
    return true;
}

void FlowRunner::cancel_all_runs(StringView reason)
{
    Vector<String> ids;
    ids.ensure_capacity(m_active_runs.size());
    for (auto const& [id, _] : m_active_runs)
        ids.unchecked_append(id);
    for (auto const& id : ids)
        fail_run(id, String::from_utf8_without_validation(reason.bytes()), "cancelled"sv);
}

void FlowRunner::did_close_view(u64 view_id)
{
    Vector<String> ids;
    for (auto const& [id, run] : m_active_runs) {
        if (run->tab_id == view_id)
            ids.append(id);
    }
    for (auto const& id : ids)
        fail_run(id, "Target tab was closed"_string, "failed"sv);
}

void FlowRunner::run_next_step(String const& run_id)
{
    auto entry = m_active_runs.get(run_id);
    if (!entry.has_value())
        return;
    auto& run = *entry.value();
    if (run.finished)
        return;

    if (run.step_index >= run.flow.steps.size()) {
        finish_run_success(run_id);
        return;
    }

    auto view = m_application.find_automation_view(run.tab_id, run.require_mcp_permission);
    if (!view.has_value()) {
        fail_run(run_id, "Target tab disappeared or permission was revoked"_string);
        return;
    }

    auto const idx = run.step_index;
    auto const& step = run.flow.steps[idx];
    auto const timeout_ms = step.timeout_ms.value_or(run.flow.default_timeout_ms);
    run.step_started_at = MonotonicTime::now();
    run.record.current_step_index = idx;
    run.record.steps[idx].status = "running"_string;
    m_store.upsert_run(run.record, false);

    switch (step.action) {
    case FlowStep::Action::Open: {
        auto raw_url = interpolate_flow_variables(*step.url, run.runtime_vars);
        auto parsed = validate_automation_url(raw_url);
        if (parsed.is_error()) {
            fail_run(run_id, MUST(String::formatted("{}", parsed.error())));
            return;
        }
        auto url = parsed.release_value();
        if (run.require_mcp_permission) {
            if (auto policy_check = mcp_validate_url_against_policy(url, m_application.mcp_security_policy()); policy_check.is_error()) {
                fail_run(run_id, MUST(String::formatted("{}", policy_check.error())));
                return;
            }
        }
        if (step.wait_for_load && url != URL::about_blank()) {
            run.nav_wait_id = m_application.start_automation_navigation_wait(run.tab_id, timeout_ms, run.require_mcp_permission, [weak_self = make_weak_ptr<FlowRunner>(), run_id, idx, tab_id = run.tab_id, require_mcp = run.require_mcp_permission](ErrorOr<void> result) {
                if (!weak_self)
                    return;
                if (auto active = weak_self->m_active_runs.get(run_id); active.has_value())
                    active.value()->nav_wait_id.clear();
                if (result.is_error()) {
                    weak_self->fail_run(run_id, MUST(String::formatted("Navigation failed: {}", result.error())));
                    return;
                }
                if (auto landed_view = weak_self->m_application.find_automation_view(tab_id, require_mcp); landed_view.has_value()) {
                    if (landed_view->url().scheme() == "about"sv && landed_view->url() != URL::about_blank()) {
                        weak_self->fail_run(run_id, MUST(String::formatted("Navigation landed on browser error page '{}' (check SSL certificate or network)", landed_view->url().to_string())));
                        return;
                    }
                }
                weak_self->complete_current_step(run_id, idx);
            });
            view->load_from_browser_ui(url);
            return;
        }
        view->load_from_browser_ui(url);
        complete_current_step(run_id, idx);
        return;
    }

    case FlowStep::Action::Reload: {
        if (step.wait_for_load) {
            run.nav_wait_id = m_application.start_automation_navigation_wait(run.tab_id, timeout_ms, run.require_mcp_permission, [weak_self = make_weak_ptr<FlowRunner>(), run_id, idx, tab_id = run.tab_id, require_mcp = run.require_mcp_permission](ErrorOr<void> result) {
                if (!weak_self)
                    return;
                if (auto active = weak_self->m_active_runs.get(run_id); active.has_value())
                    active.value()->nav_wait_id.clear();
                if (result.is_error()) {
                    weak_self->fail_run(run_id, MUST(String::formatted("Reload failed: {}", result.error())));
                    return;
                }
                if (auto landed_view = weak_self->m_application.find_automation_view(tab_id, require_mcp); landed_view.has_value()) {
                    if (landed_view->url().scheme() == "about"sv && landed_view->url() != URL::about_blank()) {
                        weak_self->fail_run(run_id, MUST(String::formatted("Reload landed on browser error page '{}' (check SSL certificate or network)", landed_view->url().to_string())));
                        return;
                    }
                }
                weak_self->complete_current_step(run_id, idx);
            });
            view->reload();
            return;
        }
        view->reload();
        complete_current_step(run_id, idx);
        return;
    }

    case FlowStep::Action::Back:
    case FlowStep::Action::Forward: {
        view->traverse_the_history_by_delta(step.action == FlowStep::Action::Back ? -1 : 1);
        run.step_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(250), [weak_self = make_weak_ptr<FlowRunner>(), run_id, idx] {
            if (weak_self)
                weak_self->complete_current_step(run_id, idx);
        });
        run.step_timer->start();
        return;
    }

    case FlowStep::Action::Sleep: {
        auto delay = step.ms.value_or(250);
        run.step_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(delay), [weak_self = make_weak_ptr<FlowRunner>(), run_id, idx] {
            if (weak_self)
                weak_self->complete_current_step(run_id, idx);
        });
        run.step_timer->start();
        return;
    }

    case FlowStep::Action::AssertUrl: {
        if (run.require_mcp_permission && m_application.is_view_mcp_secret_tainted(run.tab_id)) {
            fail_run(run_id, "Blocked by credential vault quarantine: cannot inspect tainted tab URL until navigation commits to a new document"_string);
            return;
        }
        Optional<String> contains;
        Optional<String> equals;
        if (step.contains.has_value())
            contains = interpolate_flow_variables(*step.contains, run.runtime_vars);
        if (step.equals.has_value())
            equals = interpolate_flow_variables(*step.equals, run.runtime_vars);
        poll_assert_url(run_id, idx, move(contains), move(equals), timeout_ms);
        return;
    }

    case FlowStep::Action::Click:
    case FlowStep::Action::WaitFor:
    case FlowStep::Action::AssertVisible:
    case FlowStep::Action::Fill:
    case FlowStep::Action::AssertText:
    case FlowStep::Action::Extract: {
        bool locator_reads_text = step.locator.has_value()
            && (step.locator->kind == FlowLocator::Kind::Text
                || (step.locator->kind == FlowLocator::Kind::Role && step.locator->name.has_value() && !step.locator->name->is_empty()));
        if (run.require_mcp_permission
            && m_application.is_view_mcp_secret_tainted(run.tab_id)
            && (step.action == FlowStep::Action::Extract
                || step.action == FlowStep::Action::AssertText
                || ((step.action == FlowStep::Action::WaitFor || step.action == FlowStep::Action::AssertVisible) && locator_reads_text))) {
            fail_run(run_id, "Blocked by credential vault quarantine: cannot inspect tainted tab until navigation commits to a new document"_string);
            return;
        }
        auto locator = interpolate_locator(*step.locator, run.runtime_vars);
        JsonObject params;
        if (step.action == FlowStep::Action::Fill) {
            params.set("text"sv, interpolate_flow_variables(*step.text, run.runtime_vars));
            params.set("clear"sv, step.clear);
        } else if (step.action == FlowStep::Action::AssertText) {
            if (step.contains.has_value())
                params.set("contains"sv, interpolate_flow_variables(*step.contains, run.runtime_vars));
            if (step.equals.has_value())
                params.set("equals"sv, interpolate_flow_variables(*step.equals, run.runtime_vars));
        }
        auto script = build_locator_action_script(flow_action_name(step.action), locator, params);
        execute_locator_step_attempt(run_id, idx, move(script), timeout_ms);
        return;
    }

    case FlowStep::Action::Press: {
        auto key = interpolate_flow_variables(*step.key, run.runtime_vars);
        if (step.locator.has_value()) {
            auto locator = interpolate_locator(*step.locator, run.runtime_vars);
            JsonObject params;
            params.set("key"sv, key);
            auto script = build_locator_action_script("press"sv, locator, params);
            execute_locator_step_attempt(run_id, idx, move(script), timeout_ms);
            return;
        }
        JsonObject cfg;
        cfg.set("key"sv, key);
        auto script = MUST(String::formatted(R"JS(return (function(cfg) {{
  var el = document.activeElement || document.body || document.documentElement;
  if (!el) return {{ ok: false, retryable: true, error: 'No active element' }};
  var init = {{ key: cfg.key, bubbles: true, cancelable: true }};
  var ok = el.dispatchEvent(new KeyboardEvent('keydown', init));
  if (cfg.key === 'Enter' || cfg.key.length === 1)
    el.dispatchEvent(new KeyboardEvent('keypress', init));
  el.dispatchEvent(new KeyboardEvent('keyup', init));
  if (cfg.key === 'Enter' && ok && el.form) {{
    if (typeof el.form.requestSubmit === 'function') el.form.requestSubmit();
    else el.form.submit();
  }}
  return {{ ok: true }};
}})({});)JS",
            JsonValue { move(cfg) }.serialized()));
        execute_locator_step_attempt(run_id, idx, move(script), timeout_ms);
        return;
    }

    case FlowStep::Action::Eval: {
        if (run.require_mcp_permission) {
            if (m_application.mcp_security_policy().disable_eval_js) {
                fail_run(run_id, "Flow eval step is disabled by --mcp-disable-eval-js policy"_string);
                return;
            }
            if (m_application.is_view_mcp_secret_tainted(run.tab_id)) {
                fail_run(run_id, "Blocked by credential vault quarantine: eval is disabled on this tab until navigation commits to a new document"_string);
                return;
            }
        }
        JsonObject vars_json;
        for (auto const& [k, v] : run.runtime_vars)
            vars_json.set(k, v);

        JsonArray args;
        args.must_append(move(vars_json));

        JsonObject payload;
        payload.set("script"sv, *step.script);
        payload.set("args"sv, move(args));

        m_application.enqueue_automation_webdriver_command(run.tab_id, "execute_script"_string, move(payload), {}, run.require_mcp_permission, [weak_self = make_weak_ptr<FlowRunner>(), run_id, idx](Web::WebDriver::Response response) {
            if (!weak_self)
                return;
            if (response.is_error()) {
                weak_self->fail_run(run_id, MUST(String::formatted("JS error ({}): {}", response.error().error, response.error().message)));
                return;
            }
            weak_self->complete_current_step(run_id, idx, response.value());
        });
        return;
    }

    case FlowStep::Action::Screenshot: {
        if (run.require_mcp_permission && m_application.is_view_mcp_secret_tainted(run.tab_id)) {
            fail_run(run_id, "Blocked by credential vault quarantine: screenshot is disabled on this tab until navigation commits to a new document"_string);
            return;
        }
        m_application.enqueue_automation_webdriver_command(run.tab_id, "take_screenshot"_string, JsonValue {}, {}, run.require_mcp_permission, [weak_self = make_weak_ptr<FlowRunner>(), run_id, idx](Web::WebDriver::Response response) {
            if (!weak_self)
                return;
            if (response.is_error()) {
                weak_self->fail_run(run_id, MUST(String::formatted("Screenshot error: {}", response.error().message)));
                return;
            }
            if (!response.value().is_string()) {
                weak_self->fail_run(run_id, "Screenshot response was not base64 string"_string);
                return;
            }
            auto decoded = decode_base64(response.value().as_string());
            if (decoded.is_error()) {
                weak_self->fail_run(run_id, MUST(String::formatted("Could not decode screenshot: {}", decoded.error())));
                return;
            }
            auto runs_dir = weak_self->m_store.runs_directory();
            if (auto dir_res = Core::Directory::create(runs_dir, Core::Directory::CreateDirectories::Yes); dir_res.is_error()) {
                weak_self->fail_run(run_id, MUST(String::formatted("Could not create runs dir: {}", dir_res.error())));
                return;
            }
            auto file_name = ByteString::formatted("{}_step_{}.png", run_id, idx);
            auto full_path = LexicalPath::join(runs_dir, file_name).string();
            auto file = Core::File::open(full_path, Core::File::OpenMode::Write);
            if (file.is_error()) {
                weak_self->fail_run(run_id, MUST(String::formatted("Could not open screenshot file: {}", file.error())));
                return;
            }
            if (auto write_res = file.value()->write_until_depleted(decoded.value()); write_res.is_error()) {
                weak_self->fail_run(run_id, MUST(String::formatted("Could not write screenshot file: {}", write_res.error())));
                return;
            }
            auto path_str = String::from_utf8_without_validation(full_path.bytes());
            weak_self->complete_current_step(run_id, idx, JsonValue { path_str }, path_str);
        });
        return;
    }

    case FlowStep::Action::UploadFile: {
        if (!step.locator.has_value() || step.paths.is_empty()) {
            fail_run(run_id, "Step 'upload_file' requires both 'locator' and 'paths'"_string);
            return;
        }
        auto upload_root = m_application.effective_upload_directory();
        Vector<String> validated_paths;
        validated_paths.ensure_capacity(step.paths.size());
        for (auto const& raw_path : step.paths) {
            auto interpolated = interpolate_flow_variables(raw_path, run.runtime_vars);
            auto validated = resolve_and_validate_upload_path(interpolated, upload_root);
            if (validated.is_error()) {
                fail_run(run_id, MUST(String::formatted("{}: {}", validated.error(), interpolated)));
                return;
            }
            validated_paths.unchecked_append(String::from_utf8_without_validation(validated.value().bytes()));
        }
        auto locator = interpolate_locator(*step.locator, run.runtime_vars);
        auto script = build_locator_action_script("resolve_upload_input"sv, locator, {});
        execute_upload_step_attempt(run_id, idx, move(script), move(validated_paths), timeout_ms);
        return;
    }
    }
}

void FlowRunner::execute_upload_step_attempt(String const& run_id, size_t step_index, String script, Vector<String> validated_paths, u32 timeout_ms)
{
    auto entry = m_active_runs.get(run_id);
    if (!entry.has_value())
        return;
    auto& run = *entry.value();
    if (run.finished || run.step_index != step_index)
        return;

    JsonObject payload;
    payload.set("script"sv, script);
    payload.set("args"sv, JsonArray {});

    m_application.enqueue_automation_webdriver_command(run.tab_id, "execute_script"_string, move(payload), {}, run.require_mcp_permission, [weak_self = make_weak_ptr<FlowRunner>(), run_id, step_index, script = move(script), validated_paths = move(validated_paths), timeout_ms](Web::WebDriver::Response response) mutable {
        if (!weak_self)
            return;
        auto active = weak_self->m_active_runs.get(run_id);
        if (!active.has_value())
            return;
        auto& run = *active.value();
        if (run.finished || run.step_index != step_index)
            return;

        if (response.is_error()) {
            weak_self->fail_run(run_id, MUST(String::formatted("WebDriver error ({}): {}", response.error().error, response.error().message)));
            return;
        }

        auto const& val = response.value();
        if (!val.is_object()) {
            weak_self->fail_run(run_id, "Unexpected script result while resolving upload input"_string);
            return;
        }

        auto const& obj = val.as_object();
        if (obj.get_bool("ok"sv).value_or(false)) {
            Optional<String> element_id;
            if (auto el_val = obj.get("element"sv); el_val.has_value())
                element_id = Application::extract_webdriver_element_id(*el_val);
            if (!element_id.has_value())
                element_id = Application::extract_webdriver_element_id(val);
            if (!element_id.has_value()) {
                weak_self->fail_run(run_id, "Resolved upload element response missing element handle"_string);
                return;
            }

            StringBuilder joined;
            JsonArray paths_array;
            for (auto const& path : validated_paths) {
                if (!joined.is_empty())
                    joined.append('\n');
                joined.append(path);
                paths_array.must_append(path);
            }

            JsonObject send_payload;
            send_payload.set("text"sv, MUST(joined.to_string()));
            Vector<String> args;
            args.append(*element_id);

            auto file_count = validated_paths.size();
            weak_self->m_application.enqueue_automation_webdriver_command(run.tab_id, "element_send_keys"_string, move(send_payload), move(args), run.require_mcp_permission, [weak_self, run_id, step_index, file_count, paths_array = move(paths_array)](Web::WebDriver::Response send_resp) mutable {
                if (!weak_self)
                    return;
                if (send_resp.is_error()) {
                    weak_self->fail_run(run_id, MUST(String::formatted("Upload failed ({}): {}", send_resp.error().error, send_resp.error().message)));
                    return;
                }
                JsonObject output;
                output.set("uploaded"sv, true);
                output.set("file_count"sv, static_cast<u64>(file_count));
                output.set("paths"sv, move(paths_array));
                weak_self->complete_current_step(run_id, step_index, JsonValue { move(output) });
            });
            return;
        }

        bool retryable = obj.get_bool("retryable"sv).value_or(false);
        auto err_msg = obj.get_string("error"sv).value_or("Upload target check failed"_string);
        auto elapsed_ms = (MonotonicTime::now() - run.step_started_at).to_milliseconds();
        if (!retryable || elapsed_ms >= timeout_ms) {
            weak_self->fail_run(run_id, MUST(String::formatted("Step timed out or failed ({} ms): {}", elapsed_ms, err_msg)));
            return;
        }

        run.step_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(step_poll_interval_ms), [weak_self, run_id, step_index, script = move(script), validated_paths = move(validated_paths), timeout_ms]() mutable {
            if (weak_self)
                weak_self->execute_upload_step_attempt(run_id, step_index, move(script), move(validated_paths), timeout_ms);
        });
        run.step_timer->start();
    });
}

void FlowRunner::execute_locator_step_attempt(String const& run_id, size_t step_index, String script, u32 timeout_ms)
{
    auto entry = m_active_runs.get(run_id);
    if (!entry.has_value())
        return;
    auto& run = *entry.value();
    if (run.finished || run.step_index != step_index)
        return;

    JsonObject payload;
    payload.set("script"sv, script);
    payload.set("args"sv, JsonArray {});

    m_application.enqueue_automation_webdriver_command(run.tab_id, "execute_script"_string, move(payload), {}, run.require_mcp_permission, [weak_self = make_weak_ptr<FlowRunner>(), run_id, step_index, script = move(script), timeout_ms](Web::WebDriver::Response response) mutable {
        if (!weak_self)
            return;
        auto active = weak_self->m_active_runs.get(run_id);
        if (!active.has_value())
            return;
        auto& run = *active.value();
        if (run.finished || run.step_index != step_index)
            return;

        if (response.is_error()) {
            weak_self->fail_run(run_id, MUST(String::formatted("WebDriver error ({}): {}", response.error().error, response.error().message)));
            return;
        }

        auto const& val = response.value();
        if (!val.is_object()) {
            weak_self->fail_run(run_id, "Unexpected script result while executing step"_string);
            return;
        }

        auto const& obj = val.as_object();
        if (obj.get_bool("ok"sv).value_or(false)) {
            if (auto text = obj.get_string("text"sv); text.has_value())
                weak_self->complete_current_step(run_id, step_index, JsonValue { *text });
            else
                weak_self->complete_current_step(run_id, step_index);
            return;
        }

        bool retryable = obj.get_bool("retryable"sv).value_or(false);
        auto err_msg = obj.get_string("error"sv).value_or("Locator check failed"_string);
        auto elapsed_ms = (MonotonicTime::now() - run.step_started_at).to_milliseconds();
        if (!retryable || elapsed_ms >= timeout_ms) {
            weak_self->fail_run(run_id, MUST(String::formatted("Step timed out or failed ({} ms): {}", elapsed_ms, err_msg)));
            return;
        }

        run.step_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(step_poll_interval_ms), [weak_self, run_id, step_index, script = move(script), timeout_ms]() mutable {
            if (weak_self)
                weak_self->execute_locator_step_attempt(run_id, step_index, move(script), timeout_ms);
        });
        run.step_timer->start();
    });
}

void FlowRunner::poll_assert_url(String const& run_id, size_t step_index, Optional<String> contains, Optional<String> equals, u32 timeout_ms)
{
    auto entry = m_active_runs.get(run_id);
    if (!entry.has_value())
        return;
    auto& run = *entry.value();
    if (run.finished || run.step_index != step_index)
        return;

    auto view = m_application.find_automation_view(run.tab_id, run.require_mcp_permission);
    if (!view.has_value()) {
        fail_run(run_id, "Target tab disappeared or permission was revoked"_string);
        return;
    }

    auto current_url = view->url().to_string();
    bool ok = true;
    if (equals.has_value() && current_url != *equals)
        ok = false;
    if (contains.has_value() && !current_url.contains(*contains))
        ok = false;

    if (ok) {
        complete_current_step(run_id, step_index, JsonValue { current_url });
        return;
    }

    auto elapsed_ms = (MonotonicTime::now() - run.step_started_at).to_milliseconds();
    if (elapsed_ms >= timeout_ms) {
        fail_run(run_id, MUST(String::formatted("URL assertion timed out (current URL: {})", current_url)));
        return;
    }

    run.step_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(step_poll_interval_ms), [weak_self = make_weak_ptr<FlowRunner>(), run_id, step_index, contains = move(contains), equals = move(equals), timeout_ms]() mutable {
        if (weak_self)
            weak_self->poll_assert_url(run_id, step_index, move(contains), move(equals), timeout_ms);
    });
    run.step_timer->start();
}

void FlowRunner::complete_current_step(String const& run_id, size_t step_index, JsonValue output, Optional<String> screenshot_path)
{
    auto entry = m_active_runs.get(run_id);
    if (!entry.has_value())
        return;
    auto& run = *entry.value();
    if (run.finished || run.step_index != step_index)
        return;

    cleanup_run_timers(run);

    auto const& step_def = run.flow.steps[step_index];
    auto& step_res = run.record.steps[step_index];
    step_res.status = "passed"_string;
    step_res.duration_ms = (MonotonicTime::now() - run.step_started_at).to_milliseconds();
    if (output.is_string()) {
        step_res.output = JsonValue { m_application.scrub_mcp_secrets(output.as_string()) };
    } else if (!output.is_null()) {
        auto serialized = output.serialized();
        auto scrubbed = m_application.scrub_mcp_secrets(serialized);
        if (auto reparsed = JsonValue::from_string(scrubbed); !reparsed.is_error())
            step_res.output = reparsed.release_value();
        else
            step_res.output = JsonValue { move(scrubbed) };
    } else {
        step_res.output = move(output);
    }
    step_res.screenshot_path = move(screenshot_path);

    if (step_def.action == FlowStep::Action::Extract
        || ((step_def.action == FlowStep::Action::Eval || step_def.action == FlowStep::Action::Screenshot || step_def.action == FlowStep::Action::UploadFile) && step_def.save_as.has_value())) {
        auto key = step_def.save_as.value_or("extracted"_string);
        run.record.outputs.set(key, step_res.output);
        if (step_res.output.is_string())
            run.runtime_vars.set(key, step_res.output.as_string());
        else if (step_res.output.is_number() || step_res.output.is_bool())
            run.runtime_vars.set(key, step_res.output.serialized());
    }

    run.step_index += 1;
    m_store.upsert_run(run.record, false);

    Core::deferred_invoke([weak_self = make_weak_ptr<FlowRunner>(), run_id] {
        if (weak_self)
            weak_self->run_next_step(run_id);
    });
}

void FlowRunner::fail_run(String const& run_id, String error, StringView terminal_status)
{
    auto taken = m_active_runs.take(run_id);
    if (!taken.has_value())
        return;
    auto& run = *taken.value();
    run.finished = true;
    cleanup_run_timers(run);

    error = m_application.scrub_mcp_secrets(error);

    if (run.step_index < run.record.steps.size() && run.record.steps[run.step_index].status == "running"sv) {
        run.record.steps[run.step_index].status = String::from_utf8_without_validation(terminal_status.bytes());
        run.record.steps[run.step_index].duration_ms = (MonotonicTime::now() - run.step_started_at).to_milliseconds();
        run.record.steps[run.step_index].error = error;
    }

    auto now_ms = UnixDateTime::now().milliseconds_since_epoch();
    run.record.status = String::from_utf8_without_validation(terminal_status.bytes());
    run.record.finished_at_ms = now_ms;
    run.record.duration_ms = now_ms - run.record.started_at_ms;
    run.record.error = move(error);

    auto final_record = run.record;
    m_store.upsert_run(final_record, true);
    if (run.on_complete)
        run.on_complete(final_record);
    maybe_close_run_tab(run);
}

void FlowRunner::finish_run_success(String const& run_id)
{
    auto taken = m_active_runs.take(run_id);
    if (!taken.has_value())
        return;
    auto& run = *taken.value();
    run.finished = true;
    cleanup_run_timers(run);

    auto now_ms = UnixDateTime::now().milliseconds_since_epoch();
    run.record.status = "passed"_string;
    run.record.finished_at_ms = now_ms;
    run.record.duration_ms = now_ms - run.record.started_at_ms;

    auto final_record = run.record;
    m_store.upsert_run(final_record, true);
    if (run.on_complete)
        run.on_complete(final_record);
    maybe_close_run_tab(run);
}

void FlowRunner::maybe_close_run_tab(ActiveRun const& run)
{
    if (!run.opened_tab || !run.close_tab_on_finish)
        return;
    Core::deferred_invoke([weak_self = make_weak_ptr<FlowRunner>(), tab_id = run.tab_id, require_mcp = run.require_mcp_permission] {
        if (!weak_self)
            return;
        if (auto view = weak_self->m_application.find_automation_view(tab_id, require_mcp); view.has_value())
            view->force_close();
    });
}

}
