/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibCore/Timer.h>
#include <LibWebView/Automation/FlowStore.h>
#include <LibWebView/Automation/FlowTypes.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API FlowRunRequest {
    Optional<String> flow_id;
    Optional<FlowDefinition> inline_flow;
    Optional<u64> tab_id;
    HashMap<String, String> variables;
    bool require_mcp_permission { false };
    bool open_new_tab { true };
    bool close_tab_on_finish { false };
};

class WEBVIEW_API FlowRunner final : public Weakable<FlowRunner> {
    AK_MAKE_NONCOPYABLE(FlowRunner);

public:
    AK_ALLOC_WITH_KMALLOC;

    FlowRunner(Application&, FlowStore&);
    ~FlowRunner();

    ErrorOr<String> start_run(FlowRunRequest request, Function<void(FlowRunRecord const&)> on_complete = nullptr);
    bool cancel_run(StringView run_id);
    void cancel_all_runs(StringView reason = "Cancelled"sv);
    void did_close_view(u64 view_id);

private:
    struct ActiveRun {
        AK_ALLOC_WITH_KMALLOC;

        String run_id;
        FlowDefinition flow;
        HashMap<String, String> runtime_vars;
        FlowRunRecord record;
        u64 tab_id { 0 };
        bool require_mcp_permission { false };
        bool opened_tab { false };
        bool close_tab_on_finish { false };
        size_t step_index { 0 };
        MonotonicTime step_started_at;
        RefPtr<Core::Timer> step_timer;
        Optional<u64> nav_wait_id;
        Function<void(FlowRunRecord const&)> on_complete;
        bool finished { false };
    };

    void run_next_step(String const& run_id);
    void execute_locator_step_attempt(String const& run_id, size_t step_index, String script, u32 timeout_ms);
    void poll_assert_url(String const& run_id, size_t step_index, Optional<String> contains, Optional<String> equals, u32 timeout_ms);
    void complete_current_step(String const& run_id, size_t step_index, JsonValue output = {}, Optional<String> screenshot_path = {});
    void fail_run(String const& run_id, String error, StringView terminal_status = "failed"sv);
    void finish_run_success(String const& run_id);
    void maybe_close_run_tab(ActiveRun const&);
    void cleanup_run_timers(ActiveRun&);

    Application& m_application;
    FlowStore& m_store;
    HashMap<String, NonnullOwnPtr<ActiveRun>> m_active_runs;
};

}
