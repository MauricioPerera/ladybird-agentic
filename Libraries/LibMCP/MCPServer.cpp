/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/IPv4Address.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <AK/WeakPtr.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibCore/Timer.h>
#include <LibHTTP/HttpRequest.h>
#include <LibHTTP/Status.h>
#include <LibMCP/MCPDelegate.h>
#include <LibMCP/MCPServer.h>

namespace MCP {

static constexpr size_t MAX_REQUEST_BYTES = 4 * 1024 * 1024;
static constexpr size_t MAX_ACTIVE_CONNECTIONS = 64;

static bool is_ascii_digits(StringView view)
{
    if (view.is_empty())
        return false;
    for (auto ch : view) {
        if (ch < '0' || ch > '9')
            return false;
    }
    return true;
}

static bool is_allowed_host(StringView host)
{
    host = host.trim_whitespace();
    if (auto colon = host.find_last(':'); colon.has_value()) {
        auto port_part = host.substring_view(*colon + 1);
        if (!is_ascii_digits(port_part))
            return false;
        host = host.substring_view(0, *colon);
    }
    return host.equals_ignoring_ascii_case("localhost"sv) || host == "127.0.0.1"sv;
}

static Optional<StringView> find_header(HTTP::HttpRequest const& request, StringView name)
{
    if (auto it = request.headers().headers().find_if([&](auto& header) { return header.name.equals_ignoring_ascii_case(name); }); !it.is_end())
        return it->value.view();
    return {};
}

static bool constant_time_equals(ReadonlyBytes a, ReadonlyBytes b)
{
    if (a.size() != b.size())
        return false;
    u8 diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= a[i] ^ b[i];
    return diff == 0;
}

static JsonObject make_prop(StringView type, StringView description)
{
    JsonObject prop;
    prop.set("type"sv, type);
    prop.set("description"sv, description);
    return prop;
}

static JsonObject make_string_enum_prop(StringView description, ReadonlySpan<StringView> values, Optional<StringView> default_value = {})
{
    JsonObject prop = make_prop("string"sv, description);
    JsonArray enum_values;
    for (auto value : values)
        enum_values.must_append(value);
    prop.set("enum"sv, move(enum_values));
    if (default_value.has_value())
        prop.set("default"sv, *default_value);
    return prop;
}

static JsonObject make_tool_schema(StringView name, StringView description, JsonObject properties, ReadonlySpan<StringView> required = {})
{
    JsonObject schema;
    schema.set("type"sv, "object"sv);
    schema.set("properties"sv, move(properties));
    if (!required.is_empty()) {
        JsonArray required_array;
        for (auto field : required)
            required_array.must_append(field);
        schema.set("required"sv, move(required_array));
    }
    schema.set("additionalProperties"sv, false);

    JsonObject tool;
    tool.set("name"sv, name);
    tool.set("description"sv, description);
    tool.set("inputSchema"sv, move(schema));
    return tool;
}

static JsonObject make_int_prop(StringView description, i64 minimum, i64 maximum, Optional<i64> default_value = {})
{
    JsonObject prop = make_prop("integer"sv, description);
    prop.set("minimum"sv, minimum);
    prop.set("maximum"sv, maximum);
    if (default_value.has_value())
        prop.set("default"sv, *default_value);
    return prop;
}

static JsonObject build_flow_schema(StringView description)
{
    static constexpr Array step_actions {
        "open"sv,
        "reload"sv,
        "back"sv,
        "forward"sv,
        "click"sv,
        "fill"sv,
        "press"sv,
        "wait_for"sv,
        "assert_text"sv,
        "assert_visible"sv,
        "assert_url"sv,
        "extract"sv,
        "eval"sv,
        "sleep"sv,
        "screenshot"sv,
    };
    static constexpr Array locator_kinds {
        "css"sv,
        "xpath"sv,
        "role"sv,
        "label"sv,
        "placeholder"sv,
        "text"sv,
        "test_id"sv,
    };

    JsonObject locator_props;
    locator_props.set("kind"sv, make_string_enum_prop("Element matching strategy."sv, locator_kinds, "css"sv));
    locator_props.set("value"sv, make_prop("string"sv, "Selector, XPath, label, placeholder, text, or data-testid depending on 'kind'."sv));
    locator_props.set("role"sv, make_prop("string"sv, "ARIA role name (for kind='role', e.g. button, heading, textbox, link)."sv));
    locator_props.set("name"sv, make_prop("string"sv, "Accessible name filter when kind='role'."sv));
    locator_props.set("exact"sv, make_prop("boolean"sv, "Require exact string match instead of substring (default: false)."sv));
    locator_props.set("nth"sv, make_int_prop("Zero-based match index when multiple elements match."sv, 0, 1000));

    JsonObject locator_schema;
    locator_schema.set("type"sv, "object"sv);
    locator_schema.set("description"sv, "Target element locator."sv);
    locator_schema.set("properties"sv, move(locator_props));

    JsonObject step_props;
    step_props.set("action"sv, make_string_enum_prop("Step action to execute."sv, step_actions, "click"sv));
    step_props.set("label"sv, make_prop("string"sv, "Optional human-readable step label."sv));
    step_props.set("url"sv, make_prop("string"sv, "URL for 'open' (supports {{var}} interpolation; http:, https:, about:blank)."sv));
    step_props.set("locator"sv, move(locator_schema));
    step_props.set("text"sv, make_prop("string"sv, "Input value for 'fill' (supports {{var}} interpolation)."sv));
    step_props.set("key"sv, make_prop("string"sv, "Keyboard key name for 'press' (e.g. 'Enter', 'Tab', 'Escape')."sv));
    step_props.set("clear"sv, make_prop("boolean"sv, "Clear existing field value before 'fill' (default: true)."sv));
    step_props.set("contains"sv, make_prop("string"sv, "Expected substring for 'assert_text' or 'assert_url'."sv));
    step_props.set("equals"sv, make_prop("string"sv, "Expected exact string for 'assert_text' or 'assert_url'."sv));
    step_props.set("wait_for_load"sv, make_prop("boolean"sv, "Wait for navigation load completion on 'open'/'reload' (default: true)."sv));
    step_props.set("script"sv, make_prop("string"sv, "JavaScript function body for 'eval' (receives runtime variables as arguments[0])."sv));
    step_props.set("save_as"sv, make_prop("string"sv, "Variable/output key to store result of 'extract' or 'eval'."sv));
    step_props.set("timeout_ms"sv, make_int_prop("Step timeout override in milliseconds."sv, 100, 120000));
    step_props.set("ms"sv, make_int_prop("Delay in milliseconds for 'sleep'."sv, 0, 60000));

    JsonArray step_required;
    step_required.must_append("action"sv);

    JsonObject step_schema;
    step_schema.set("type"sv, "object"sv);
    step_schema.set("properties"sv, move(step_props));
    step_schema.set("required"sv, move(step_required));

    JsonObject steps_schema;
    steps_schema.set("type"sv, "array"sv);
    steps_schema.set("minItems"sv, 1);
    steps_schema.set("description"sv, "Ordered list of automation steps to execute."sv);
    steps_schema.set("items"sv, move(step_schema));

    JsonObject var_value_schema;
    var_value_schema.set("type"sv, "string"sv);

    JsonObject variables_schema;
    variables_schema.set("type"sv, "object"sv);
    variables_schema.set("description"sv, "Default variable map interpolated into strings via {{var}}."sv);
    variables_schema.set("additionalProperties"sv, move(var_value_schema));

    JsonObject flow_props;
    flow_props.set("schema_version"sv, make_int_prop("Declarative flow schema version (default: 1)."sv, 1, 1, 1));
    flow_props.set("id"sv, make_prop("string"sv, "Flow ID: lowercase a-z0-9 start, a-z0-9-_ body, max 64 chars."sv));
    flow_props.set("name"sv, make_prop("string"sv, "Human-readable flow name."sv));
    flow_props.set("description"sv, make_prop("string"sv, "Optional description of what the flow automates."sv));
    flow_props.set("default_timeout_ms"sv, make_int_prop("Default per-step timeout in milliseconds (default: 10000)."sv, 100, 120000, 10000));
    flow_props.set("variables"sv, move(variables_schema));
    flow_props.set("steps"sv, move(steps_schema));

    JsonArray flow_required;
    flow_required.must_append("id"sv);
    flow_required.must_append("name"sv);
    flow_required.must_append("steps"sv);

    JsonObject flow_schema;
    flow_schema.set("type"sv, "object"sv);
    flow_schema.set("description"sv, description);
    flow_schema.set("properties"sv, move(flow_props));
    flow_schema.set("required"sv, move(flow_required));
    return flow_schema;
}

static JsonArray build_tool_definitions()
{
    static constexpr Array selector_strategies { "css selector"sv, "xpath"sv, "link text"sv, "partial link text"sv, "tag name"sv };
    JsonArray tools;

    tools.must_append(make_tool_schema(
        "list_tabs"sv,
        "List open browser tabs that currently allow MCP access. Denied or private-by-default tabs are omitted."sv,
        JsonObject {}));

    tools.must_append(make_tool_schema(
        "get_active_tab"sv,
        "Get metadata for the active browser tab if it currently allows MCP access."sv,
        JsonObject {}));

    {
        JsonObject props;
        props.set("url"sv, make_prop("string"sv, "Optional http:, https:, or about:blank URL to load in the new tab."sv));
        props.set("activate"sv, make_prop("boolean"sv, "Whether to focus the new tab in the window (default: true)."sv));
        props.set("wait_for_load"sv, make_prop("boolean"sv, "Whether to wait for initial page load before returning (default: true)."sv));
        props.set("container_id"sv, make_prop("string"sv, "Optional tab container ID (e.g. 'personal', 'work') to isolate cookies and site storage from other tabs."sv));
        tools.must_append(make_tool_schema(
            "open_tab"sv,
            "Open a new non-private browser tab with MCP access automatically enabled."sv,
            move(props)));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        tools.must_append(make_tool_schema(
            "switch_tab"sv,
            "Activate and focus an MCP-allowed browser tab."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        tools.must_append(make_tool_schema(
            "close_tab"sv,
            "Close an MCP-allowed browser tab."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("url"sv, make_prop("string"sv, "Destination URL (only http:, https:, or about:blank allowed)."sv));
        props.set("wait_for_load"sv, make_prop("boolean"sv, "Wait until load completes before returning (default: true)."sv));
        tools.must_append(make_tool_schema(
            "navigate"sv,
            "Navigate an MCP-allowed tab to a URL."sv,
            move(props),
            Array { "tab_id"sv, "url"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("wait_for_load"sv, make_prop("boolean"sv, "Wait until reload completes before returning (default: true)."sv));
        tools.must_append(make_tool_schema(
            "reload"sv,
            "Reload an MCP-allowed browser tab."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        tools.must_append(make_tool_schema(
            "go_back"sv,
            "Navigate back one entry in the tab's session history."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        tools.must_append(make_tool_schema(
            "go_forward"sv,
            "Navigate forward one entry in the tab's session history."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        tools.must_append(make_tool_schema(
            "get_accessibility_tree"sv,
            "Retrieve the current document's accessibility tree JSON."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        tools.must_append(make_tool_schema(
            "get_dom_tree"sv,
            "Retrieve the current document's serialized DOM tree JSON."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        tools.must_append(make_tool_schema(
            "get_page_source"sv,
            "Retrieve the current document's serialized HTML source."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("selector"sv, make_prop("string"sv, "Selector expression to match DOM elements."sv));
        props.set("strategy"sv, make_string_enum_prop("Locator strategy (default: css selector)."sv, selector_strategies, "css selector"sv));
        tools.must_append(make_tool_schema(
            "find_elements"sv,
            "Find matching DOM elements in the tab and return their WebDriver element handles."sv,
            move(props),
            Array { "tab_id"sv, "selector"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("ref"sv, make_prop("string"sv, "Element reference from get_page_snapshot (e.g. '@e1'). Takes precedence over selector."sv));
        props.set("selector"sv, make_prop("string"sv, "Selector for the element to click (used if ref and element_id are omitted)."sv));
        props.set("element_id"sv, make_prop("string"sv, "Previously returned element ID from find_elements."sv));
        props.set("strategy"sv, make_string_enum_prop("Locator strategy when selector is used (default: css selector)."sv, selector_strategies, "css selector"sv));
        tools.must_append(make_tool_schema(
            "click"sv,
            "Click an element identified by @ref, element_id, or selector."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("text"sv, make_prop("string"sv, "Text to type into the element."sv));
        props.set("ref"sv, make_prop("string"sv, "Element reference from get_page_snapshot (e.g. '@e2'). Takes precedence over selector."sv));
        props.set("selector"sv, make_prop("string"sv, "Selector for the target input element (used if ref and element_id are omitted)."sv));
        props.set("element_id"sv, make_prop("string"sv, "Previously returned element ID from find_elements."sv));
        props.set("clear"sv, make_prop("boolean"sv, "Clear existing value before typing (default: false)."sv));
        props.set("strategy"sv, make_string_enum_prop("Locator strategy when selector is used (default: css selector)."sv, selector_strategies, "css selector"sv));
        tools.must_append(make_tool_schema(
            "type_text"sv,
            "Type text into an element identified by @ref, element_id, or selector."sv,
            move(props),
            Array { "tab_id"sv, "text"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("max_elements"sv, make_int_prop("Maximum interactive elements to capture (default: 150)."sv, 1, 500, 150));
        props.set("include_text"sv, make_prop("boolean"sv, "Include a compact visible body text excerpt (default: true)."sv));
        tools.must_append(make_tool_schema(
            "get_page_snapshot"sv,
            "Return a compact, token-efficient snapshot of visible interactive elements with @e1/@e2 refs usable directly by click, type_text, press_key, hover, select_option, scroll, and wait_for."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("key"sv, make_prop("string"sv, "Keyboard key name (e.g. 'Enter', 'Tab', 'Escape', 'ArrowDown')."sv));
        props.set("ref"sv, make_prop("string"sv, "Optional @ref target element to focus before pressing the key."sv));
        props.set("selector"sv, make_prop("string"sv, "Optional CSS selector target to focus before pressing the key."sv));
        tools.must_append(make_tool_schema(
            "press_key"sv,
            "Dispatch a keyboard key press on the active element or specified @ref/selector."sv,
            move(props),
            Array { "tab_id"sv, "key"sv }));
    }

    {
        static constexpr Array scroll_directions { "down"sv, "up"sv, "top"sv, "bottom"sv };
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("direction"sv, make_string_enum_prop("Scroll direction (default: down)."sv, scroll_directions, "down"sv));
        props.set("amount"sv, make_int_prop("Pixels to scroll for 'up'/'down' (default: 600)."sv, 0, 10000, 600));
        props.set("ref"sv, make_prop("string"sv, "Optional @ref element to scroll into view instead of scrolling viewport."sv));
        props.set("selector"sv, make_prop("string"sv, "Optional CSS selector to scroll into view."sv));
        tools.must_append(make_tool_schema(
            "scroll"sv,
            "Scroll the viewport or scroll a specific @ref/selector element into view."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("ref"sv, make_prop("string"sv, "Element reference from get_page_snapshot (e.g. '@e1')."sv));
        props.set("selector"sv, make_prop("string"sv, "CSS selector (used if ref is omitted)."sv));
        tools.must_append(make_tool_schema(
            "hover"sv,
            "Move pointer/hover over an element identified by @ref or CSS selector."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("from_ref"sv, make_prop("string"sv, "Source element @ref from get_page_snapshot."sv));
        props.set("from_selector"sv, make_prop("string"sv, "Source CSS selector (used if from_ref is omitted)."sv));
        props.set("from_x"sv, make_prop("integer"sv, "Source viewport X coordinate (used if from_ref/from_selector omitted)."sv));
        props.set("from_y"sv, make_prop("integer"sv, "Source viewport Y coordinate (used if from_ref/from_selector omitted)."sv));
        props.set("to_ref"sv, make_prop("string"sv, "Target element @ref from get_page_snapshot."sv));
        props.set("to_selector"sv, make_prop("string"sv, "Target CSS selector (used if to_ref is omitted)."sv));
        props.set("to_x"sv, make_prop("integer"sv, "Target viewport X coordinate."sv));
        props.set("to_y"sv, make_prop("integer"sv, "Target viewport Y coordinate."sv));
        props.set("delta_x"sv, make_prop("integer"sv, "Horizontal drag offset in pixels from source center/coordinates."sv));
        props.set("delta_y"sv, make_prop("integer"sv, "Vertical drag offset in pixels from source center/coordinates."sv));
        props.set("steps"sv, make_int_prop("Interpolated move steps between source and destination (default: 10)."sv, 1, 60, 10));
        props.set("duration_ms"sv, make_int_prop("Total drag duration in milliseconds (default: 250)."sv, 0, 5000, 250));
        tools.must_append(make_tool_schema(
            "drag"sv,
            "Perform a real pointer mouse drag between elements (@ref/selector) or viewport coordinates/deltas (for timeline handles, sliders, drag-and-drop)."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("ref"sv, make_prop("string"sv, "File input @ref from get_page_snapshot."sv));
        props.set("selector"sv, make_prop("string"sv, "CSS selector for target <input type=\"file\"> (traverses open shadow DOM)."sv));
        props.set("element_id"sv, make_prop("string"sv, "Previously returned element ID from find_elements."sv));
        props.set("path"sv, make_prop("string"sv, "Single absolute local file path to attach."sv));
        JsonObject paths_prop;
        paths_prop.set("type"sv, "array"sv);
        paths_prop.set("description"sv, "Array of absolute local file paths to attach (for multi-file inputs)."sv);
        JsonObject item_schema;
        item_schema.set("type"sv, "string"sv);
        paths_prop.set("items"sv, move(item_schema));
        props.set("paths"sv, move(paths_prop));
        tools.must_append(make_tool_schema(
            "upload_file"sv,
            "Attach one or more existing local files directly to an <input type=\"file\"> without opening a native file dialog."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("ref"sv, make_prop("string"sv, "<select> element reference from get_page_snapshot."sv));
        props.set("selector"sv, make_prop("string"sv, "<select> CSS selector (used if ref is omitted)."sv));
        props.set("value"sv, make_prop("string"sv, "Option value attribute to select."sv));
        props.set("label"sv, make_prop("string"sv, "Option visible label text to select."sv));
        props.set("index"sv, make_int_prop("Zero-based option index to select."sv, 0, 10000));
        tools.must_append(make_tool_schema(
            "select_option"sv,
            "Select an option in a <select> element by value, label, or index."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("ref"sv, make_prop("string"sv, "Optional @ref element to wait to appear/remain visible."sv));
        props.set("selector"sv, make_prop("string"sv, "Optional CSS selector to wait to become visible."sv));
        props.set("text"sv, make_prop("string"sv, "Optional substring text to wait for in the page body (or inside ref/selector)."sv));
        props.set("timeout_ms"sv, make_int_prop("Timeout in milliseconds (default: 10000)."sv, 100, 60000, 10000));
        tools.must_append(make_tool_schema(
            "wait_for"sv,
            "Wait until an element (@ref/selector) is visible and/or expected text appears on the page."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("limit"sv, make_int_prop("Maximum recent log entries to return (default: 100)."sv, 1, 200, 100));
        props.set("clear"sv, make_prop("boolean"sv, "Clear buffered console logs for this tab after reading (default: false)."sv));
        tools.must_append(make_tool_schema(
            "get_console_logs"sv,
            "Read buffered JavaScript console logs, warnings, errors, and uncaught exceptions for an MCP-allowed tab."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("limit"sv, make_int_prop("Maximum recent network requests to return (default: 100)."sv, 1, 200, 100));
        props.set("clear"sv, make_prop("boolean"sv, "Clear buffered network requests for this tab after reading (default: false)."sv));
        props.set("only_errors"sv, make_prop("boolean"sv, "Return only failed requests or HTTP status >= 400 (default: false)."sv));
        tools.must_append(make_tool_schema(
            "get_network_requests"sv,
            "Inspect buffered HTTP/network requests, status codes, sizes, and network/SSL errors for an MCP-allowed tab."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("script"sv, make_prop("string"sv, "JavaScript function body to execute in the top-level document context (use `return ...` to return a value)."sv));
        tools.must_append(make_tool_schema(
            "eval_js"sv,
            "Execute JavaScript in the tab and return the JSON-serialized result."sv,
            move(props),
            Array { "tab_id"sv, "script"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        tools.must_append(make_tool_schema(
            "take_screenshot"sv,
            "Capture a PNG screenshot of the tab's top-level viewport."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    tools.must_append(make_tool_schema(
        "list_flows"sv,
        "List saved browser automation flows with their IDs, names, descriptions, and step counts."sv,
        JsonObject {}));

    {
        JsonObject props;
        props.set("flow_id"sv, make_prop("string"sv, "ID of the saved automation flow."sv));
        tools.must_append(make_tool_schema(
            "get_flow"sv,
            "Retrieve the full declarative JSON definition of a saved automation flow."sv,
            move(props),
            Array { "flow_id"sv }));
    }

    {
        JsonObject props;
        props.set("flow"sv, build_flow_schema("Declarative automation flow JSON definition to validate and save."sv));
        tools.must_append(make_tool_schema(
            "save_flow"sv,
            "Validate and persist a declarative browser automation flow for repeated execution."sv,
            move(props),
            Array { "flow"sv }));
    }

    {
        JsonObject props;
        props.set("flow_id"sv, make_prop("string"sv, "ID of the saved automation flow to delete."sv));
        tools.must_append(make_tool_schema(
            "delete_flow"sv,
            "Delete a saved automation flow by ID."sv,
            move(props),
            Array { "flow_id"sv }));
    }

    {
        JsonObject props;
        props.set("flow_id"sv, make_prop("string"sv, "ID of a saved automation flow to run (omit if providing inline 'flow')."sv));
        props.set("flow"sv, build_flow_schema("Inline declarative automation flow definition to run without saving first."sv));
        props.set("tab_id"sv, make_prop("integer"sv, "Optional MCP-allowed tab ID to run the flow in."sv));
        props.set("variables"sv, make_prop("object"sv, "Optional key-value variables overriding flow defaults for this run."sv));
        props.set("open_new_tab"sv, make_prop("boolean"sv, "Open a new MCP-allowed tab when tab_id is omitted (default: true)."sv));
        props.set("wait"sv, make_prop("boolean"sv, "Wait for the flow to finish before responding (default: true). Set false to start asynchronously and poll with get_flow_run."sv));
        props.set("close_tab_on_finish"sv, make_prop("boolean"sv, "Close the tab automatically when the flow finishes if run_flow opened a new tab (never closes a pre-existing tab_id)."sv));
        tools.must_append(make_tool_schema(
            "run_flow"sv,
            "Execute a saved or inline declarative automation flow in Ladybird and return step-by-step results and extracted outputs."sv,
            move(props)));
    }

    {
        JsonObject props;
        props.set("flow_id"sv, make_prop("string"sv, "Optional flow ID filter."sv));
        props.set("limit"sv, make_prop("integer"sv, "Maximum number of recent runs to return (default: 20)."sv));
        tools.must_append(make_tool_schema(
            "list_flow_runs"sv,
            "List recent automation flow execution records."sv,
            move(props)));
    }

    {
        JsonObject props;
        props.set("run_id"sv, make_prop("string"sv, "ID of a flow execution run."sv));
        tools.must_append(make_tool_schema(
            "get_flow_run"sv,
            "Retrieve step-by-step details and outputs of a flow execution run."sv,
            move(props),
            Array { "run_id"sv }));
    }

    {
        JsonObject props;
        props.set("run_id"sv, make_prop("string"sv, "ID of an active flow execution run to cancel."sv));
        tools.must_append(make_tool_schema(
            "cancel_flow_run"sv,
            "Cancel an in-progress automation flow execution run."sv,
            move(props),
            Array { "run_id"sv }));
    }

    tools.must_append(make_tool_schema(
        "list_extensions"sv,
        "List installed browser extensions with IDs, versions, permissions, bundled flows, and errors."sv,
        JsonObject {}));

    {
        JsonObject props;
        props.set("path"sv, make_prop("string"sv, "Absolute path to an unpacked extension directory containing manifest.json."sv));
        tools.must_append(make_tool_schema(
            "install_unpacked_extension"sv,
            "Validate and install an unpacked MV3 extension directory (with optional bundled Ladybird automation flows)."sv,
            move(props),
            Array { "path"sv }));
    }

    {
        JsonObject props;
        props.set("extension_id"sv, make_prop("string"sv, "ID of the installed extension."sv));
        props.set("enabled"sv, make_prop("boolean"sv, "Whether the extension should be enabled."sv));
        tools.must_append(make_tool_schema(
            "set_extension_enabled"sv,
            "Enable or disable an installed browser extension."sv,
            move(props),
            Array { "extension_id"sv, "enabled"sv }));
    }

    {
        JsonObject props;
        props.set("extension_id"sv, make_prop("string"sv, "ID of the installed extension to reload from disk."sv));
        tools.must_append(make_tool_schema(
            "reload_extension"sv,
            "Reload an unpacked extension's manifest and bundled flows from disk."sv,
            move(props),
            Array { "extension_id"sv }));
    }

    {
        JsonObject props;
        props.set("extension_id"sv, make_prop("string"sv, "ID of the installed extension to remove from Ladybird."sv));
        tools.must_append(make_tool_schema(
            "remove_extension"sv,
            "Uninstall a browser extension from the profile registry (keeps the unpacked source folder intact)."sv,
            move(props),
            Array { "extension_id"sv }));
    }

    {
        JsonObject props;
        props.set("extension_id"sv, make_prop("string"sv, "ID of the installed extension to inject."sv));
        props.set("tab_id"sv, make_prop("integer"sv, "Optional allowed tab ID; defaults to the active allowed tab."sv));
        tools.must_append(make_tool_schema(
            "inject_extension"sv,
            "Inject an enabled extension's CSS and JS content scripts into a currently open allowed tab."sv,
            move(props),
            Array { "extension_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("include_declarative"sv, [] {
            auto p = make_prop("boolean"sv, "Also scan declarative <form toolname=...> HTML tools (default: true)."sv);
            p.set("default"sv, true);
            return p;
        }());
        tools.must_append(make_tool_schema(
            "list_page_tools"sv,
            "Discover WebMCP tools exposed by a page or extension via document.modelContext / navigator.modelContext and declarative <form toolname> annotations in an MCP-allowed tab."sv,
            move(props),
            Array { "tab_id"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab."sv));
        props.set("name"sv, make_prop("string"sv, "Name of the WebMCP tool to invoke."sv));
        props.set("arguments"sv, make_prop("object"sv, "JSON object of tool input arguments matching the tool's inputSchema (default: {})."sv));
        props.set("confirm_consequential"sv, [] {
            auto p = make_prop("boolean"sv, "Set true only after user confirmation when calling a tool annotated as consequentialHint or destructiveHint."sv);
            p.set("default"sv, false);
            return p;
        }());
        tools.must_append(make_tool_schema(
            "call_page_tool"sv,
            "Invoke a WebMCP tool discovered via list_page_tools inside an MCP-allowed tab. Supports async execute() handlers and declarative forms. Consequential/destructive tools require confirm_consequential: true."sv,
            move(props),
            Array { "tab_id"sv, "name"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "Optional MCP-allowed tab ID to filter skills matching that tab's current URL."sv));
        props.set("url"sv, make_prop("string"sv, "Optional http: or https: URL to filter skills matching that page/site."sv));
        props.set("include_disabled"sv, [] {
            auto p = make_prop("boolean"sv, "Include disabled skills (default: false; ignored when filtering by URL/tab)."sv);
            p.set("default"sv, false);
            return p;
        }());
        tools.must_append(make_tool_schema(
            "list_skills"sv,
            "Discover Agent Skills configured in Ladybird (optionally filtered to a tab or URL). Returns skill names, URIs, descriptions, URL match patterns, and resource manifests without loading full instructions."sv,
            move(props)));
    }

    {
        JsonObject props;
        props.set("name"sv, make_prop("string"sv, "Skill slug name (e.g. 'youtube-navigation'). Required unless 'uri' is provided."sv));
        props.set("uri"sv, make_prop("string"sv, "Skill resource URI (e.g. 'skill://youtube-navigation/SKILL.md'). Required unless 'name' is provided."sv));
        props.set("file"sv, make_prop("string"sv, "Optional relative path of a supporting file inside the skill (defaults to 'SKILL.md')."sv));
        tools.must_append(make_tool_schema(
            "read_skill"sv,
            "Read a skill's SKILL.md instructions or a supporting file on demand. Also available via standard SEP-2640 resources/read."sv,
            move(props)));
    }

    {
        JsonObject props;
        JsonObject skill_props;
        skill_props.set("name"sv, make_prop("string"sv, "Skill slug name (1-64 lowercase a-z0-9 and hyphens)."sv));
        skill_props.set("description"sv, make_prop("string"sv, "Concise description of when to use this skill."sv));
        JsonObject matches_schema;
        matches_schema.set("type"sv, "array"sv);
        matches_schema.set("items"sv, make_prop("string"sv, "URL pattern such as 'https://www.youtube.com/*' or '*'."sv));
        matches_schema.set("description"sv, "URL match patterns controlling which pages surface this skill automatically."sv);
        skill_props.set("matches"sv, move(matches_schema));
        skill_props.set("enabled"sv, make_prop("boolean"sv, "Whether the skill is active (default: true)."sv));
        skill_props.set("body"sv, make_prop("string"sv, "Markdown instructions for the agent."sv));
        JsonObject file_props;
        file_props.set("path"sv, make_prop("string"sv, "Relative path such as 'references/selectors.md'."sv));
        file_props.set("content"sv, make_prop("string"sv, "UTF-8 file content."sv));
        JsonObject file_item;
        file_item.set("type"sv, "object"sv);
        file_item.set("properties"sv, move(file_props));
        JsonObject files_schema;
        files_schema.set("type"sv, "array"sv);
        files_schema.set("items"sv, move(file_item));
        skill_props.set("supporting_files"sv, move(files_schema));
        JsonArray skill_req;
        skill_req.must_append("name"sv);
        skill_req.must_append("description"sv);
        JsonObject skill_obj;
        skill_obj.set("type"sv, "object"sv);
        skill_obj.set("description"sv, "Skill definition to create or update."sv);
        skill_obj.set("properties"sv, move(skill_props));
        skill_obj.set("required"sv, move(skill_req));
        props.set("skill"sv, move(skill_obj));
        tools.must_append(make_tool_schema(
            "save_skill"sv,
            "Create or update an Agent Skill on disk. Disabled by default for security unless Ladybird is launched with --mcp-allow-skill-write (users manage skills in about:skills)."sv,
            move(props),
            Array { "skill"sv }));
    }

    {
        JsonObject props;
        props.set("name"sv, make_prop("string"sv, "Skill slug name to delete."sv));
        tools.must_append(make_tool_schema(
            "delete_skill"sv,
            "Delete an Agent Skill from disk. Disabled by default unless Ladybird is launched with --mcp-allow-skill-write."sv,
            move(props),
            Array { "name"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "Optional MCP-allowed tab ID to filter credentials pinned to that tab's origin and container."sv));
        tools.must_append(make_tool_schema(
            "list_credentials"sv,
            "List blind credential metadata from the browser's encrypted Vault (about:vault). Returns only IDs, labels, pinned origins, containers, and available field types ('username', 'password', 'totp') — never secrets."sv,
            move(props)));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab currently committed on a pinned origin of the credential."sv));
        props.set("credential_id"sv, make_prop("string"sv, "Vault credential ID to inject blindly."sv));
        static constexpr Array vault_fields { "username"sv, "password"sv, "totp"sv };
        props.set("field"sv, make_string_enum_prop("Credential field to inject blindly into the input."sv, vault_fields));
        props.set("ref"sv, make_prop("string"sv, "Optional snapshot @eN element ref for the target input (auto-detects visible input if omitted)."sv));
        props.set("selector"sv, make_prop("string"sv, "Optional CSS selector for the target input."sv));
        tools.must_append(make_tool_schema(
            "fill_credential"sv,
            "Blindly inject a single credential field ('username', 'password', or live 6-digit 'totp') into an input on an origin- and container-pinned tab without revealing the value to the agent. Injecting 'password' or 'totp' quarantines read/eval tools on the tab until post-login navigation."sv,
            move(props),
            Array { "tab_id"sv, "credential_id"sv, "field"sv }));
    }

    {
        JsonObject props;
        props.set("tab_id"sv, make_prop("integer"sv, "ID of an MCP-allowed tab currently committed on a pinned origin of the credential."sv));
        props.set("credential_id"sv, make_prop("string"sv, "Vault credential ID for atomic blind login."sv));
        props.set("username_ref"sv, make_prop("string"sv, "Optional @eN ref for username/email input."sv));
        props.set("username_selector"sv, make_prop("string"sv, "Optional CSS selector for username/email input."sv));
        props.set("password_ref"sv, make_prop("string"sv, "Optional @eN ref for <input type='password'>."sv));
        props.set("password_selector"sv, make_prop("string"sv, "Optional CSS selector for <input type='password'>."sv));
        props.set("totp_ref"sv, make_prop("string"sv, "Optional @eN ref for 2FA TOTP code input."sv));
        props.set("totp_selector"sv, make_prop("string"sv, "Optional CSS selector for 2FA TOTP code input."sv));
        props.set("submit_ref"sv, make_prop("string"sv, "Optional @eN ref for submit button."sv));
        props.set("submit_selector"sv, make_prop("string"sv, "Optional CSS selector for submit button."sv));
        props.set("wait_for_navigation"sv, [] {
            auto p = make_prop("boolean"sv, "Wait briefly for post-login navigation commit (default: true)."sv);
            p.set("default"sv, true);
            return p;
        }());
        tools.must_append(make_tool_schema(
            "login_with_credential"sv,
            "Preferred atomic blind login: verifies exact origin and container pinning, fills username/password/(optional TOTP), validates form action origin, submits, and quarantines read/eval tools until navigation commits — zero secrets ever enter the MCP response."sv,
            move(props),
            Array { "tab_id"sv, "credential_id"sv }));
    }

    return tools;
}

static JsonObject build_server_capabilities()
{
    JsonObject capabilities;
    JsonObject tools_cap;
    tools_cap.set("listChanged"sv, false);
    capabilities.set("tools"sv, move(tools_cap));

    capabilities.set("resources"sv, JsonObject {});

    JsonObject skills_ext;
    skills_ext.set("directoryRead"sv, true);
    JsonObject extensions_cap;
    extensions_cap.set("io.modelcontextprotocol/skills"sv, move(skills_ext));
    capabilities.set("extensions"sv, move(extensions_cap));
    return capabilities;
}

static JsonObject tab_info_to_json(TabInfo const& info)
{
    JsonObject object;
    object.set("id"sv, info.id);
    object.set("title"sv, info.title);
    object.set("url"sv, info.url);
    object.set("container_id"sv, info.container_id);
    object.set("is_loading"sv, info.is_loading);
    object.set("is_active"sv, info.is_active);
    if (!info.matching_skills.is_empty()) {
        JsonArray skills_json;
        for (auto const& hint : info.matching_skills) {
            JsonObject s;
            s.set("name"sv, hint.name);
            s.set("uri"sv, hint.uri);
            s.set("description"sv, hint.description);
            skills_json.must_append(move(s));
        }
        object.set("matching_skills"sv, move(skills_json));
    }
    if (!info.matching_credentials.is_empty()) {
        JsonArray creds_json;
        for (auto const& hint : info.matching_credentials) {
            JsonObject c;
            c.set("id"sv, hint.id);
            c.set("label"sv, hint.label);
            JsonArray fields_json;
            for (auto const& f : hint.fields)
                fields_json.must_append(f);
            c.set("fields"sv, move(fields_json));
            creds_json.must_append(move(c));
        }
        object.set("matching_credentials"sv, move(creds_json));
    }
    return object;
}

class MCPServer::Connection final
    : public RefCounted<Connection>
    , public Weakable<Connection> {
public:
    static ErrorOr<NonnullRefPtr<Connection>> create(MCPServer& server, NonnullOwnPtr<Core::TCPSocket> socket)
    {
        auto buffered_socket = TRY(Core::BufferedTCPSocket::create(move(socket)));
        return adopt_ref(*new Connection(server, move(buffered_socket)));
    }

    ~Connection()
    {
        close();
    }

    void close()
    {
        if (m_closed)
            return;
        m_closed = true;
        if (m_read_timeout_timer) {
            m_read_timeout_timer->stop();
            m_read_timeout_timer.clear();
        }
        m_socket->on_ready_to_read = {};
        m_socket->close();
    }

private:
    Connection(MCPServer& server, NonnullOwnPtr<Core::BufferedTCPSocket> socket)
        : m_server(server.make_weak_ptr())
        , m_socket(move(socket))
    {
        m_read_timeout_timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(30), [weak_self = make_weak_ptr()] {
            if (!weak_self || weak_self->m_closed || weak_self->m_request_handled)
                return;
            weak_self->send_http_text_error(408, "Request Timeout"sv);
        });
        m_read_timeout_timer->start();

        m_socket->on_ready_to_read = [this] {
            if (auto result = on_ready_to_read(); result.is_error()) {
                warnln("MCP connection error: {}", result.error());
                send_http_text_error(500, "Internal Server Error"sv);
            }
        };
    }

    void close_later()
    {
        if (m_close_scheduled)
            return;
        m_close_scheduled = true;
        close();
        Core::deferred_invoke([weak_self = make_weak_ptr(), weak_server = m_server] {
            if (weak_server && weak_self)
                weak_server->remove_connection(*weak_self);
        });
    }

    ErrorOr<void> on_ready_to_read()
    {
        if (m_closed || m_request_handled)
            return {};

        auto buffer = TRY(ByteBuffer::create_uninitialized(max(m_socket->buffer_size(), static_cast<size_t>(4096))));
        for (;;) {
            if (!TRY(m_socket->can_read_without_blocking()))
                break;

            auto data = TRY(m_socket->read_some(buffer));
            if (data.is_empty()) {
                if (m_socket->is_eof())
                    break;
                break;
            }
            if (m_remaining_request.length() + data.size() > MAX_REQUEST_BYTES) {
                send_http_text_error(413, "Request exceeded maximum allowed size"sv);
                return {};
            }
            TRY(m_remaining_request.try_append(StringView { data }));

            if (m_socket->is_eof())
                break;
        }

        if (m_remaining_request.is_empty()) {
            if (m_socket->is_eof())
                close_later();
            return {};
        }

        auto parsed_request = HTTP::HttpRequest::from_raw_request(m_remaining_request.string_view().bytes());
        if (parsed_request.is_error()) {
            switch (parsed_request.error()) {
            case HTTP::HttpRequest::ParseError::RequestIncomplete:
                if (m_socket->is_eof())
                    close_later();
                return {};
            case HTTP::HttpRequest::ParseError::RequestTooLarge:
                send_http_text_error(413, "Request body or header too large (max 4 MiB body / 64 KiB header)"sv);
                return {};
            case HTTP::HttpRequest::ParseError::UnsupportedMethod:
                send_http_text_error(405, "Method Not Allowed"sv, "Allow: POST, DELETE\r\n"sv);
                return {};
            case HTTP::HttpRequest::ParseError::InvalidURL:
            case HTTP::HttpRequest::ParseError::OutOfMemory:
                send_http_text_error(400, HTTP::HttpRequest::parse_error_to_string(parsed_request.error()));
                return {};
            }
            VERIFY_NOT_REACHED();
        }

        m_remaining_request.clear();
        m_request_handled = true;
        if (m_read_timeout_timer) {
            m_read_timeout_timer->stop();
            m_read_timeout_timer.clear();
        }
        handle_http_request(parsed_request.release_value());
        return {};
    }

    void handle_http_request(HTTP::HttpRequest const& request)
    {
        if (!m_server) {
            close_later();
            return;
        }

        auto host = find_header(request, "Host"sv);
        if (!host.has_value() || !is_allowed_host(*host)) {
            send_http_text_error(403, "Forbidden: Host header must be localhost or 127.0.0.1"sv);
            return;
        }

        if (find_header(request, "Origin"sv).has_value()) {
            send_http_text_error(403, "Forbidden: Requests with an Origin header are not allowed"sv);
            return;
        }

        auto authorization = find_header(request, "Authorization"sv);
        if (!authorization.has_value()) {
            send_http_text_error(401, "Unauthorized: Missing Bearer token"sv, "WWW-Authenticate: Bearer\r\n"sv);
            return;
        }
        auto trimmed_auth = authorization->trim_whitespace();
        static constexpr auto bearer_prefix = "Bearer "sv;
        if (!trimmed_auth.starts_with(bearer_prefix)
            || !constant_time_equals(trimmed_auth.substring_view(bearer_prefix.length()).bytes(), m_server->auth_token().bytes())) {
            send_http_text_error(401, "Unauthorized: Invalid Bearer token"sv, "WWW-Authenticate: Bearer\r\n"sv);
            return;
        }

        if (request.resource() != "/mcp"sv) {
            send_http_text_error(404, "Not Found"sv);
            return;
        }

        if (auto session_header = find_header(request, "Mcp-Session-Id"sv); session_header.has_value()) {
            auto trimmed_session = session_header->trim_whitespace();
            if (!constant_time_equals(trimmed_session.bytes(), m_server->session_id().bytes())) {
                send_http_text_error(404, "Invalid or expired Mcp-Session-Id"sv);
                return;
            }
        }

        if (request.method() == HTTP::HttpRequest::Method::DELETE) {
            send_raw_http_response(200, "application/json; charset=utf-8"sv, "{\"ok\":true}"sv);
            return;
        }

        if (request.method() != HTTP::HttpRequest::Method::POST) {
            send_http_text_error(405, "Method Not Allowed"sv, "Allow: POST, DELETE\r\n"sv);
            return;
        }

        auto content_type = find_header(request, "Content-Type"sv);
        if (!content_type.has_value()
            || !content_type->find_first_split_view(';').trim_whitespace().equals_ignoring_ascii_case("application/json"sv)) {
            send_http_text_error(415, "Unsupported Media Type: Expected application/json"sv);
            return;
        }

        auto maybe_json = JsonValue::from_string(StringView { request.body() });
        if (maybe_json.is_error()) {
            send_json_rpc_error(JsonValue {}, -32700, "Parse error: Invalid JSON"sv, 400);
            return;
        }

        if (!maybe_json.value().is_object()) {
            send_json_rpc_error(JsonValue {}, -32600, "Invalid Request: Expected JSON-RPC object"sv, 400);
            return;
        }

        auto const& rpc = maybe_json.value().as_object();
        auto jsonrpc = rpc.get_string("jsonrpc"sv);
        auto method = rpc.get_string("method"sv);
        if (!jsonrpc.has_value() || *jsonrpc != "2.0"sv || !method.has_value()) {
            JsonValue id;
            if (auto maybe_id = rpc.get("id"sv); maybe_id.has_value())
                id = *maybe_id;
            send_json_rpc_error(id, -32600, "Invalid Request: Expected jsonrpc '2.0' and method"sv, 400);
            return;
        }

        if (!rpc.has("id"sv)) {
            send_raw_http_response(202, "text/plain; charset=utf-8"sv, ""sv);
            return;
        }

        JsonValue id = *rpc.get("id"sv);
        if (id.is_null()) {
            send_json_rpc_error(id, -32600, "Invalid Request: Request id cannot be null"sv, 400);
            return;
        }

        handle_json_rpc_request(move(id), *method, rpc.get_object("params"sv));
    }

    void handle_json_rpc_request(JsonValue id, StringView method, Optional<JsonObject const&> params)
    {
        if (!m_server) {
            close_later();
            return;
        }

        if (method == "initialize"sv) {
            StringView negotiated_version = "2025-03-26"sv;
            if (params.has_value()) {
                if (auto requested_version = params->get_string("protocolVersion"sv); requested_version.has_value() && requested_version->bytes_as_string_view() >= "2026-07-28"sv)
                    negotiated_version = "2026-07-28"sv;
            }

            JsonObject server_info;
            server_info.set("name"sv, "ladybird-mcp"sv);
            server_info.set("version"sv, "0.1.0"sv);

            JsonObject result;
            result.set("protocolVersion"sv, negotiated_version);
            result.set("capabilities"sv, build_server_capabilities());
            result.set("serverInfo"sv, move(server_info));
            result.set("instructions"sv, "Ladybird native MCP server. User tabs require explicit per-tab opt-in before access; tabs opened via open_tab or run_flow (with open_new_tab: true) are automatically allowed. When list_tabs, get_active_tab, open_tab, navigate, or get_page_snapshot includes matching_skills, call read_skill (or SEP-2640 skills/get + resources/read) to load site-specific instructions before acting, and use list_skills to discover skills. When matching_credentials is present or login is needed, use list_credentials, login_with_credential (preferred atomic blind login), or fill_credential (multi-step blind fill): credentials stay in the browser vault and are never revealed, and injecting password/totp quarantines read/eval tools on that tab until navigation commits. Prefer get_page_snapshot first to inspect interactive elements with compact @e1 refs usable by click, type_text, press_key, hover, drag, upload_file, select_option, scroll, and wait_for. Use get_console_logs and get_network_requests to inspect runtime JS errors and HTTP/SSL failures. Use save_flow, list_flows, get_flow, and delete_flow to manage reusable declarative browser automations, and run_flow (by flow_id or inline flow; set wait: false for async polling via get_flow_run and close_tab_on_finish: true for temporary tabs), list_flow_runs, get_flow_run, and cancel_flow_run to execute and monitor them. Use list_extensions, install_unpacked_extension, set_extension_enabled, reload_extension, inject_extension, and remove_extension to manage unpacked browser extensions. Use list_page_tools and call_page_tool to discover and invoke page- or extension-registered WebMCP tools (document.modelContext / navigator.modelContext and declarative <form toolname> elements) before falling back to low-level DOM primitives."sv);

            auto extra_headers = MUST(String::formatted("Mcp-Session-Id: {}\r\n", m_server->session_id()));
            send_json_rpc_result(id, move(result), extra_headers);
            return;
        }

        if (method == "server/discover"sv) {
            JsonObject server_info;
            server_info.set("name"sv, "ladybird-mcp"sv);
            server_info.set("version"sv, "0.1.0"sv);

            JsonObject result;
            result.set("capabilities"sv, build_server_capabilities());
            result.set("serverInfo"sv, move(server_info));
            send_json_rpc_result(id, move(result));
            return;
        }

        if (method == "ping"sv) {
            send_json_rpc_result(id, JsonObject {});
            return;
        }

        auto make_protocol_complete = [weak_self = make_weak_ptr(), id](ProtocolCallResult proto_result) {
            if (!weak_self || weak_self->m_closed)
                return;
            if (proto_result.is_error) {
                weak_self->send_json_rpc_error(id, proto_result.error_code, proto_result.error_message);
                return;
            }
            weak_self->send_json_rpc_result(id, move(proto_result.result));
        };

        if (method == "skills/list"sv) {
            Optional<String> cursor;
            if (params.has_value() && params->has("cursor"sv)) {
                auto cursor_str = params->get_string("cursor"sv);
                if (!cursor_str.has_value()) {
                    send_json_rpc_error(id, -32602, "Invalid params: 'cursor' must be a string"sv);
                    return;
                }
                cursor = *cursor_str;
            }
            m_server->m_delegate.skills_list(move(cursor), move(make_protocol_complete));
            return;
        }

        if (method == "skills/get"sv) {
            auto uri = params.has_value() ? params->get_string("uri"sv) : Optional<String> {};
            if (!uri.has_value() || uri->is_empty()) {
                send_json_rpc_error(id, -32602, "Invalid params: Expected non-empty string 'uri'"sv);
                return;
            }
            m_server->m_delegate.skills_get(*uri, move(make_protocol_complete));
            return;
        }

        if (method == "resources/read"sv) {
            auto uri = params.has_value() ? params->get_string("uri"sv) : Optional<String> {};
            if (!uri.has_value() || uri->is_empty()) {
                send_json_rpc_error(id, -32602, "Invalid params: Expected non-empty string 'uri'"sv);
                return;
            }
            m_server->m_delegate.resources_read(*uri, move(make_protocol_complete));
            return;
        }

        if (method == "resources/directory/read"sv) {
            auto uri = params.has_value() ? params->get_string("uri"sv) : Optional<String> {};
            if (!uri.has_value() || uri->is_empty()) {
                send_json_rpc_error(id, -32602, "Invalid params: Expected non-empty string 'uri'"sv);
                return;
            }
            Optional<String> cursor;
            if (params->has("cursor"sv)) {
                auto cursor_str = params->get_string("cursor"sv);
                if (!cursor_str.has_value()) {
                    send_json_rpc_error(id, -32602, "Invalid params: 'cursor' must be a string"sv);
                    return;
                }
                cursor = *cursor_str;
            }
            m_server->m_delegate.resources_directory_read(*uri, move(cursor), move(make_protocol_complete));
            return;
        }

        if (method == "tools/list"sv) {
            JsonObject result;
            result.set("tools"sv, build_tool_definitions());
            send_json_rpc_result(id, move(result));
            return;
        }

        if (method == "tools/call"sv) {
            if (!params.has_value()) {
                send_json_rpc_error(id, -32602, "Invalid params: Expected params object"sv);
                return;
            }

            auto tool_name = params->get_string("name"sv);
            if (!tool_name.has_value()) {
                send_json_rpc_error(id, -32602, "Invalid params: Missing tool name"sv);
                return;
            }

            JsonObject empty_args;
            JsonObject const& arguments = params->get_object("arguments"sv).value_or(empty_args);

            dispatch_tool_call(move(id), *tool_name, arguments);
            return;
        }

        send_json_rpc_error(id, -32601, "Method not found"sv);
    }

    void dispatch_tool_call(JsonValue id, StringView tool_name, JsonObject const& arguments)
    {
        auto complete = [weak_self = make_weak_ptr(), id = move(id)](ToolCallResult call_result) mutable {
            if (!weak_self || weak_self->m_closed)
                return;

            JsonObject item;
            if (call_result.image_base64_png.has_value()) {
                item.set("type"sv, "image"sv);
                item.set("data"sv, *call_result.image_base64_png);
                item.set("mimeType"sv, "image/png"sv);
            } else {
                item.set("type"sv, "text"sv);
                item.set("text"sv, call_result.text);
            }

            JsonArray content;
            content.must_append(move(item));

            JsonObject result;
            result.set("content"sv, move(content));
            result.set("isError"sv, call_result.is_error);
            weak_self->send_json_rpc_result(id, move(result));
        };

        auto fail_invalid_params = [&](StringView msg) {
            complete({ .is_error = true, .text = String::from_utf8_without_validation(msg.bytes()) });
        };

        auto require_tab_id = [&]() -> Optional<u64> {
            return arguments.get_u64("tab_id"sv);
        };

        auto& delegate = m_server->m_delegate;

        if (tool_name == "list_tabs"sv) {
            JsonArray tabs_json;
            for (auto const& tab : delegate.list_allowed_tabs())
                tabs_json.must_append(tab_info_to_json(tab));
            JsonObject payload;
            payload.set("tabs"sv, move(tabs_json));
            complete({ .is_error = false, .text = JsonValue { move(payload) }.serialized() });
            return;
        }

        if (tool_name == "get_active_tab"sv) {
            JsonObject payload;
            if (auto tab = delegate.active_allowed_tab(); tab.has_value()) {
                payload.set("tab"sv, tab_info_to_json(*tab));
            } else {
                payload.set("tab"sv, JsonValue {});
                payload.set("reason"sv, "Active tab does not have MCP access enabled"sv);
            }
            complete({ .is_error = false, .text = JsonValue { move(payload) }.serialized() });
            return;
        }

        if (tool_name == "open_tab"sv) {
            Optional<String> url;
            if (auto maybe_url = arguments.get_string("url"sv); maybe_url.has_value())
                url = *maybe_url;
            bool activate = arguments.get_bool("activate"sv).value_or(true);
            bool wait_for_load = arguments.get_bool("wait_for_load"sv).value_or(true);
            Optional<String> container_id;
            if (auto maybe_container = arguments.get_string("container_id"sv); maybe_container.has_value())
                container_id = *maybe_container;
            delegate.open_tab(move(url), activate, wait_for_load, move(container_id), move(complete));
            return;
        }

        if (tool_name == "switch_tab"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            delegate.switch_tab(*tab_id, move(complete));
            return;
        }

        if (tool_name == "close_tab"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            delegate.close_tab(*tab_id, move(complete));
            return;
        }

        if (tool_name == "navigate"sv) {
            auto tab_id = require_tab_id();
            auto url = arguments.get_string("url"sv);
            if (!tab_id.has_value() || !url.has_value())
                return fail_invalid_params("Missing required parameters 'tab_id' and 'url'"sv);
            bool wait_for_load = arguments.get_bool("wait_for_load"sv).value_or(true);
            delegate.navigate_tab(*tab_id, *url, wait_for_load, move(complete));
            return;
        }

        if (tool_name == "reload"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            bool wait_for_load = arguments.get_bool("wait_for_load"sv).value_or(true);
            delegate.reload_tab(*tab_id, wait_for_load, move(complete));
            return;
        }

        if (tool_name == "go_back"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            delegate.go_back(*tab_id, move(complete));
            return;
        }

        if (tool_name == "go_forward"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            delegate.go_forward(*tab_id, move(complete));
            return;
        }

        if (tool_name == "get_accessibility_tree"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            delegate.get_accessibility_tree(*tab_id, move(complete));
            return;
        }

        if (tool_name == "get_dom_tree"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            delegate.get_dom_tree(*tab_id, move(complete));
            return;
        }

        if (tool_name == "get_page_source"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            delegate.get_page_source(*tab_id, move(complete));
            return;
        }

        if (tool_name == "find_elements"sv) {
            auto tab_id = require_tab_id();
            auto selector = arguments.get_string("selector"sv);
            if (!tab_id.has_value() || !selector.has_value())
                return fail_invalid_params("Missing required parameters 'tab_id' and 'selector'"sv);
            auto strategy = arguments.get_string("strategy"sv).value_or("css selector"_string);
            delegate.find_elements(*tab_id, *selector, strategy, move(complete));
            return;
        }

        if (tool_name == "click"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            auto ref = arguments.get_string("ref"sv).copy();
            auto selector = arguments.get_string("selector"sv).copy();
            auto element_id = arguments.get_string("element_id"sv).copy();
            auto strategy = arguments.get_string("strategy"sv).value_or("css selector"_string);
            delegate.click_element(*tab_id, move(ref), move(selector), move(element_id), strategy, move(complete));
            return;
        }

        if (tool_name == "type_text"sv) {
            auto tab_id = require_tab_id();
            auto text = arguments.get_string("text"sv);
            if (!tab_id.has_value() || !text.has_value())
                return fail_invalid_params("Missing required parameters 'tab_id' and 'text'"sv);
            auto ref = arguments.get_string("ref"sv).copy();
            auto selector = arguments.get_string("selector"sv).copy();
            auto element_id = arguments.get_string("element_id"sv).copy();
            bool clear_first = arguments.get_bool("clear"sv).value_or(false);
            auto strategy = arguments.get_string("strategy"sv).value_or("css selector"_string);
            delegate.type_text(*tab_id, move(ref), move(selector), move(element_id), *text, clear_first, strategy, move(complete));
            return;
        }

        if (tool_name == "get_page_snapshot"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            auto max_elements = static_cast<size_t>(arguments.get_u64("max_elements"sv).value_or(150));
            bool include_text = arguments.get_bool("include_text"sv).value_or(true);
            delegate.get_page_snapshot(*tab_id, max_elements, include_text, move(complete));
            return;
        }

        if (tool_name == "press_key"sv) {
            auto tab_id = require_tab_id();
            auto key = arguments.get_string("key"sv);
            if (!tab_id.has_value() || !key.has_value() || key->is_empty())
                return fail_invalid_params("Missing required parameters 'tab_id' and 'key'"sv);
            delegate.press_key(*tab_id, *key, arguments.get_string("ref"sv).copy(), arguments.get_string("selector"sv).copy(), move(complete));
            return;
        }

        if (tool_name == "scroll"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            auto direction = arguments.get_string("direction"sv).value_or("down"_string);
            auto amount = static_cast<i32>(arguments.get_integer<i64>("amount"sv).value_or(600));
            delegate.scroll_page(*tab_id, move(direction), amount, arguments.get_string("ref"sv).copy(), arguments.get_string("selector"sv).copy(), move(complete));
            return;
        }

        if (tool_name == "hover"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            delegate.hover_element(*tab_id, arguments.get_string("ref"sv).copy(), arguments.get_string("selector"sv).copy(), move(complete));
            return;
        }

        if (tool_name == "drag"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            auto from_ref = arguments.get_string("from_ref"sv);
            auto from_selector = arguments.get_string("from_selector"sv);
            auto from_x = arguments.get_integer<i32>("from_x"sv);
            auto from_y = arguments.get_integer<i32>("from_y"sv);
            auto to_ref = arguments.get_string("to_ref"sv);
            auto to_selector = arguments.get_string("to_selector"sv);
            auto to_x = arguments.get_integer<i32>("to_x"sv);
            auto to_y = arguments.get_integer<i32>("to_y"sv);
            auto delta_x = arguments.get_integer<i32>("delta_x"sv);
            auto delta_y = arguments.get_integer<i32>("delta_y"sv);

            bool has_source = (from_ref.has_value() && !from_ref->is_empty())
                || (from_selector.has_value() && !from_selector->is_empty())
                || (from_x.has_value() && from_y.has_value());
            bool has_dest = (to_ref.has_value() && !to_ref->is_empty())
                || (to_selector.has_value() && !to_selector->is_empty())
                || (to_x.has_value() && to_y.has_value())
                || delta_x.has_value()
                || delta_y.has_value();
            if (!has_source || !has_dest)
                return fail_invalid_params("drag requires source (from_ref/from_selector or from_x+from_y) and destination (to_ref/to_selector, to_x+to_y, or delta_x/delta_y)"sv);

            auto steps = static_cast<u32>(min<u64>(60, max<u64>(1, arguments.get_u64("steps"sv).value_or(10))));
            auto duration_ms = static_cast<u32>(min<u64>(5000, arguments.get_u64("duration_ms"sv).value_or(250)));
            delegate.drag(*tab_id, from_ref.copy(), from_selector.copy(), from_x, from_y, to_ref.copy(), to_selector.copy(), to_x, to_y, delta_x, delta_y, steps, duration_ms, move(complete));
            return;
        }

        if (tool_name == "upload_file"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            auto ref = arguments.get_string("ref"sv);
            auto selector = arguments.get_string("selector"sv);
            auto element_id = arguments.get_string("element_id"sv);
            if ((!ref.has_value() || ref->is_empty()) && (!selector.has_value() || selector->is_empty()) && (!element_id.has_value() || element_id->is_empty()))
                return fail_invalid_params("upload_file requires at least one target ('ref', 'selector', or 'element_id')"sv);

            Vector<String> file_paths;
            if (auto paths_arr = arguments.get_array("paths"sv); paths_arr.has_value()) {
                for (size_t i = 0; i < paths_arr->size(); ++i) {
                    if (paths_arr->at(i).is_string()) {
                        auto p = paths_arr->at(i).as_string();
                        if (!p.is_empty())
                            file_paths.append(move(p));
                    }
                }
            }
            if (auto single_path = arguments.get_string("path"sv); single_path.has_value() && !single_path->is_empty())
                file_paths.append(*single_path);
            if (file_paths.is_empty())
                return fail_invalid_params("upload_file requires non-empty 'path' or 'paths'"sv);

            delegate.upload_file(*tab_id, ref.copy(), selector.copy(), element_id.copy(), move(file_paths), move(complete));
            return;
        }

        if (tool_name == "select_option"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            Optional<size_t> index;
            if (auto maybe_idx = arguments.get_u64("index"sv); maybe_idx.has_value())
                index = static_cast<size_t>(*maybe_idx);
            delegate.select_option(*tab_id, arguments.get_string("ref"sv).copy(), arguments.get_string("selector"sv).copy(), arguments.get_string("value"sv).copy(), arguments.get_string("label"sv).copy(), index, move(complete));
            return;
        }

        if (tool_name == "wait_for"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            auto timeout_ms = static_cast<u32>(arguments.get_u64("timeout_ms"sv).value_or(10000));
            delegate.wait_for(*tab_id, arguments.get_string("ref"sv).copy(), arguments.get_string("selector"sv).copy(), arguments.get_string("text"sv).copy(), timeout_ms, move(complete));
            return;
        }

        if (tool_name == "get_console_logs"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            auto limit = static_cast<size_t>(arguments.get_u64("limit"sv).value_or(100));
            bool clear = arguments.get_bool("clear"sv).value_or(false);
            delegate.get_console_logs(*tab_id, limit, clear, move(complete));
            return;
        }

        if (tool_name == "get_network_requests"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            auto limit = static_cast<size_t>(arguments.get_u64("limit"sv).value_or(100));
            bool clear = arguments.get_bool("clear"sv).value_or(false);
            bool only_errors = arguments.get_bool("only_errors"sv).value_or(false);
            delegate.get_network_requests(*tab_id, limit, clear, only_errors, move(complete));
            return;
        }

        if (tool_name == "eval_js"sv) {
            auto tab_id = require_tab_id();
            auto script = arguments.get_string("script"sv);
            if (!tab_id.has_value() || !script.has_value())
                return fail_invalid_params("Missing required parameters 'tab_id' and 'script'"sv);
            delegate.evaluate_javascript(*tab_id, *script, move(complete));
            return;
        }

        if (tool_name == "take_screenshot"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            delegate.take_screenshot(*tab_id, move(complete));
            return;
        }

        if (tool_name == "list_flows"sv) {
            delegate.list_flows(move(complete));
            return;
        }

        if (tool_name == "get_flow"sv) {
            auto flow_id = arguments.get_string("flow_id"sv);
            if (!flow_id.has_value() || flow_id->is_empty())
                return fail_invalid_params("Missing string parameter 'flow_id'"sv);
            delegate.get_flow(*flow_id, move(complete));
            return;
        }

        auto extract_flow_value = [](JsonObject const& args) -> Optional<JsonValue> {
            if (auto flow_obj = args.get_object("flow"sv); flow_obj.has_value())
                return JsonValue { *flow_obj };
            if (auto flow_str = args.get_string("flow"sv); flow_str.has_value()) {
                auto parsed = JsonValue::from_string(*flow_str);
                if (!parsed.is_error() && parsed.value().is_object())
                    return parsed.release_value();
            }
            return {};
        };

        if (tool_name == "save_flow"sv) {
            auto flow_val = extract_flow_value(arguments);
            if (!flow_val.has_value())
                return fail_invalid_params("Missing object parameter 'flow'"sv);
            delegate.save_flow(flow_val.release_value(), move(complete));
            return;
        }

        if (tool_name == "delete_flow"sv) {
            auto flow_id = arguments.get_string("flow_id"sv);
            if (!flow_id.has_value() || flow_id->is_empty())
                return fail_invalid_params("Missing string parameter 'flow_id'"sv);
            delegate.delete_flow(*flow_id, move(complete));
            return;
        }

        if (tool_name == "run_flow"sv) {
            Optional<String> flow_id;
            if (auto maybe_flow_id = arguments.get_string("flow_id"sv); maybe_flow_id.has_value() && !maybe_flow_id->is_empty())
                flow_id = *maybe_flow_id;
            auto inline_flow = extract_flow_value(arguments);
            auto tab_id = arguments.get_u64("tab_id"sv);
            JsonObject variables;
            if (auto maybe_vars = arguments.get_object("variables"sv); maybe_vars.has_value())
                variables = *maybe_vars;
            bool open_new_tab = arguments.get_bool("open_new_tab"sv).value_or(true);
            bool wait = arguments.get_bool("wait"sv).value_or(true);
            bool close_tab_on_finish = arguments.get_bool("close_tab_on_finish"sv).value_or(false);
            delegate.run_flow(move(flow_id), move(inline_flow), tab_id, move(variables), open_new_tab, wait, close_tab_on_finish, move(complete));
            return;
        }

        if (tool_name == "list_flow_runs"sv) {
            Optional<String> flow_id;
            if (auto maybe_flow_id = arguments.get_string("flow_id"sv); maybe_flow_id.has_value() && !maybe_flow_id->is_empty())
                flow_id = *maybe_flow_id;
            auto limit = static_cast<size_t>(arguments.get_u64("limit"sv).value_or(20));
            delegate.list_flow_runs(move(flow_id), limit, move(complete));
            return;
        }

        if (tool_name == "get_flow_run"sv) {
            auto run_id = arguments.get_string("run_id"sv);
            if (!run_id.has_value() || run_id->is_empty())
                return fail_invalid_params("Missing string parameter 'run_id'"sv);
            delegate.get_flow_run(*run_id, move(complete));
            return;
        }

        if (tool_name == "cancel_flow_run"sv) {
            auto run_id = arguments.get_string("run_id"sv);
            if (!run_id.has_value() || run_id->is_empty())
                return fail_invalid_params("Missing string parameter 'run_id'"sv);
            delegate.cancel_flow_run(*run_id, move(complete));
            return;
        }

        if (tool_name == "list_extensions"sv) {
            delegate.list_extensions(move(complete));
            return;
        }

        if (tool_name == "install_unpacked_extension"sv) {
            auto path = arguments.get_string("path"sv);
            if (!path.has_value() || path->is_empty())
                return fail_invalid_params("Missing string parameter 'path'"sv);
            delegate.install_unpacked_extension(*path, move(complete));
            return;
        }

        if (tool_name == "set_extension_enabled"sv) {
            auto ext_id = arguments.get_string("extension_id"sv);
            auto enabled = arguments.get_bool("enabled"sv);
            if (!ext_id.has_value() || ext_id->is_empty() || !enabled.has_value())
                return fail_invalid_params("Missing required parameters 'extension_id' and 'enabled'"sv);
            delegate.set_extension_enabled(*ext_id, *enabled, move(complete));
            return;
        }

        if (tool_name == "reload_extension"sv) {
            auto ext_id = arguments.get_string("extension_id"sv);
            if (!ext_id.has_value() || ext_id->is_empty())
                return fail_invalid_params("Missing string parameter 'extension_id'"sv);
            delegate.reload_extension(*ext_id, move(complete));
            return;
        }

        if (tool_name == "remove_extension"sv) {
            auto ext_id = arguments.get_string("extension_id"sv);
            if (!ext_id.has_value() || ext_id->is_empty())
                return fail_invalid_params("Missing string parameter 'extension_id'"sv);
            delegate.remove_extension(*ext_id, move(complete));
            return;
        }

        if (tool_name == "inject_extension"sv) {
            auto ext_id = arguments.get_string("extension_id"sv);
            if (!ext_id.has_value() || ext_id->is_empty())
                return fail_invalid_params("Missing string parameter 'extension_id'"sv);
            auto tab_id = arguments.get_u64("tab_id"sv);
            if (arguments.has("tab_id"sv) && (!tab_id.has_value() || *tab_id == 0))
                return fail_invalid_params("Parameter 'tab_id' must be a positive integer"sv);
            delegate.inject_extension(*ext_id, tab_id, move(complete));
            return;
        }

        if (tool_name == "list_page_tools"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            bool include_declarative = arguments.get_bool("include_declarative"sv).value_or(true);
            delegate.list_page_tools(*tab_id, include_declarative, move(complete));
            return;
        }

        if (tool_name == "call_page_tool"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            auto name = arguments.get_string("name"sv);
            if (!name.has_value() || name->is_empty())
                return fail_invalid_params("Missing string parameter 'name'"sv);

            JsonObject tool_args;
            if (auto arg_obj = arguments.get_object("arguments"sv); arg_obj.has_value()) {
                tool_args = *arg_obj;
            } else if (auto arg_str = arguments.get_string("arguments"sv); arg_str.has_value()) {
                auto parsed = JsonValue::from_string(*arg_str);
                if (parsed.is_error() || !parsed.value().is_object())
                    return fail_invalid_params("Parameter 'arguments' must be a JSON object"sv);
                tool_args = parsed.release_value().as_object();
            } else if (arguments.has("arguments"sv) && !arguments.get("arguments"sv)->is_null()) {
                return fail_invalid_params("Parameter 'arguments' must be a JSON object"sv);
            }

            bool confirm_consequential = arguments.get_bool("confirm_consequential"sv).value_or(false);
            delegate.call_page_tool(*tab_id, *name, move(tool_args), confirm_consequential, move(complete));
            return;
        }

        if (tool_name == "list_skills"sv) {
            auto tab_id = arguments.get_u64("tab_id"sv);
            if (arguments.has("tab_id"sv) && (!tab_id.has_value() || *tab_id == 0))
                return fail_invalid_params("Parameter 'tab_id' must be a positive integer"sv);
            Optional<String> url;
            if (arguments.has("url"sv) && !arguments.get("url"sv)->is_null()) {
                auto raw_url = arguments.get_string("url"sv);
                if (!raw_url.has_value())
                    return fail_invalid_params("Parameter 'url' must be a string"sv);
                url = *raw_url;
            }
            bool include_disabled = arguments.get_bool("include_disabled"sv).value_or(false);
            delegate.list_skills_tool(tab_id, move(url), include_disabled, move(complete));
            return;
        }

        if (tool_name == "read_skill"sv) {
            Optional<String> name;
            Optional<String> uri;
            Optional<String> file;
            if (arguments.has("name"sv) && !arguments.get("name"sv)->is_null()) {
                auto raw_name = arguments.get_string("name"sv);
                if (!raw_name.has_value())
                    return fail_invalid_params("Parameter 'name' must be a string"sv);
                name = *raw_name;
            }
            if (arguments.has("uri"sv) && !arguments.get("uri"sv)->is_null()) {
                auto raw_uri = arguments.get_string("uri"sv);
                if (!raw_uri.has_value())
                    return fail_invalid_params("Parameter 'uri' must be a string"sv);
                uri = *raw_uri;
            }
            if (arguments.has("file"sv) && !arguments.get("file"sv)->is_null()) {
                auto raw_file = arguments.get_string("file"sv);
                if (!raw_file.has_value())
                    return fail_invalid_params("Parameter 'file' must be a string"sv);
                file = *raw_file;
            }
            if ((!name.has_value() || name->is_empty()) && (!uri.has_value() || uri->is_empty()))
                return fail_invalid_params("Provide at least one non-empty parameter: 'name' or 'uri'"sv);
            delegate.read_skill_tool(move(name), move(uri), move(file), move(complete));
            return;
        }

        if (tool_name == "save_skill"sv) {
            auto skill_val = arguments.get("skill"sv);
            if (!skill_val.has_value() || !skill_val->is_object())
                return fail_invalid_params("Missing object parameter 'skill'"sv);
            delegate.save_skill_tool(*skill_val, move(complete));
            return;
        }

        if (tool_name == "delete_skill"sv) {
            auto name = arguments.get_string("name"sv);
            if (!name.has_value() || name->is_empty())
                return fail_invalid_params("Missing string parameter 'name'"sv);
            delegate.delete_skill_tool(*name, move(complete));
            return;
        }

        if (tool_name == "list_credentials"sv) {
            auto tab_id = arguments.get_u64("tab_id"sv);
            if (arguments.has("tab_id"sv) && (!tab_id.has_value() || *tab_id == 0))
                return fail_invalid_params("Parameter 'tab_id' must be a positive integer"sv);
            delegate.list_credentials_tool(tab_id, move(complete));
            return;
        }

        if (tool_name == "fill_credential"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            auto credential_id = arguments.get_string("credential_id"sv);
            auto field = arguments.get_string("field"sv);
            if (!credential_id.has_value() || credential_id->is_empty() || !field.has_value() || field->is_empty())
                return fail_invalid_params("Missing required parameters 'credential_id' and 'field'"sv);
            if (*field != "username"sv && *field != "password"sv && *field != "totp"sv)
                return fail_invalid_params("Parameter 'field' must be one of: username, password, totp"sv);

            Optional<String> ref;
            if (arguments.has("ref"sv) && !arguments.get("ref"sv)->is_null()) {
                auto raw = arguments.get_string("ref"sv);
                if (!raw.has_value())
                    return fail_invalid_params("Parameter 'ref' must be a string"sv);
                ref = *raw;
            }
            Optional<String> selector;
            if (arguments.has("selector"sv) && !arguments.get("selector"sv)->is_null()) {
                auto raw = arguments.get_string("selector"sv);
                if (!raw.has_value())
                    return fail_invalid_params("Parameter 'selector' must be a string"sv);
                selector = *raw;
            }
            delegate.fill_credential_tool(*tab_id, *credential_id, *field, move(ref), move(selector), move(complete));
            return;
        }

        if (tool_name == "login_with_credential"sv) {
            auto tab_id = require_tab_id();
            if (!tab_id.has_value())
                return fail_invalid_params("Missing integer parameter 'tab_id'"sv);
            auto credential_id = arguments.get_string("credential_id"sv);
            if (!credential_id.has_value() || credential_id->is_empty())
                return fail_invalid_params("Missing string parameter 'credential_id'"sv);

            auto get_opt_str = [&](StringView key, Optional<String>& out) -> bool {
                if (!arguments.has(key) || arguments.get(key)->is_null())
                    return true;
                auto val = arguments.get_string(key);
                if (!val.has_value())
                    return false;
                out = *val;
                return true;
            };
            Optional<String> username_ref;
            Optional<String> username_selector;
            Optional<String> password_ref;
            Optional<String> password_selector;
            Optional<String> totp_ref;
            Optional<String> totp_selector;
            Optional<String> submit_ref;
            Optional<String> submit_selector;
            if (!get_opt_str("username_ref"sv, username_ref)
                || !get_opt_str("username_selector"sv, username_selector)
                || !get_opt_str("password_ref"sv, password_ref)
                || !get_opt_str("password_selector"sv, password_selector)
                || !get_opt_str("totp_ref"sv, totp_ref)
                || !get_opt_str("totp_selector"sv, totp_selector)
                || !get_opt_str("submit_ref"sv, submit_ref)
                || !get_opt_str("submit_selector"sv, submit_selector)) {
                return fail_invalid_params("Ref/selector parameters must be strings"sv);
            }
            bool wait_for_navigation = arguments.get_bool("wait_for_navigation"sv).value_or(true);
            delegate.login_with_credential_tool(
                *tab_id,
                *credential_id,
                move(username_ref),
                move(username_selector),
                move(password_ref),
                move(password_selector),
                move(totp_ref),
                move(totp_selector),
                move(submit_ref),
                move(submit_selector),
                wait_for_navigation,
                move(complete));
            return;
        }

        fail_invalid_params("Unknown tool name"sv);
    }

    void send_json_rpc_result(JsonValue const& id, JsonValue result, StringView extra_headers = {})
    {
        JsonObject response;
        response.set("jsonrpc"sv, "2.0"sv);
        response.set("id"sv, id);
        response.set("result"sv, move(result));
        auto serialized = response.serialized();
        send_raw_http_response(200, "application/json; charset=utf-8"sv, serialized.bytes_as_string_view(), extra_headers);
    }

    void send_json_rpc_error(JsonValue const& id, i64 code, StringView message, u32 http_status = 200)
    {
        JsonObject error;
        error.set("code"sv, code);
        error.set("message"sv, message);

        JsonObject response;
        response.set("jsonrpc"sv, "2.0"sv);
        response.set("id"sv, id);
        response.set("error"sv, move(error));
        auto serialized = response.serialized();
        send_raw_http_response(http_status, "application/json; charset=utf-8"sv, serialized.bytes_as_string_view());
    }

    void send_http_text_error(u32 status_code, StringView message, StringView extra_headers = {})
    {
        JsonObject body;
        body.set("error"sv, message);
        auto serialized = body.serialized();
        send_raw_http_response(status_code, "application/json; charset=utf-8"sv, serialized.bytes_as_string_view(), extra_headers);
    }

    void send_raw_http_response(u32 status_code, StringView content_type, StringView body, StringView extra_headers = {})
    {
        if (m_closed)
            return;

        StringBuilder builder;
        builder.appendff("HTTP/1.1 {} {}\r\n", status_code, HTTP::reason_phrase_for_code(status_code));
        builder.append("Connection: close\r\n"sv);
        builder.append("Cache-Control: no-store\r\n"sv);
        builder.append("X-Content-Type-Options: nosniff\r\n"sv);
        builder.appendff("Content-Type: {}\r\n", content_type);
        builder.appendff("Content-Length: {}\r\n", body.length());
        if (!extra_headers.is_empty())
            builder.append(extra_headers);
        builder.append("\r\n"sv);
        builder.append(body);

        if (auto result = m_socket->write_until_depleted(builder.string_view()); result.is_error())
            warnln("Failed to write MCP HTTP response: {}", result.error());

        close_later();
    }

    WeakPtr<MCPServer> m_server;
    NonnullOwnPtr<Core::BufferedTCPSocket> m_socket;
    RefPtr<Core::Timer> m_read_timeout_timer;
    StringBuilder m_remaining_request;
    bool m_request_handled { false };
    bool m_closed { false };
    bool m_close_scheduled { false };
};

ErrorOr<NonnullOwnPtr<MCPServer>> MCPServer::create(MCPDelegate& delegate, u16 port, String auth_token)
{
    auto address = IPv4Address { 127, 0, 0, 1 };
    auto server = TRY(Core::TCPServer::try_create());
    TRY(server->listen(address, port, Core::TCPServer::AllowAddressReuse::Yes));

    return adopt_own(*new MCPServer(delegate, move(server), move(auth_token), generate_random_uuid()));
}

MCPServer::MCPServer(MCPDelegate& delegate, NonnullRefPtr<Core::TCPServer> server, String auth_token, String session_id)
    : m_delegate(delegate)
    , m_server(move(server))
    , m_auth_token(move(auth_token))
    , m_session_id(move(session_id))
{
    m_server->on_ready_to_accept = [weak_self = make_weak_ptr<MCPServer>()] {
        if (!weak_self)
            return;

        if (auto result = weak_self->on_new_client(); result.is_error())
            warnln("Failed to accept MCP client: {}", result.error());
    };
}

MCPServer::~MCPServer()
{
    m_is_shutting_down = true;
    m_server->on_ready_to_accept = {};

    for (auto const& connection : m_connections)
        connection->close();
    m_connections.clear();
}

Optional<u16> MCPServer::local_port() const
{
    return m_server->local_port();
}

ErrorOr<void> MCPServer::on_new_client()
{
    auto client = TRY(m_server->accept());
    if (m_connections.size() >= MAX_ACTIVE_CONNECTIONS) {
        (void)client->write_until_depleted("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: 24\r\n\r\nToo many MCP connections"sv.bytes());
        client->close();
        return {};
    }
    auto connection = TRY(Connection::create(*this, move(client)));
    m_connections.set(move(connection));
    return {};
}

void MCPServer::remove_connection(Connection& connection)
{
    if (m_is_shutting_down)
        return;

    m_connections.remove_all_matching([&](auto const& entry) {
        return entry.ptr() == &connection;
    });
}

}
