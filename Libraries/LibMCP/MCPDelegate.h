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

struct SkillMatchHint {
    String name {};
    String uri {};
    String description {};
};

struct CredentialMatchHint {
    String id {};
    String label {};
    Vector<String> fields {};
};

struct TabInfo {
    u64 id { 0 };
    String title {};
    String url {};
    String container_id {};
    String mcp_access_mode { "full"_string };
    bool is_loading { false };
    bool is_active { false };
    Vector<SkillMatchHint> matching_skills {};
    Vector<CredentialMatchHint> matching_credentials {};
};

struct ContainerInfo {
    String id {};
    String name {};
    String color {};
    bool is_default_mcp { false };
    bool mcp_allowed { true };
    bool ephemeral { false };
};

struct ToolCallResult {
    bool is_error { false };
    String text {};
    Optional<String> image_base64_png {};
};

struct ProtocolCallResult {
    bool is_error { false };
    i32 error_code { -32603 };
    String error_message {};
    JsonObject result {};
};

using OnToolCallComplete = Function<void(ToolCallResult)>;
using OnProtocolCallComplete = Function<void(ProtocolCallResult)>;

class MCP_API MCPDelegate {
public:
    virtual ~MCPDelegate() = default;

    virtual Vector<TabInfo> list_allowed_tabs() const = 0;
    virtual Optional<TabInfo> active_allowed_tab() const = 0;
    virtual Vector<ContainerInfo> list_containers() const = 0;
    virtual String active_profile_name() const { return {}; }
    virtual Optional<String> active_ephemeral_container_id() const { return {}; }
    virtual bool ephemeral_default_enabled() const { return true; }
    virtual bool webmcp_only_mode() const { return false; }
    virtual void on_mcp_session_closed() { }
    virtual void create_container_tool(String name, Optional<String> color, Optional<String> id, OnToolCallComplete) = 0;

    virtual void open_tab(Optional<String> url, bool activate, bool wait_for_load, Optional<String> container_id, OnToolCallComplete) = 0;
    virtual void migrate_tab_container(u64 tab_id, String target_container_id, bool activate, bool wait_for_load, bool close_source_tab, bool create_if_missing, Optional<String> target_name, Optional<String> target_color, OnToolCallComplete) = 0;
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
    virtual void click_element(u64 tab_id, Optional<String> ref, Optional<String> selector, Optional<String> element_id, String strategy, Optional<i32> x, Optional<i32> y, String button, u32 click_count, Vector<String> modifiers, OnToolCallComplete) = 0;
    virtual void type_text(u64 tab_id, Optional<String> ref, Optional<String> selector, Optional<String> element_id, String text, bool clear_first, String strategy, bool use_keyboard, OnToolCallComplete) = 0;
    virtual void get_page_snapshot(u64 tab_id, size_t max_elements, bool include_text, Optional<String> root_selector, OnToolCallComplete) = 0;
    virtual void get_page_markdown(u64 tab_id, Optional<String> root_selector, bool main_content_only, bool include_links, bool include_images, size_t max_chars, OnToolCallComplete on_complete)
    {
        (void)tab_id;
        (void)root_selector;
        (void)main_content_only;
        (void)include_links;
        (void)include_images;
        (void)max_chars;
        on_complete({ .is_error = true, .text = "get_page_markdown not implemented"_string });
    }
    virtual void press_key(u64 tab_id, String key, Optional<String> ref, Optional<String> selector, Vector<String> modifiers, OnToolCallComplete) = 0;
    virtual void scroll_page(u64 tab_id, String direction, i32 amount, Optional<String> ref, Optional<String> selector, OnToolCallComplete) = 0;
    virtual void wheel(u64 tab_id, Optional<i32> x, Optional<i32> y, Optional<String> ref, Optional<String> selector, i32 delta_x, i32 delta_y, Vector<String> modifiers, u32 duration_ms, OnToolCallComplete) = 0;
    virtual void hover_element(u64 tab_id, Optional<String> ref, Optional<String> selector, Optional<i32> x, Optional<i32> y, OnToolCallComplete) = 0;
    virtual void drag(u64 tab_id, Optional<String> from_ref, Optional<String> from_selector, Optional<i32> from_x, Optional<i32> from_y, Optional<String> to_ref, Optional<String> to_selector, Optional<i32> to_x, Optional<i32> to_y, Optional<i32> delta_x, Optional<i32> delta_y, u32 steps, u32 duration_ms, Vector<String> modifiers, OnToolCallComplete) = 0;
    virtual void upload_file(u64 tab_id, Optional<String> ref, Optional<String> selector, Optional<String> element_id, Vector<String> file_paths, OnToolCallComplete) = 0;
    virtual void list_upload_files(OnToolCallComplete) = 0;
    virtual void select_option(u64 tab_id, Optional<String> ref, Optional<String> selector, Optional<String> value, Optional<String> label, Optional<size_t> index, OnToolCallComplete) = 0;
    virtual void wait_for(u64 tab_id, Optional<String> ref, Optional<String> selector, Optional<String> text, u32 timeout_ms, OnToolCallComplete) = 0;
    virtual void list_notifications(Optional<u64> tab_id, Optional<String> origin, size_t limit, bool clear, OnToolCallComplete) = 0;
    virtual void wait_for_notification(Optional<u64> tab_id, Optional<String> origin, Optional<String> title_contains, Optional<String> body_contains, Optional<String> event_type, u32 timeout_ms, OnToolCallComplete) = 0;
    virtual void get_console_logs(u64 tab_id, size_t limit, bool clear, OnToolCallComplete) = 0;
    virtual void get_network_requests(u64 tab_id, size_t limit, bool clear, bool only_errors, OnToolCallComplete) = 0;
    virtual void add_network_rule(JsonObject rule_args, OnToolCallComplete on_complete)
    {
        (void)rule_args;
        on_complete({ .is_error = true, .text = "add_network_rule not implemented"_string });
    }
    virtual void list_network_rules(OnToolCallComplete on_complete)
    {
        on_complete({ .is_error = true, .text = "list_network_rules not implemented"_string });
    }
    virtual void clear_network_rules(Optional<String> rule_id, OnToolCallComplete on_complete)
    {
        (void)rule_id;
        on_complete({ .is_error = true, .text = "clear_network_rules not implemented"_string });
    }
    virtual void evaluate_javascript(u64 tab_id, String script, OnToolCallComplete) = 0;
    virtual void take_screenshot(u64 tab_id, bool redact_media, OnToolCallComplete) = 0;

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
    virtual void inject_extension(String extension_id, Optional<u64> tab_id, OnToolCallComplete) = 0;

