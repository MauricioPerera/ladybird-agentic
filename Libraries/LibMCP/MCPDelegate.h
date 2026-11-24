/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
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
    virtual void click_element(u64 tab_id, Optional<String> selector, Optional<String> element_id, String strategy, OnToolCallComplete) = 0;
    virtual void type_text(u64 tab_id, Optional<String> selector, Optional<String> element_id, String text, bool clear_first, String strategy, OnToolCallComplete) = 0;
    virtual void evaluate_javascript(u64 tab_id, String script, OnToolCallComplete) = 0;
    virtual void take_screenshot(u64 tab_id, OnToolCallComplete) = 0;
};

}
