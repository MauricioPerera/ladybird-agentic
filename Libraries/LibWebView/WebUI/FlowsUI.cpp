/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Application.h>
#include <LibWebView/Automation/FlowRunner.h>
#include <LibWebView/Automation/FlowTypes.h>
#include <LibWebView/Settings.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebUI/FlowsUI.h>

namespace WebView {

FlowsUI::~FlowsUI()
{
    if (m_store)
        m_store->remove_observer(*this);
}

void FlowsUI::register_interfaces()
{
    auto& store = Application::the().flow_store();
    store.add_observer(*this);
    m_store = store.make_weak_ptr();

    register_interface("loadState"sv, [this](auto const&) {
        send_state(true);
    });
    register_interface("saveFlow"sv, [this](auto const& data) {
        save_flow(data);
    });
    register_interface("deleteFlow"sv, [this](auto const& data) {
        delete_flow(data);
    });
    register_interface("createExampleFlow"sv, [this](auto const&) {
        create_example_flow();
    });
    register_interface("runFlow"sv, [this](auto const& data) {
        run_flow(data);
    });
    register_interface("cancelRun"sv, [this](auto const& data) {
        cancel_run(data);
    });
    register_interface("setUploadDirectory"sv, [this](auto const& data) {
        set_upload_directory(data);
    });
    register_interface("openUploadDirectory"sv, [this](auto const&) {
        open_upload_directory();
    });
}

void FlowsUI::flows_changed()
{
    if (has_client())
        send_state();
}

void FlowsUI::flow_runs_changed()
{
    if (has_client())
        send_state();
}

void FlowsUI::upload_directory_changed()
{
    m_has_cached_upload_files = false;
    if (has_client())
        send_state(true);
}

void FlowsUI::refresh_upload_files_cache()
{
    auto upload_dir = Application::the().effective_upload_directory();
    struct UploadFileCandidate {
        String name;
        u64 size_bytes { 0 };
    };
    Vector<UploadFileCandidate> upload_candidates;
    if (FileSystem::is_directory(upload_dir)) {
        (void)Core::Directory::for_each_entry(upload_dir, Core::DirIterator::SkipParentAndBaseDir, [&](Core::DirectoryEntry const& entry, Core::Directory const&) -> ErrorOr<IterationDecision> {
            if (entry.type != Core::DirectoryEntry::Type::File)
                return IterationDecision::Continue;
            if (entry.name.starts_with('.'))
                return IterationDecision::Continue;
            auto full_path = LexicalPath::join(upload_dir, entry.name).string();
            if (FileSystem::is_link(full_path))
                return IterationDecision::Continue;
            auto name_str = String::from_utf8(entry.name.view());
            if (name_str.is_error())
                return IterationDecision::Continue;
            u64 size_bytes = 0;
            if (auto file_or = Core::File::open(full_path, Core::File::OpenMode::Read); !file_or.is_error()) {
                if (auto sz = file_or.value()->size(); !sz.is_error())
                    size_bytes = sz.value();
            }
            upload_candidates.append({ name_str.release_value(), size_bytes });
            if (upload_candidates.size() >= 200)
                return IterationDecision::Break;
            return IterationDecision::Continue;
        });
    }
    quick_sort(upload_candidates, [](UploadFileCandidate const& a, UploadFileCandidate const& b) {
        return a.name < b.name;
    });
    JsonArray upload_files_json;
    for (auto const& candidate : upload_candidates) {
        JsonObject f;
        f.set("name"sv, candidate.name);
        f.set("sizeBytes"sv, candidate.size_bytes);
        upload_files_json.must_append(move(f));
    }
    m_cached_upload_files = move(upload_files_json);
    m_has_cached_upload_files = true;
}

void FlowsUI::send_state(bool refresh_upload_files)
{
    auto& app = Application::the();
    auto& store = app.flow_store();

    JsonArray flows_json;
    for (auto const& flow : store.list_flows())
        flows_json.must_append(serialize_flow_definition(flow));

    JsonArray runs_json;
    for (auto const& run : store.list_runs({}, 20))
        runs_json.must_append(serialize_flow_run(run));

    JsonArray tabs_json;
    auto active = app.active_web_view();
    Optional<u64> active_id = active.has_value() ? Optional<u64> { active->view_id() } : OptionalNone {};
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (view.owner_view_id().has_value() || view.is_private() == IsPrivate::Yes)
            return IterationDecision::Continue;
        if (view.url().scheme() == "about"sv && view.url() != URL::about_blank())
            return IterationDecision::Continue;

        JsonObject tab;
        tab.set("id"sv, view.view_id());
        tab.set("title"sv, view.title().to_well_formed_utf8());
        tab.set("url"sv, view.url().to_string());
        tab.set("isActive"sv, active_id.has_value() && *active_id == view.view_id());
        tab.set("mcpAllowed"sv, view.is_mcp_access_allowed());
        tabs_json.must_append(move(tab));
        return IterationDecision::Continue;
    });

    JsonObject mcp_json;
    bool mcp_enabled = app.mcp_enabled();
    mcp_json.set("enabled"sv, mcp_enabled);
    if (auto port = app.mcp_port(); port.has_value()) {
        mcp_json.set("port"sv, static_cast<u64>(*port));
        mcp_json.set("endpoint"sv, MUST(String::formatted("http://127.0.0.1:{}/mcp", *port)));
    }

    auto upload_dir = app.effective_upload_directory();
    if (refresh_upload_files || !m_has_cached_upload_files)
        refresh_upload_files_cache();
    auto upload_files_json = m_cached_upload_files;

    JsonObject state;
    state.set("flows"sv, move(flows_json));
    state.set("runs"sv, move(runs_json));
    state.set("tabs"sv, move(tabs_json));
    state.set("mcp"sv, move(mcp_json));
    state.set("storageDirectory"sv, String::from_utf8_without_validation(store.storage_directory().bytes()));
    state.set("uploadDirectory"sv, String::from_utf8_without_validation(upload_dir.bytes()));
    state.set("defaultUploadDirectory"sv, Settings::default_upload_directory());
    state.set("uploadDirectoryCustom"sv, Application::settings().upload_directory().value_or(String {}));
    state.set("uploadDirectoryOverriddenByCli"sv, app.upload_directory_overridden_by_cli());
    state.set("uploadFiles"sv, move(upload_files_json));

    async_send_message("stateUpdated"sv, move(state));
}