    virtual void list_page_tools(u64 tab_id, bool include_declarative, OnToolCallComplete) = 0;
    virtual void call_page_tool(u64 tab_id, String tool_name, JsonObject arguments, bool confirm_consequential, OnToolCallComplete) = 0;
    virtual void inspect_site_manifests(u64 tab_id, bool include_content, size_t max_bytes_per_manifest, OnToolCallComplete) = 0;

    virtual void skills_list(Optional<String> cursor, OnProtocolCallComplete) = 0;
    virtual void skills_get(String uri, OnProtocolCallComplete) = 0;
    virtual void resources_read(String uri, OnProtocolCallComplete) = 0;
    virtual void resources_directory_read(String uri, Optional<String> cursor, OnProtocolCallComplete) = 0;

    virtual void list_skills_tool(Optional<u64> tab_id, Optional<String> url, bool include_disabled, OnToolCallComplete) = 0;
    virtual void read_skill_tool(Optional<String> name, Optional<String> uri, Optional<String> file, OnToolCallComplete) = 0;
    virtual void save_skill_tool(JsonValue skill_json, OnToolCallComplete) = 0;
    virtual void delete_skill_tool(String name, OnToolCallComplete) = 0;

    virtual void list_credentials_tool(Optional<u64> tab_id, OnToolCallComplete) = 0;
    virtual void fill_credential_tool(u64 tab_id, String credential_id, String field, Optional<String> ref, Optional<String> selector, OnToolCallComplete) = 0;
    virtual void login_with_credential_tool(u64 tab_id, String credential_id, Optional<String> username_ref, Optional<String> username_selector, Optional<String> password_ref, Optional<String> password_selector, Optional<String> totp_ref, Optional<String> totp_selector, Optional<String> submit_ref, Optional<String> submit_selector, bool wait_for_navigation, OnToolCallComplete) = 0;

    virtual void on_mcp_tool_audit(StringView tool_name, Optional<u64> tab_id, bool is_error, StringView summary)
    {
        (void)tool_name;
        (void)tab_id;
        (void)is_error;
        (void)summary;
    }
};

}
