/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibWebView/Application.h>
#include <LibWebView/Automation/FlowRunner.h>
#include <LibWebView/Automation/FlowTypes.h>
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
        send_state();
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

void FlowsUI::send_state()
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
        if (view.owner_view_id().has_value())
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

    JsonObject state;
    state.set("flows"sv, move(flows_json));
    state.set("runs"sv, move(runs_json));
    state.set("tabs"sv, move(tabs_json));
    state.set("mcp"sv, move(mcp_json));
    state.set("storageDirectory"sv, String::from_utf8_without_validation(store.storage_directory().bytes()));

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

    auto saved = Application::the().flow_store().save_flow(parsed.release_value());
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

}