void FlowsUI::send_action_result(bool ok, Optional<String> message, Optional<String> flow_id, Optional<String> run_id)
{
    JsonObject result;
    result.set("ok"sv, ok);
    if (message.has_value())
        result.set("message"sv, *message);
    if (flow_id.has_value())
        result.set("flowId"sv, *flow_id);
    if (run_id.has_value())
        result.set("runId"sv, *run_id);
    async_send_message("actionResult"sv, move(result));
}

void FlowsUI::save_flow(JsonValue const& data)
{
    auto parsed = parse_flow_definition(data);
    if (parsed.is_error()) {
        send_action_result(false, MUST(String::formatted("Invalid flow: {}", parsed.error())));
        return;
    }

    auto saved = Application::the().flow_store().save_flow_from_ui(parsed.release_value());
    if (saved.is_error()) {
        send_action_result(false, MUST(String::formatted("Failed to save flow: {}", saved.error())));
        return;
    }

    send_action_result(true, "Flow saved."_string, saved.value().id);
}

void FlowsUI::delete_flow(JsonValue const& data)
{
    if (!data.is_string()) {
        send_action_result(false, "Expected flow id string"_string);
        return;
    }

    auto id = data.as_string();
    if (auto res = Application::the().flow_store().delete_flow(id); res.is_error()) {
        send_action_result(false, MUST(String::formatted("Failed to delete flow: {}", res.error())));
        return;
    }

    send_action_result(true, "Flow deleted."_string);
}

