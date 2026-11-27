/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibWebView/Application.h>
#include <LibWebView/MCPContentShield.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebUI/AgentUI.h>

namespace WebView {

static StringView access_mode_key(MCPAccessMode mode)
{
    switch (mode) {
    case MCPAccessMode::Blocked:
        return "blocked"sv;
    case MCPAccessMode::ReadOnly:
        return "read_only"sv;
    case MCPAccessMode::SameOrigin:
        return "same_origin"sv;
    case MCPAccessMode::Full:
        return "full"sv;
    }
    VERIFY_NOT_REACHED();
}

static Optional<MCPAccessMode> parse_access_mode(StringView mode)
{
    if (mode == "blocked"sv)
        return MCPAccessMode::Blocked;
    if (mode == "read_only"sv)
        return MCPAccessMode::ReadOnly;
    if (mode == "same_origin"sv)
        return MCPAccessMode::SameOrigin;
    if (mode == "full"sv)
        return MCPAccessMode::Full;
    return {};
}

AgentUI::~AgentUI()
{
    if (m_observing && Application::is_initialized())
        Application::the().remove_mcp_audit_observer(*this);
}

void AgentUI::register_interfaces()
{
    if (Application::is_initialized() && !m_observing) {
        Application::the().add_mcp_audit_observer(*this);
        m_observing = true;
    }

    register_interface("loadState"sv, [this](auto const&) {
        send_state();
    });
    register_interface("setPaused"sv, [this](auto const& data) {
        set_paused(data);
    });
    register_interface("setTabMode"sv, [this](auto const& data) {
        set_tab_mode(data);
    });
    register_interface("clearEgressTaint"sv, [this](auto const&) {
        clear_egress_taint();
    });
    register_interface("clearAuditLog"sv, [this](auto const&) {
        clear_audit_log();
    });
}

void AgentUI::mcp_state_changed()
{
    if (has_client())
        send_state();
}

void AgentUI::set_paused(JsonValue const& data)
{
    if (!data.is_object() || !Application::is_initialized())
        return;
    auto paused = data.as_object().get_bool("paused"sv);
    if (!paused.has_value())
        return;
    Application::the().set_mcp_paused(*paused);
}

void AgentUI::set_tab_mode(JsonValue const& data)
{
    if (!data.is_object())
        return;
    auto const& obj = data.as_object();
    auto tab_id = obj.get_u64("tabId"sv);
    auto mode_str = obj.get_string("mode"sv);
    if (!tab_id.has_value() || !mode_str.has_value())
        return;
    auto mode = parse_access_mode(*mode_str);
    if (!mode.has_value())
        return;
    if (auto view = ViewImplementation::find_view_by_id(*tab_id); view.has_value()) {
        view->set_mcp_access_mode(*mode);
        if (has_client())
            send_state();
    }
}

void AgentUI::clear_egress_taint()
{
    if (Application::is_initialized())
        Application::the().clear_mcp_egress_taint();
}

void AgentUI::clear_audit_log()
{
    if (Application::is_initialized())
        Application::the().clear_mcp_audit_log();
}

void AgentUI::send_state()
{
    auto& app = Application::the();
    auto const& opts = Application::browser_options();

    JsonObject state;
    state.set("mcpEnabled"sv, app.mcp_enabled());
    if (auto port = app.mcp_port(); port.has_value())
        state.set("mcpPort"sv, static_cast<u64>(*port));
    state.set("mcpPaused"sv, app.mcp_paused());
    state.set("evalJsDisabled"sv, opts.mcp_disable_eval_js);
    state.set("autoApprove"sv, opts.mcp_auto_approve);
    state.set("allowExtensionInstall"sv, opts.mcp_allow_extension_install);
    state.set("allowSkillWrite"sv, opts.mcp_allow_skill_write);

    JsonObject egress;
    egress.set("tainted"sv, app.mcp_egress_tainted());
    JsonArray src_origins;
    for (auto const& o : app.mcp_egress_source_origins())
        src_origins.must_append(o);
    egress.set("sourceOrigins"sv, move(src_origins));
    JsonArray allowed_origins;
    for (auto const& o : app.mcp_egress_allowed_origins())
        allowed_origins.must_append(o);
    egress.set("allowedOrigins"sv, move(allowed_origins));
    state.set("egress"sv, move(egress));

    auto const& st = app.mcp_audit_stats();
    JsonObject stats;
    stats.set("totalCalls"sv, st.total_calls);
    stats.set("blockedCalls"sv, st.blocked_calls);
    stats.set("approvalsRequested"sv, st.approvals_requested);
    stats.set("approvalsDenied"sv, st.approvals_denied);
    stats.set("strippedInvisibleChars"sv, st.stripped_invisible_chars);
    stats.set("strippedHtmlComments"sv, st.stripped_html_comments);
    stats.set("flaggedInjections"sv, st.flagged_injections);
    state.set("stats"sv, move(stats));

    JsonArray tabs_json;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (view.owner_view_id().has_value() || view.is_private() == IsPrivate::Yes)
            return IterationDecision::Continue;
        if (!view.url().scheme().is_one_of("http"sv, "https"sv) && view.url() != URL::about_blank())
            return IterationDecision::Continue;

        JsonObject tab;
        tab.set("id"sv, view.view_id());
        tab.set("title"sv, sanitize_mcp_untrusted_text(view.title().to_well_formed_utf8()).text);
        tab.set("url"sv, view.url().to_string());
        tab.set("origin"sv, view.url().origin().serialize());
        tab.set("containerId"sv, view.container_id());
        tab.set("mcpAccessMode"sv, access_mode_key(view.mcp_access_mode()));
        if (auto const& pinned = view.mcp_pinned_origin(); pinned.has_value())
            tab.set("pinnedOrigin"sv, pinned->serialize());
        tab.set("secretQuarantined"sv, app.is_view_mcp_secret_tainted(view.view_id()));
        tabs_json.must_append(move(tab));
        return IterationDecision::Continue;
    });
    state.set("tabs"sv, move(tabs_json));

    JsonArray audit_json;
    auto const& entries = app.mcp_audit_entries();
    size_t count = min(entries.size(), static_cast<size_t>(100));
    for (size_t i = 0; i < count; ++i) {
        auto const& entry = entries[entries.size() - 1 - i];
        JsonObject obj;
        obj.set("timestampMs"sv, entry.timestamp_ms);
        obj.set("toolName"sv, entry.tool_name);
        if (entry.tab_id.has_value())
            obj.set("tabId"sv, *entry.tab_id);
        obj.set("origin"sv, entry.origin);
        obj.set("containerId"sv, entry.container_id);
        obj.set("outcome"sv, entry.outcome);
        obj.set("detail"sv, entry.detail);
        obj.set("strippedInvisibleChars"sv, entry.stripped_invisible_chars);
        obj.set("strippedHtmlComments"sv, entry.stripped_html_comments);
        JsonArray signals;
        for (auto const& s : entry.risk_signals)
            signals.must_append(s);
        obj.set("riskSignals"sv, move(signals));
        audit_json.must_append(move(obj));
    }
    state.set("audit"sv, move(audit_json));

    async_send_message("state"_string, move(state));
}

}
