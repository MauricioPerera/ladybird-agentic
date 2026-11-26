/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibMCP/Forward.h>

namespace MCP {

struct TabInfo {
    u64 id { 0 };
    String title;
    String url;
    bool is_loading { false };
    bool is_active { false };
};

struct ToolCallResult {
    bool is_error { false };
    String text;
    Optional<String> image_base64_png {};
};

using OnToolCallComplete = Function<void(ToolCallResult)>;

class MCP_API MCPDelegate {
public:
    virtual ~MCPDelegate() = default;

    virtual Vector<TabInfo> list_allowed_tabs() const = 0;
    virtual Optional<TabInfo> active_allowed_tab() const = 0;

    virtual void open_tab(Optional<String> url, bool activate, bool wait_for_load, OnToolCallComplete) = 0;
    virtual void switch_tab(u64 tab_id, OnToolCallComplete) = 0;
    virtual void close_tab(u64 tab_id, OnToolCallComplete) = 0;
    virtual void navigate_tab(u64 tab_id, String url, bool wait_for_load, OnToolCallComplete) = 0;
    virtual void reload_tab(u64 tab_id, bool wait_for_load, OnToolCallComplete) = 0;
    virtual void go_back(u64 tab_id, OnToolCallComplete) = 0;
    virtual void go_forward(u64 tab_id, OnToolCallComplete) = 0;
    virtual void get_accessibility_tree(u64 tab_id, OnToolCallComplete) = 0;
    virtual void get_dom_tree(u64 tab_id, OnToolCallComplete) = 0;
    virtual void get_page_source(u64 tab_id, OnToolCallComplete) = 0;
    virtual void find_elements(u64 tab_id, String selector, String strategy, OnToolCallComplete) = 0;
    virtual void click_element(u64 tab_id, Optional<String> ref, Optional<String> selector, Optional<String> element_id, String strategy, OnToolCallComplete) = 0;
    virtual void type_text(u64 tab_id, Optional<String> ref, Optional<String> selector, Optional<String> element_id, String text, bool clear_first, String strategy, OnToolCallComplete) = 0;
    virtual void get_page_snapshot(u64 tab_id, size_t max_elements, bool include_text, OnToolCallComplete) = 0;
    virtual void press_key(u64 tab_id, String key, Optional<String> ref, Optional<String> selector, OnToolCallComplete) = 0;
    virtual void scroll_page(u64 tab_id, String direction, i32 amount, Optional<String> ref, Optional<String> selector, OnToolCallComplete) = 0;
    virtual void hover_element(u64 tab_id, Optional<String> ref, Optional<String> selector, OnToolCallComplete) = 0;
    virtual void select_option(u64 tab_id, Optional<String> ref, Optional<String> selector, Optional<String> value, Optional<String> label, Optional<size_t> index, OnToolCallComplete) = 0;
    virtual void wait_for(u64 tab_id, Optional<String> ref, Optional<String> selector, Optional<String> text, u32 timeout_ms, OnToolCallComplete) = 0;
    virtual void get_console_logs(u64 tab_id, size_t limit, bool clear, OnToolCallComplete) = 0;
    virtual void get_network_requests(u64 tab_id, size_t limit, bool clear, bool only_errors, OnToolCallComplete) = 0;
    virtual void evaluate_javascript(u64 tab_id, String script, OnToolCallComplete) = 0;
    virtual void take_screenshot(u64 tab_id, OnToolCallComplete) = 0;

    virtual void list_flows(OnToolCallComplete) = 0;
    virtual void get_flow(String flow_id, OnToolCallComplete) = 0;
    virtual void save_flow(JsonValue flow_json, OnToolCallComplete) = 0;
    virtual void delete_flow(String flow_id, OnToolCallComplete) = 0;
    virtual void run_flow(Optional<String> flow_id, Optional<JsonValue> inline_flow, Optional<u64> tab_id, JsonObject variables, bool open_new_tab, bool wait, bool close_tab_on_finish, OnToolCallComplete) = 0;
    virtual void list_flow_runs(Optional<String> flow_id, size_t limit, OnToolCallComplete) = 0;
    virtual void get_flow_run(String run_id, OnToolCallComplete) = 0;
    virtual void cancel_flow_run(String run_id, OnToolCallComplete) = 0;

    virtual void list_extensions(OnToolCallComplete) = 0;
    virtual void install_unpacked_extension(String path, OnToolCallComplete) = 0;
    virtual void set_extension_enabled(String extension_id, bool enabled, OnToolCallComplete) = 0;
    virtual void reload_extension(String extension_id, OnToolCallComplete) = 0;
    virtual void remove_extension(String extension_id, OnToolCallComplete) = 0;
};

}