void FlowsUI::create_example_flow()
{
    auto& store = Application::the().flow_store();
    auto flow = make_example_flow();
    flow.source = "user"_string;

    if (store.get_flow(flow.id).has_value()) {
        for (size_t suffix = 2; suffix < 100; ++suffix) {
            auto candidate = MUST(String::formatted("example-smoke-{}", suffix));
            if (!store.get_flow(candidate).has_value()) {
                flow.id = move(candidate);
                flow.name = MUST(String::formatted("Example Domain Smoke Check #{}", suffix));
                break;
            }
        }
    }

    auto saved = store.save_flow(move(flow));
    if (saved.is_error()) {
        send_action_result(false, MUST(String::formatted("Could not create example flow: {}", saved.error())));
        return;
    }

    send_action_result(true, "Example flow created."_string, saved.value().id);
}

void FlowsUI::run_flow(JsonValue const& data)
{
    if (!data.is_object()) {
        send_action_result(false, "Expected object payload for runFlow"_string);
        return;
    }
    auto const& obj = data.as_object();

    FlowRunRequest request;
    request.require_mcp_permission = false;

    if (auto flow_id = obj.get_string("flowId"sv); flow_id.has_value() && !flow_id->is_empty())
        request.flow_id = *flow_id;

    if (auto inline_flow = obj.get("flow"sv); inline_flow.has_value() && inline_flow->is_object()) {
        auto parsed = parse_flow_definition(*inline_flow);
        if (parsed.is_error()) {
            send_action_result(false, MUST(String::formatted("Invalid flow: {}", parsed.error())));
            return;
        }
        request.inline_flow = parsed.release_value();
    }

    if (auto tab_id = obj.get_u64("tabId"sv); tab_id.has_value() && *tab_id > 0)
        request.tab_id = *tab_id;
    else
        request.open_new_tab = true;

    if (auto vars = obj.get_object("variables"sv); vars.has_value()) {
        vars->for_each_member([&](String const& key, JsonValue const& val) {
            if (val.is_string())
                request.variables.set(key, val.as_string());
            else if (val.is_number() || val.is_bool())
                request.variables.set(key, val.serialized());
        });
    }

    auto flow_id_for_ui = request.inline_flow.has_value() ? Optional<String> { request.inline_flow->id } : request.flow_id;
    auto started = Application::the().flow_runner().start_run(move(request));
    if (started.is_error()) {
        send_action_result(false, MUST(String::formatted("Could not start flow: {}", started.error())), flow_id_for_ui);
        return;
    }

    send_action_result(true, "Flow started."_string, flow_id_for_ui, started.value());
}

void FlowsUI::cancel_run(JsonValue const& data)
{
    if (!data.is_string())
        return;
    bool cancelled = Application::the().flow_runner().cancel_run(data.as_string());
    if (!cancelled)
        send_action_result(false, "Run is no longer active."_string);
}

void FlowsUI::set_upload_directory(JsonValue const& data)
{
    if (!data.is_string()) {
        send_action_result(false, "Expected upload directory path string"_string);
        return;
    }

    auto raw = data.as_string();
    auto trimmed = raw.bytes_as_string_view().trim_whitespace();
    Optional<String> next_value;
    if (!trimmed.is_empty()) {
        auto validated = Settings::validate_upload_directory(trimmed);
        if (validated.is_error()) {
            send_action_result(false, MUST(String::formatted("Invalid upload directory: {}", validated.error())));
            return;
        }
        if (auto dir_res = Core::Directory::create(validated.value().to_byte_string(), Core::Directory::CreateDirectories::Yes); dir_res.is_error()) {
            send_action_result(false, MUST(String::formatted("Could not create upload directory: {}", dir_res.error())));
            return;
        }
        next_value = validated.release_value();
    }

    if (auto res = Application::settings().set_upload_directory(move(next_value)); res.is_error()) {
        send_action_result(false, MUST(String::formatted("Failed to save upload directory: {}", res.error())));
        return;
    }

    auto effective = Application::the().effective_upload_directory();
    send_action_result(true, MUST(String::formatted("Upload directory set to {}", effective)));
}

void FlowsUI::open_upload_directory()
{
    if (auto res = Application::the().open_upload_directory(); res.is_error()) {
        send_action_result(false, MUST(String::formatted("Could not open upload directory: {}", res.error())));
        return;
    }
    send_action_result(true, "Opened upload directory."_string);
}

}
