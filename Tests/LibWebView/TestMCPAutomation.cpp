/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IPv4Address.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Socket.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibMCP/MCPDelegate.h>
#include <LibMCP/MCPServer.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Automation/FlowStore.h>
#include <LibWebView/Automation/FlowTypes.h>
#include <LibWebView/Extensions/ExtensionStore.h>
#include <LibWebView/Extensions/ExtensionTypes.h>
#include <LibWebView/Skills/SkillStore.h>
#include <LibWebView/Skills/SkillTypes.h>
#include <LibWebView/Utilities.h>

namespace {

ByteString make_temp_mcp_dir()
{
    auto dir = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("ladybird-mcp-test-{}", get_random<u64>())).string();
    MUST(Core::Directory::create(dir, Core::Directory::CreateDirectories::Yes));
    return dir;
}

MCP::ToolCallResult ok_json_result(JsonValue value)
{
    MCP::ToolCallResult result;
    result.is_error = false;
    result.text = value.serialized();
    return result;
}

MCP::ToolCallResult error_result(StringView message)
{
    MCP::ToolCallResult result;
    result.is_error = true;
    result.text = String::from_utf8_without_validation(message.bytes());
    return result;
}

class FakeMCPDelegate final : public MCP::MCPDelegate {
public:
    explicit FakeMCPDelegate(ByteString storage_dir)
        : m_store(LexicalPath::join(storage_dir, "flows"sv).string())
        , m_ext_store(LexicalPath::join(storage_dir, "extensions"sv).string())
        , m_skill_store(LexicalPath::join(storage_dir, "skills"sv).string())
    {
    }

    WebView::FlowStore& store() { return m_store; }
    WebView::SkillStore& skill_store() { return m_skill_store; }
    bool allow_skill_write { false };
    Vector<MCP::TabInfo> mock_tabs;

    struct LastRunCall {
        Optional<String> flow_id;
        bool has_inline_flow { false };
        Optional<u64> tab_id;
        HashMap<String, String> variables;
        bool open_new_tab { false };
        bool wait { true };
        bool close_tab_on_finish { false };
    };
    Optional<LastRunCall> last_run_call;

    struct RecordedAction {
        String tool;
        Optional<String> ref;
        Optional<String> selector;
        Optional<String> key;
    };
    Optional<RecordedAction> last_action;

    virtual Vector<MCP::TabInfo> list_allowed_tabs() const override
    {
        Vector<MCP::TabInfo> out;
        for (auto tab : mock_tabs) {
            if (auto parsed = URL::Parser::basic_parse(tab.url); parsed.has_value()) {
                for (auto const& s : m_skill_store.matching_skills_for_url(*parsed)) {
                    tab.matching_skills.append({
                        .name = s.name,
                        .uri = WebView::skill_uri(s.name, "SKILL.md"sv),
                        .description = s.description,
                    });
                }
            }
            out.append(move(tab));
        }
        return out;
    }
    virtual Optional<MCP::TabInfo> active_allowed_tab() const override
    {
        auto tabs = list_allowed_tabs();
        for (auto const& tab : tabs) {
            if (tab.is_active)
                return tab;
        }
        return {};
    }

    virtual void open_tab(Optional<String>, bool, bool, Optional<String>, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }
    virtual void switch_tab(u64, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }
    virtual void close_tab(u64, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }
    virtual void navigate_tab(u64, String, bool, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }
    virtual void reload_tab(u64, bool, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }
    virtual void go_back(u64, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }
    virtual void go_forward(u64, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }
    virtual void get_accessibility_tree(u64, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }
    virtual void get_dom_tree(u64, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }
    virtual void get_page_source(u64, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }
    virtual void find_elements(u64, String, String, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }
    virtual void click_element(u64, Optional<String> ref, Optional<String> selector, Optional<String>, String, MCP::OnToolCallComplete cb) override
    {
        last_action = RecordedAction { "click"_string, ref, selector, {} };
        JsonObject out;
        out.set("clicked"sv, true);
        if (ref.has_value())
            out.set("ref"sv, *ref);
        cb(ok_json_result(JsonValue { move(out) }));
    }
    virtual void type_text(u64, Optional<String> ref, Optional<String> selector, Optional<String>, String, bool, String, MCP::OnToolCallComplete cb) override
    {
        last_action = RecordedAction { "type_text"_string, ref, selector, {} };
        JsonObject out;
        out.set("typed"sv, true);
        if (ref.has_value())
            out.set("ref"sv, *ref);
        cb(ok_json_result(JsonValue { move(out) }));
    }
    virtual void get_page_snapshot(u64 tab_id, size_t max_elements, bool include_text, MCP::OnToolCallComplete cb) override
    {
        JsonArray elements;
        JsonObject btn;
        btn.set("ref"sv, "@e1"sv);
        btn.set("role"sv, "button"sv);
        btn.set("text"sv, "Submit"sv);
        elements.must_append(move(btn));

        JsonObject out;
        out.set("tab_id"sv, tab_id);
        out.set("max_elements"sv, static_cast<u64>(max_elements));
        out.set("include_text"sv, include_text);
        out.set("element_count"sv, 1);
        out.set("elements"sv, move(elements));
        cb(ok_json_result(JsonValue { move(out) }));
    }
    virtual void press_key(u64, String key, Optional<String> ref, Optional<String> selector, MCP::OnToolCallComplete cb) override
    {
        last_action = RecordedAction { "press_key"_string, ref, selector, key };
        JsonObject out;
        out.set("pressed"sv, true);
        out.set("key"sv, move(key));
        cb(ok_json_result(JsonValue { move(out) }));
    }
    virtual void scroll_page(u64, String direction, i32 amount, Optional<String> ref, Optional<String> selector, MCP::OnToolCallComplete cb) override
    {
        last_action = RecordedAction { "scroll"_string, ref, selector, {} };
        JsonObject out;
        out.set("scrolled"sv, true);
        out.set("direction"sv, move(direction));
        out.set("amount"sv, static_cast<i64>(amount));
        cb(ok_json_result(JsonValue { move(out) }));
    }
    virtual void hover_element(u64, Optional<String> ref, Optional<String> selector, MCP::OnToolCallComplete cb) override
    {
        last_action = RecordedAction { "hover"_string, ref, selector, {} };
        JsonObject out;
        out.set("hovered"sv, true);
        cb(ok_json_result(JsonValue { move(out) }));
    }
    virtual void select_option(u64, Optional<String> ref, Optional<String> selector, Optional<String> value, Optional<String>, Optional<size_t>, MCP::OnToolCallComplete cb) override
    {
        last_action = RecordedAction { "select_option"_string, ref, selector, {} };
        JsonObject out;
        out.set("selected"sv, true);
        if (value.has_value())
            out.set("selected_value"sv, *value);
        cb(ok_json_result(JsonValue { move(out) }));
    }
    virtual void wait_for(u64, Optional<String> ref, Optional<String> selector, Optional<String> text, u32 timeout_ms, MCP::OnToolCallComplete cb) override
    {
        last_action = RecordedAction { "wait_for"_string, ref, selector, text };
        JsonObject out;
        out.set("satisfied"sv, true);
        out.set("timeout_ms"sv, static_cast<u64>(timeout_ms));
        cb(ok_json_result(JsonValue { move(out) }));
    }
    virtual void drag(u64, Optional<String> from_ref, Optional<String> from_selector, Optional<i32> from_x, Optional<i32> from_y, Optional<String>, Optional<String>, Optional<i32> to_x, Optional<i32> to_y, Optional<i32> delta_x, Optional<i32> delta_y, u32 steps, u32 duration_ms, MCP::OnToolCallComplete cb) override
    {
        last_action = RecordedAction { "drag"_string, from_ref, from_selector, {} };
        JsonObject out;
        out.set("dragged"sv, true);
        JsonObject from_obj;
        from_obj.set("x"sv, from_x.value_or(10));
        from_obj.set("y"sv, from_y.value_or(20));
        JsonObject to_obj;
        to_obj.set("x"sv, to_x.value_or(from_x.value_or(10) + delta_x.value_or(0)));
        to_obj.set("y"sv, to_y.value_or(from_y.value_or(20) + delta_y.value_or(0)));
        out.set("from"sv, move(from_obj));
        out.set("to"sv, move(to_obj));
        out.set("steps"sv, static_cast<u64>(steps));
        out.set("duration_ms"sv, static_cast<u64>(duration_ms));
        cb(ok_json_result(JsonValue { move(out) }));
    }
    virtual void upload_file(u64, Optional<String> ref, Optional<String> selector, Optional<String> element_id, Vector<String> file_paths, MCP::OnToolCallComplete cb) override
    {
        last_action = RecordedAction { "upload_file"_string, ref, selector, element_id };
        JsonObject out;
        out.set("uploaded"sv, true);
        out.set("file_count"sv, static_cast<u64>(file_paths.size()));
        JsonArray paths;
        for (auto const& p : file_paths)
            paths.must_append(p);
        out.set("paths"sv, move(paths));
        cb(ok_json_result(JsonValue { move(out) }));
    }
    virtual void get_console_logs(u64 tab_id, size_t, bool, MCP::OnToolCallComplete cb) override
    {
        JsonArray logs;
        JsonObject entry;
        entry.set("level"sv, "error"sv);
        entry.set("text"sv, "TypeError: test"sv);
        logs.must_append(move(entry));
        JsonObject out;
        out.set("tab_id"sv, tab_id);
        out.set("count"sv, 1);
        out.set("logs"sv, move(logs));
        cb(ok_json_result(JsonValue { move(out) }));
    }
    virtual void get_network_requests(u64 tab_id, size_t, bool, bool only_errors, MCP::OnToolCallComplete cb) override
    {
        JsonArray reqs;
        JsonObject req;
        req.set("url"sv, "https://example.com/api"sv);
        req.set("status_code"sv, only_errors ? 500 : 200);
        reqs.must_append(move(req));
        JsonObject out;
        out.set("tab_id"sv, tab_id);
        out.set("count"sv, 1);
        out.set("requests"sv, move(reqs));
        cb(ok_json_result(JsonValue { move(out) }));
    }
    virtual void evaluate_javascript(u64, String, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }
    virtual void take_screenshot(u64, MCP::OnToolCallComplete cb) override { cb(error_result("unsupported"sv)); }

    virtual void list_flows(MCP::OnToolCallComplete cb) override
    {
        JsonArray flows;
        for (auto const& flow : m_store.list_flows())
            flows.must_append(WebView::serialize_flow_summary(flow));
        JsonObject out;
        out.set("flows"sv, move(flows));
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void get_flow(String flow_id, MCP::OnToolCallComplete cb) override
    {
        auto flow = m_store.get_flow(flow_id);
        if (!flow.has_value()) {
            cb(error_result("Flow not found"sv));
            return;
        }
        cb(ok_json_result(JsonValue { WebView::serialize_flow_definition(*flow) }));
    }

    virtual void save_flow(JsonValue flow_json, MCP::OnToolCallComplete cb) override
    {
        auto parsed = WebView::parse_flow_definition(flow_json);
        if (parsed.is_error()) {
            cb(error_result(MUST(String::formatted("Invalid flow definition: {}", parsed.error()))));
            return;
        }
        auto saved = m_store.save_flow(parsed.release_value());
        if (saved.is_error()) {
            cb(error_result(MUST(String::formatted("Failed to save flow: {}", saved.error()))));
            return;
        }
        JsonObject out;
        out.set("saved"sv, true);
        out.set("flow"sv, WebView::serialize_flow_definition(saved.value()));
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void delete_flow(String flow_id, MCP::OnToolCallComplete cb) override
    {
        if (!m_store.get_flow(flow_id).has_value()) {
            cb(error_result("Flow not found"sv));
            return;
        }
        if (auto res = m_store.delete_flow(flow_id); res.is_error()) {
            cb(error_result(MUST(String::formatted("Failed to delete flow: {}", res.error()))));
            return;
        }
        JsonObject out;
        out.set("deleted"sv, true);
        out.set("flow_id"sv, flow_id);
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void run_flow(Optional<String> flow_id, Optional<JsonValue> inline_flow, Optional<u64> tab_id, JsonObject variables, bool open_new_tab, bool wait, bool close_tab_on_finish, MCP::OnToolCallComplete cb) override
    {
        LastRunCall call;
        call.flow_id = flow_id;
        call.has_inline_flow = inline_flow.has_value();
        call.tab_id = tab_id;
        call.open_new_tab = open_new_tab;
        call.wait = wait;
        call.close_tab_on_finish = close_tab_on_finish;
        variables.for_each_member([&](String const& key, JsonValue const& val) {
            if (val.is_string())
                call.variables.set(key, val.as_string());
        });
        last_run_call = move(call);

        Optional<WebView::FlowDefinition> flow;
        if (inline_flow.has_value()) {
            auto parsed = WebView::parse_flow_definition(*inline_flow);
            if (parsed.is_error()) {
                cb(error_result("Invalid inline flow"sv));
                return;
            }
            flow = parsed.release_value();
        } else if (flow_id.has_value()) {
            flow = m_store.get_flow(*flow_id);
        }
        if (!flow.has_value()) {
            cb(error_result("Flow not found"sv));
            return;
        }

        auto run_id = MUST(String::formatted("run-test-{}", ++m_run_counter));
        WebView::FlowRunRecord record;
        record.run_id = run_id;
        record.flow_id = flow->id;
        record.flow_name = flow->name;
        record.status = wait ? "passed"_string : "running"_string;
        record.tab_id = tab_id.value_or(101u);
        record.started_at_ms = 1000;
        record.total_steps = flow->steps.size();
        if (wait) {
            record.finished_at_ms = 1020;
            record.duration_ms = 20;
        }
        m_store.upsert_run(record, false);

        if (!wait) {
            m_active_runs.append(run_id);
            JsonObject out;
            out.set("started"sv, true);
            out.set("run_id"sv, run_id);
            out.set("status"sv, "running"sv);
            out.set("tab_id"sv, *record.tab_id);
            cb(ok_json_result(JsonValue { move(out) }));
            return;
        }

        cb(ok_json_result(JsonValue { WebView::serialize_flow_run(record) }));
    }

    virtual void list_flow_runs(Optional<String> flow_id, size_t limit, MCP::OnToolCallComplete cb) override
    {
        auto runs = m_store.list_runs(flow_id.has_value() ? Optional<StringView> { *flow_id } : OptionalNone {}, limit);
        JsonArray array;
        for (auto const& run : runs)
            array.must_append(WebView::serialize_flow_run(run));
        JsonObject out;
        out.set("runs"sv, move(array));
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void get_flow_run(String run_id, MCP::OnToolCallComplete cb) override
    {
        auto run = m_store.get_run(run_id);
        if (!run.has_value()) {
            cb(error_result("Flow run not found"sv));
            return;
        }
        cb(ok_json_result(JsonValue { WebView::serialize_flow_run(*run) }));
    }

    virtual void cancel_flow_run(String run_id, MCP::OnToolCallComplete cb) override
    {
        auto idx = m_active_runs.find_first_index(run_id);
        if (!idx.has_value()) {
            cb(error_result("Active flow run not found"sv));
            return;
        }
        m_active_runs.remove(*idx);
        if (auto existing = m_store.get_run(run_id); existing.has_value()) {
            auto updated = *existing;
            updated.status = "cancelled"_string;
            updated.error = "Cancelled by user"_string;
            updated.finished_at_ms = 1050;
            updated.duration_ms = 50;
            m_store.upsert_run(move(updated), false);
        }
        JsonObject out;
        out.set("cancelled"sv, true);
        out.set("run_id"sv, run_id);
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void list_extensions(MCP::OnToolCallComplete cb) override
    {
        JsonArray list;
        for (auto const& ext : m_ext_store.list_extensions())
            list.must_append(WebView::serialize_extension(ext));
        JsonObject out;
        out.set("extensions"sv, move(list));
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void install_unpacked_extension(String path, MCP::OnToolCallComplete cb) override
    {
        auto installed = m_ext_store.install_unpacked(path.to_byte_string());
        if (installed.is_error()) {
            cb(error_result(MUST(String::formatted("Install failed: {}", installed.error()))));
            return;
        }
        JsonObject out;
        out.set("installed"sv, true);
        out.set("extension"sv, WebView::serialize_extension(installed.value()));
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void set_extension_enabled(String extension_id, bool enabled, MCP::OnToolCallComplete cb) override
    {
        auto updated = m_ext_store.set_enabled(extension_id, enabled);
        if (updated.is_error()) {
            cb(error_result(MUST(String::formatted("Update failed: {}", updated.error()))));
            return;
        }
        JsonObject out;
        out.set("updated"sv, true);
        out.set("extension"sv, WebView::serialize_extension(updated.value()));
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void reload_extension(String extension_id, MCP::OnToolCallComplete cb) override
    {
        auto reloaded = m_ext_store.reload_extension(extension_id);
        if (reloaded.is_error()) {
            cb(error_result(MUST(String::formatted("Reload failed: {}", reloaded.error()))));
            return;
        }
        JsonObject out;
        out.set("reloaded"sv, true);
        out.set("extension"sv, WebView::serialize_extension(reloaded.value()));
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void remove_extension(String extension_id, MCP::OnToolCallComplete cb) override
    {
        if (auto res = m_ext_store.remove_extension(extension_id); res.is_error()) {
            cb(error_result(MUST(String::formatted("Remove failed: {}", res.error()))));
            return;
        }
        JsonObject out;
        out.set("removed"sv, true);
        out.set("extension_id"sv, extension_id);
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void inject_extension(String extension_id, Optional<u64> tab_id, MCP::OnToolCallComplete cb) override
    {
        auto ext = m_ext_store.get_extension(extension_id);
        if (!ext.has_value()) {
            cb(error_result("Extension not found"sv));
            return;
        }
        if (!ext->enabled) {
            cb(error_result("Extension is disabled"sv));
            return;
        }
        JsonObject out;
        out.set("injected"sv, true);
        out.set("extension_id"sv, extension_id);
        out.set("tab_id"sv, tab_id.value_or(1u));
        out.set("injected_groups"sv, static_cast<u64>(ext->manifest.content_scripts.size()));
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void list_page_tools(u64 tab_id, bool include_declarative, MCP::OnToolCallComplete cb) override
    {
        JsonArray tools;
        {
            JsonObject tool;
            tool.set("name"sv, "search_items"sv);
            tool.set("kind"sv, "imperative"sv);
            tool.set("description"sv, "Search items on the page"sv);
            JsonObject schema;
            schema.set("type"sv, "object"sv);
            JsonObject props;
            JsonObject q;
            q.set("type"sv, "string"sv);
            props.set("query"sv, move(q));
            schema.set("properties"sv, move(props));
            tool.set("inputSchema"sv, move(schema));
            JsonObject ann;
            ann.set("readOnlyHint"sv, true);
            ann.set("untrustedContentHint"sv, true);
            tool.set("annotations"sv, move(ann));
            tools.must_append(move(tool));
        }
        if (include_declarative) {
            JsonObject tool;
            tool.set("name"sv, "delete_item"sv);
            tool.set("kind"sv, "declarative"sv);
            tool.set("description"sv, "Delete item form"sv);
            JsonObject schema;
            schema.set("type"sv, "object"sv);
            schema.set("properties"sv, JsonObject {});
            tool.set("inputSchema"sv, move(schema));
            JsonObject ann;
            ann.set("readOnlyHint"sv, false);
            ann.set("consequentialHint"sv, true);
            ann.set("untrustedContentHint"sv, true);
            tool.set("annotations"sv, move(ann));
            tools.must_append(move(tool));
        }
        JsonObject out;
        out.set("tab_id"sv, tab_id);
        out.set("webmcp_shim_installed"sv, true);
        out.set("count"sv, static_cast<u64>(tools.size()));
        out.set("tools"sv, move(tools));
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void call_page_tool(u64 tab_id, String tool_name, JsonObject arguments, bool confirm_consequential, MCP::OnToolCallComplete cb) override
    {
        last_action = RecordedAction { "call_page_tool"_string, {}, {}, tool_name };
        if (tool_name == "delete_item"sv && !confirm_consequential) {
            cb(error_result("Consequential tool requires confirmation"sv));
            return;
        }
        JsonObject out;
        out.set("ok"sv, true);
        out.set("tab_id"sv, tab_id);
        out.set("tool"sv, move(tool_name));
        out.set("arguments"sv, move(arguments));
        out.set("confirmed"sv, confirm_consequential);
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void skills_list(Optional<String> cursor, MCP::OnProtocolCallComplete cb) override
    {
        auto all_skills = m_skill_store.list_skills(false);
        size_t offset = 0;
        if (cursor.has_value() && !cursor->is_empty()) {
            auto parsed = cursor->to_number<size_t>();
            if (!parsed.has_value() || *parsed > all_skills.size()) {
                cb({ .is_error = true, .error_code = -32602, .error_message = "Invalid cursor"_string });
                return;
            }
            offset = *parsed;
        }
        JsonArray arr;
        for (size_t i = offset; i < all_skills.size(); ++i)
            arr.must_append(WebView::serialize_skill_mcp_entry(all_skills[i]));
        JsonObject out;
        out.set("resultType"sv, "complete"sv);
        out.set("skills"sv, move(arr));
        out.set("ttlMs"sv, 60000);
        out.set("cacheScope"sv, "private"sv);
        cb({ .is_error = false, .result = move(out) });
    }

    virtual void skills_get(String uri, MCP::OnProtocolCallComplete cb) override
    {
        auto skill = m_skill_store.get_skill_by_uri(uri, false);
        if (!skill.has_value()) {
            cb({ .is_error = true, .error_code = -32602, .error_message = "Unknown skill URI"_string });
            return;
        }
        JsonObject out;
        out.set("resultType"sv, "complete"sv);
        out.set("skill"sv, WebView::serialize_skill_mcp_entry(*skill));
        out.set("ttlMs"sv, 60000);
        out.set("cacheScope"sv, "private"sv);
        cb({ .is_error = false, .result = move(out) });
    }

    virtual void resources_read(String uri, MCP::OnProtocolCallComplete cb) override
    {
        auto res = m_skill_store.read_skill_resource(uri, false);
        if (res.is_error()) {
            cb({ .is_error = true, .error_code = -32602, .error_message = MUST(String::formatted("{}", res.error())) });
            return;
        }
        JsonObject item;
        item.set("uri"sv, res.value().uri);
        item.set("mimeType"sv, res.value().mime_type);
        item.set("text"sv, res.value().text);
        JsonArray contents;
        contents.must_append(move(item));
        JsonObject out;
        out.set("resultType"sv, "complete"sv);
        out.set("contents"sv, move(contents));
        out.set("ttlMs"sv, 60000);
        out.set("cacheScope"sv, "private"sv);
        cb({ .is_error = false, .result = move(out) });
    }

    virtual void resources_directory_read(String uri, Optional<String>, MCP::OnProtocolCallComplete cb) override
    {
        auto entries = m_skill_store.read_skill_directory(uri, false);
        if (entries.is_error()) {
            cb({ .is_error = true, .error_code = -32602, .error_message = MUST(String::formatted("{}", entries.error())) });
            return;
        }
        JsonArray resources;
        for (auto const& e : entries.value()) {
            JsonObject obj;
            obj.set("uri"sv, e.uri);
            obj.set("name"sv, e.name);
            obj.set("mimeType"sv, e.mime_type);
            resources.must_append(move(obj));
        }
        JsonObject out;
        out.set("resultType"sv, "complete"sv);
        out.set("resources"sv, move(resources));
        out.set("ttlMs"sv, 60000);
        out.set("cacheScope"sv, "private"sv);
        cb({ .is_error = false, .result = move(out) });
    }

    virtual void list_skills_tool(Optional<u64> tab_id, Optional<String> url, bool include_disabled, MCP::OnToolCallComplete cb) override
    {
        Optional<URL::URL> filter_url;
        if (tab_id.has_value()) {
            bool found = false;
            for (auto const& t : mock_tabs) {
                if (t.id == *tab_id) {
                    filter_url = URL::Parser::basic_parse(t.url);
                    found = true;
                    break;
                }
            }
            if (!found) {
                cb(error_result("Tab not found"sv));
                return;
            }
        } else if (url.has_value() && !url->is_empty()) {
            filter_url = URL::Parser::basic_parse(*url);
            if (!filter_url.has_value()) {
                cb(error_result("Invalid URL"sv));
                return;
            }
        }

        auto skills = filter_url.has_value()
            ? m_skill_store.matching_skills_for_url(*filter_url)
            : m_skill_store.list_skills(include_disabled);
        JsonArray arr;
        for (auto const& s : skills) {
            auto e = WebView::serialize_skill_mcp_entry(s);
            e.set("name"sv, s.name);
            e.set("description"sv, s.description);
            e.set("enabled"sv, s.enabled);
            arr.must_append(move(e));
        }
        JsonObject out;
        out.set("count"sv, static_cast<u64>(skills.size()));
        out.set("skills"sv, move(arr));
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void read_skill_tool(Optional<String> name, Optional<String> uri, Optional<String> file, MCP::OnToolCallComplete cb) override
    {
        String target_uri;
        String skill_name;
        if (uri.has_value() && !uri->is_empty()) {
            auto parsed = WebView::parse_skill_uri(*uri, true);
            if (!parsed.has_value()) {
                cb(error_result("Invalid URI"sv));
                return;
            }
            skill_name = parsed->name;
            target_uri = file.has_value() && !file->is_empty()
                ? WebView::skill_uri(skill_name, *file)
                : (parsed->relative_path.is_empty() ? WebView::skill_uri(skill_name, "SKILL.md"sv) : *uri);
        } else if (name.has_value()) {
            skill_name = *name;
            target_uri = WebView::skill_uri(skill_name, file.value_or("SKILL.md"_string));
        } else {
            cb(error_result("Missing name or uri"sv));
            return;
        }
        auto skill = m_skill_store.get_skill(skill_name, false);
        auto res = m_skill_store.read_skill_resource(target_uri, false);
        if (!skill.has_value() || res.is_error()) {
            cb(error_result("Skill resource not found"sv));
            return;
        }
        JsonObject out;
        out.set("name"sv, skill->name);
        out.set("uri"sv, res.value().uri);
        out.set("mimeType"sv, res.value().mime_type);
        out.set("digest"sv, res.value().digest);
        out.set("size"sv, res.value().size_bytes);
        out.set("content"sv, res.value().text);
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void save_skill_tool(JsonValue skill_json, MCP::OnToolCallComplete cb) override
    {
        if (!allow_skill_write) {
            cb(error_result("Writing skills via MCP is disabled by default for security."sv));
            return;
        }
        Vector<WebView::SkillSupportingFile> files;
        auto parsed = WebView::parse_skill_json(skill_json, files);
        if (parsed.is_error()) {
            cb(error_result(MUST(String::formatted("{}", parsed.error()))));
            return;
        }
        auto saved = m_skill_store.save_skill(parsed.release_value(), move(files));
        if (saved.is_error()) {
            cb(error_result(MUST(String::formatted("{}", saved.error()))));
            return;
        }
        JsonObject out;
        out.set("saved"sv, true);
        out.set("skill"sv, WebView::serialize_skill_mcp_entry(saved.value()));
        cb(ok_json_result(JsonValue { move(out) }));
    }

    virtual void delete_skill_tool(String name, MCP::OnToolCallComplete cb) override
    {
        if (!allow_skill_write) {
            cb(error_result("Writing skills via MCP is disabled by default for security."sv));
            return;
        }
        if (auto res = m_skill_store.delete_skill(name); res.is_error()) {
            cb(error_result(MUST(String::formatted("{}", res.error()))));
            return;
        }
        JsonObject out;
        out.set("deleted"sv, true);
        out.set("name"sv, move(name));
        cb(ok_json_result(JsonValue { move(out) }));
    }

private:
    WebView::FlowStore m_store;
    WebView::ExtensionStore m_ext_store;
    WebView::SkillStore m_skill_store;
    Vector<String> m_active_runs;
    u64 m_run_counter { 0 };
};

struct HttpResponse {
    int status_code { 0 };
    String session_id;
    String body;
};

HttpResponse send_mcp_http(u16 port, Optional<StringView> bearer_token, StringView body, Optional<StringView> origin_header = {}, Optional<StringView> session_id_header = {})
{
    auto socket = MUST(Core::TCPSocket::connect(Core::SocketAddress { IPv4Address { 127, 0, 0, 1 }, port }));

    StringBuilder req;
    req.append("POST /mcp HTTP/1.1\r\n"sv);
    req.appendff("Host: 127.0.0.1:{}\r\n", port);
    req.append("Content-Type: application/json\r\n"sv);
    if (bearer_token.has_value())
        req.appendff("Authorization: Bearer {}\r\n", *bearer_token);
    if (origin_header.has_value())
        req.appendff("Origin: {}\r\n", *origin_header);
    if (session_id_header.has_value())
        req.appendff("Mcp-Session-Id: {}\r\n", *session_id_header);
    req.appendff("Content-Length: {}\r\n", body.length());
    req.append("Connection: close\r\n\r\n"sv);
    req.append(body);

    MUST(socket->write_until_depleted(req.string_view().bytes()));
    MUST(socket->set_blocking(false));

    StringBuilder raw_response;
    bool done = false;
    bool timed_out = false;

    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [&] {
        timed_out = true;
        done = true;
    });
    timer->start();

    socket->on_ready_to_read = [&] {
        Array<u8, 16384> buffer;
        while (!done) {
            auto read_res = socket->read_some(buffer);
            if (read_res.is_error()) {
                if (read_res.error().is_errno() && (read_res.error().code() == EAGAIN || read_res.error().code() == EWOULDBLOCK))
                    break;
                done = true;
                break;
            }
            auto bytes = read_res.value();
            if (bytes.is_empty() || socket->is_eof()) {
                if (!bytes.is_empty())
                    raw_response.append(StringView { bytes });
                done = true;
                break;
            }
            raw_response.append(StringView { bytes });
        }
    };

    Core::EventLoop::current().spin_until([&] { return done; });
    timer->stop();
    socket->close();
    EXPECT(!timed_out);

    auto raw_view = raw_response.string_view();
    auto split = raw_view.find("\r\n\r\n"sv);
    EXPECT(split.has_value());

    auto header_block = raw_view.substring_view(0, *split);
    auto response_body = raw_view.substring_view(*split + 4);

    HttpResponse parsed;
    parsed.body = MUST(String::from_utf8(response_body));

    auto lines = header_block.lines();
    if (!lines.is_empty()) {
        auto status_parts = lines[0].split_view(' ');
        if (status_parts.size() >= 2)
            parsed.status_code = status_parts[1].to_number<int>().value_or(0);
    }
    for (size_t i = 1; i < lines.size(); ++i) {
        if (auto colon = lines[i].find(':'); colon.has_value()) {
            auto name = lines[i].substring_view(0, *colon).trim_whitespace();
            auto value = lines[i].substring_view(*colon + 1).trim_whitespace();
            if (name.equals_ignoring_ascii_case("Mcp-Session-Id"sv))
                parsed.session_id = MUST(String::from_utf8(value));
        }
    }
    return parsed;
}

JsonObject call_mcp_rpc(u16 port, StringView token, int id, StringView method, JsonObject params)
{
    JsonObject rpc;
    rpc.set("jsonrpc"sv, "2.0"sv);
    rpc.set("id"sv, id);
    rpc.set("method"sv, method);
    rpc.set("params"sv, move(params));

    auto http = send_mcp_http(port, token, JsonValue { move(rpc) }.serialized());
    EXPECT_EQ(http.status_code, 200);
    auto parsed = MUST(JsonValue::from_string(http.body));
    EXPECT(parsed.is_object());
    return parsed.as_object();
}

JsonObject call_mcp_tool(u16 port, StringView token, int id, StringView name, JsonObject arguments)
{
    JsonObject params;
    params.set("name"sv, name);
    params.set("arguments"sv, move(arguments));
    auto rpc_resp = call_mcp_rpc(port, token, id, "tools/call"sv, move(params));
    EXPECT(!rpc_resp.has("error"sv));
    auto result = rpc_resp.get_object("result"sv);
    EXPECT(result.has_value());
    return *result;
}

JsonObject parse_tool_json_payload(JsonObject const& tool_result)
{
    auto content = tool_result.get_array("content"sv);
    EXPECT(content.has_value());
    EXPECT(!content->is_empty());
    auto first = content->at(0).as_object();
    auto text = first.get_string("text"sv);
    EXPECT(text.has_value());
    auto parsed = MUST(JsonValue::from_string(*text));
    return parsed.as_object();
}

}

TEST_CASE(mcp_auth_and_origin_guards)
{
    Core::EventLoop loop;
    auto temp_dir = make_temp_mcp_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(temp_dir, FileSystem::RecursionMode::Allowed);
    };

    FakeMCPDelegate delegate(temp_dir);
    auto server = MUST(MCP::MCPServer::create(delegate, 0, "secret-token-123"_string));
    auto port = server->local_port();
    EXPECT(port.has_value());

    auto missing_auth = send_mcp_http(*port, {}, "{}"sv);
    EXPECT_EQ(missing_auth.status_code, 401);

    auto wrong_auth = send_mcp_http(*port, "wrong-token"sv, "{}"sv);
    EXPECT_EQ(wrong_auth.status_code, 401);

    auto bad_origin = send_mcp_http(*port, "secret-token-123"sv, "{}"sv, "https://evil.example"sv);
    EXPECT_EQ(bad_origin.status_code, 403);

    JsonObject init_rpc;
    init_rpc.set("jsonrpc"sv, "2.0"sv);
    init_rpc.set("id"sv, 1);
    init_rpc.set("method"sv, "initialize"sv);
    init_rpc.set("params"sv, JsonObject {});
    auto init_http = send_mcp_http(*port, "secret-token-123"sv, JsonValue { move(init_rpc) }.serialized());
    EXPECT_EQ(init_http.status_code, 200);
    EXPECT(!init_http.session_id.is_empty());

    // Invalid Mcp-Session-Id is rejected with 404, valid one is accepted.
    auto bad_session = send_mcp_http(*port, "secret-token-123"sv, R"({"jsonrpc":"2.0","id":2,"method":"ping"})"sv, {}, "stale-session-id"sv);
    EXPECT_EQ(bad_session.status_code, 404);
    auto good_session = send_mcp_http(*port, "secret-token-123"sv, R"({"jsonrpc":"2.0","id":3,"method":"ping"})"sv, {}, init_http.session_id);
    EXPECT_EQ(good_session.status_code, 200);

    // Request bodies > 64 KiB (up to 4 MiB) must be accepted by HttpRequest + MCPServer.
    StringBuilder large_comment;
    for (size_t i = 0; i < 80 * 1024; ++i)
        large_comment.append('a');
    JsonObject large_rpc;
    large_rpc.set("jsonrpc"sv, "2.0"sv);
    large_rpc.set("id"sv, 4);
    large_rpc.set("method"sv, "tools/call"sv);
    JsonObject large_params;
    large_params.set("name"sv, "list_tabs"sv);
    JsonObject large_args;
    large_args.set("padding"sv, MUST(large_comment.to_string()));
    large_params.set("arguments"sv, move(large_args));
    large_rpc.set("params"sv, move(large_params));
    auto large_http = send_mcp_http(*port, "secret-token-123"sv, JsonValue { move(large_rpc) }.serialized());
    EXPECT_EQ(large_http.status_code, 200);
}

TEST_CASE(mcp_initialize_and_tools_list_schemas)
{
    Core::EventLoop loop;
    auto temp_dir = make_temp_mcp_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(temp_dir, FileSystem::RecursionMode::Allowed);
    };

    FakeMCPDelegate delegate(temp_dir);
    auto token = "mcp-schema-token"_string;
    auto server = MUST(MCP::MCPServer::create(delegate, 0, token));
    auto port = *server->local_port();

    JsonObject init_rpc;
    init_rpc.set("jsonrpc"sv, "2.0"sv);
    init_rpc.set("id"sv, 1);
    init_rpc.set("method"sv, "initialize"sv);
    init_rpc.set("params"sv, JsonObject {});
    auto init_http = send_mcp_http(port, token, JsonValue { move(init_rpc) }.serialized());
    EXPECT_EQ(init_http.status_code, 200);
    EXPECT(!init_http.session_id.is_empty());

    auto init_json = MUST(JsonValue::from_string(init_http.body)).as_object();
    auto instructions = init_json.get_object("result"sv)->get_string("instructions"sv);
    EXPECT(instructions.has_value());
    EXPECT(instructions->contains("save_flow"sv));
    EXPECT(instructions->contains("run_flow"sv));
    EXPECT(instructions->contains("list_page_tools"sv));
    EXPECT(instructions->contains("call_page_tool"sv));

    auto tools_rpc = call_mcp_rpc(port, token, 2, "tools/list"sv, JsonObject {});
    auto tools = tools_rpc.get_object("result"sv)->get_array("tools"sv);
    EXPECT(tools.has_value());
    EXPECT_EQ(tools->size(), 47u);

    Optional<JsonObject> save_flow_tool;
    Optional<JsonObject> run_flow_tool;
    Optional<JsonObject> open_tab_tool;
    Optional<JsonObject> click_tool;
    Optional<JsonObject> type_text_tool;
    Optional<JsonObject> list_page_tools_tool;
    Optional<JsonObject> call_page_tool_tool;
    bool found_cancel_flow_run = false;
    bool found_install_extension = false;
    bool found_inject_extension = false;
    bool found_drag = false;
    bool found_upload_file = false;
    bool found_snapshot = false;
    bool found_console = false;
    bool found_network = false;
    bool found_wait_for = false;
    for (size_t i = 0; i < tools->size(); ++i) {
        auto tool = tools->at(i).as_object();
        auto name = tool.get_string("name"sv).value_or(String {});
        if (name == "save_flow"sv)
            save_flow_tool = tool;
        else if (name == "run_flow"sv)
            run_flow_tool = tool;
        else if (name == "open_tab"sv)
            open_tab_tool = tool;
        else if (name == "click"sv)
            click_tool = tool;
        else if (name == "type_text"sv)
            type_text_tool = tool;
        else if (name == "list_page_tools"sv)
            list_page_tools_tool = tool;
        else if (name == "call_page_tool"sv)
            call_page_tool_tool = tool;
        else if (name == "cancel_flow_run"sv)
            found_cancel_flow_run = true;
        else if (name == "install_unpacked_extension"sv)
            found_install_extension = true;
        else if (name == "inject_extension"sv)
            found_inject_extension = true;
        else if (name == "drag"sv)
            found_drag = true;
        else if (name == "upload_file"sv)
            found_upload_file = true;
        else if (name == "get_page_snapshot"sv)
            found_snapshot = true;
        else if (name == "get_console_logs"sv)
            found_console = true;
        else if (name == "get_network_requests"sv)
            found_network = true;
        else if (name == "wait_for"sv)
            found_wait_for = true;
    }

    EXPECT(save_flow_tool.has_value());
    EXPECT(run_flow_tool.has_value());
    EXPECT(open_tab_tool.has_value());
    EXPECT(open_tab_tool->get_object("inputSchema"sv)->get_object("properties"sv)->has("container_id"sv));
    EXPECT(click_tool.has_value());
    EXPECT(type_text_tool.has_value());
    EXPECT(list_page_tools_tool.has_value());
    EXPECT(call_page_tool_tool.has_value());
    EXPECT(call_page_tool_tool->get_object("inputSchema"sv)->get_object("properties"sv)->has("arguments"sv));
    EXPECT(call_page_tool_tool->get_object("inputSchema"sv)->get_object("properties"sv)->has("confirm_consequential"sv));
    EXPECT(found_cancel_flow_run);
    EXPECT(found_install_extension);
    EXPECT(found_inject_extension);
    EXPECT(found_drag);
    EXPECT(found_upload_file);
    EXPECT(found_snapshot);
    EXPECT(found_console);
    EXPECT(found_network);
    EXPECT(found_wait_for);
    EXPECT(click_tool->get_object("inputSchema"sv)->get_object("properties"sv)->has("ref"sv));
    EXPECT(type_text_tool->get_object("inputSchema"sv)->get_object("properties"sv)->has("ref"sv));

    auto flow_schema = save_flow_tool->get_object("inputSchema"sv)
                           ->get_object("properties"sv)
                           ->get_object("flow"sv);
    EXPECT(flow_schema.has_value());
    auto step_props = flow_schema->get_object("properties"sv)
                          ->get_object("steps"sv)
                          ->get_object("items"sv)
                          ->get_object("properties"sv);
    EXPECT(step_props.has_value());

    auto action_enum = step_props->get_object("action"sv)->get_array("enum"sv);
    EXPECT(action_enum.has_value());
    EXPECT_EQ(action_enum->size(), 15u);

    auto locator_kind_enum = step_props->get_object("locator"sv)
                                 ->get_object("properties"sv)
                                 ->get_object("kind"sv)
                                 ->get_array("enum"sv);
    EXPECT(locator_kind_enum.has_value());
    EXPECT_EQ(locator_kind_enum->size(), 7u);

    auto run_props = run_flow_tool->get_object("inputSchema"sv)->get_object("properties"sv);
    EXPECT(run_props->has("wait"sv));
    EXPECT(run_props->has("close_tab_on_finish"sv));
}

TEST_CASE(mcp_automation_flow_lifecycle)
{
    Core::EventLoop loop;
    auto temp_dir = make_temp_mcp_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(temp_dir, FileSystem::RecursionMode::Allowed);
    };

    FakeMCPDelegate delegate(temp_dir);
    auto token = "mcp-lifecycle-token"_string;
    auto server = MUST(MCP::MCPServer::create(delegate, 0, token));
    auto port = *server->local_port();

    auto flow1 = WebView::serialize_flow_definition(WebView::make_example_flow());

    // 1. save_flow as JSON object
    {
        JsonObject args;
        args.set("flow"sv, flow1);
        auto res = call_mcp_tool(port, token, 1, "save_flow"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("saved"sv).value_or(false));
    }

    // 2. save_flow with stringified JSON fallback (LLM resilience)
    {
        auto flow2 = WebView::make_example_flow();
        flow2.id = "mcp-stringified"_string;
        flow2.name = "Stringified Flow"_string;
        JsonObject args;
        args.set("flow"sv, JsonValue { WebView::serialize_flow_definition(flow2) }.serialized());
        auto res = call_mcp_tool(port, token, 2, "save_flow"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("saved"sv).value_or(false));
    }

    // 3. list_flows and get_flow
    {
        auto list_res = call_mcp_tool(port, token, 3, "list_flows"sv, JsonObject {});
        auto list_payload = parse_tool_json_payload(list_res);
        EXPECT_EQ(list_payload.get_array("flows"sv)->size(), 2u);

        JsonObject get_args;
        get_args.set("flow_id"sv, "mcp-stringified"sv);
        auto get_res = call_mcp_tool(port, token, 4, "get_flow"sv, move(get_args));
        auto get_payload = parse_tool_json_payload(get_res);
        EXPECT_EQ(get_payload.get_string("name"sv).value_or(String {}), "Stringified Flow"sv);
    }

    // 4. run_flow synchronous with variables & close_tab_on_finish
    {
        JsonObject vars;
        vars.set("target_url"sv, "http://127.0.0.1:8080"sv);
        JsonObject args;
        args.set("flow_id"sv, "example-smoke"sv);
        args.set("variables"sv, move(vars));
        args.set("wait"sv, true);
        args.set("close_tab_on_finish"sv, true);

        auto res = call_mcp_tool(port, token, 5, "run_flow"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT_EQ(payload.get_string("status"sv).value_or(String {}), "passed"sv);
        EXPECT(delegate.last_run_call.has_value());
        EXPECT_EQ(delegate.last_run_call->wait, true);
        EXPECT_EQ(delegate.last_run_call->close_tab_on_finish, true);
        EXPECT_EQ(delegate.last_run_call->variables.get("target_url"sv).value_or(String {}), "http://127.0.0.1:8080"sv);
    }

    // 5. run_flow asynchronous (wait: false) -> list_flow_runs -> get_flow_run -> cancel_flow_run
    String async_run_id;
    {
        JsonObject args;
        args.set("flow"sv, flow1);
        args.set("wait"sv, false);
        auto res = call_mcp_tool(port, token, 6, "run_flow"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("started"sv).value_or(false));
        EXPECT_EQ(payload.get_string("status"sv).value_or(String {}), "running"sv);
        async_run_id = payload.get_string("run_id"sv).value_or(String {});
        EXPECT(!async_run_id.is_empty());
    }

    {
        auto runs_res = call_mcp_tool(port, token, 7, "list_flow_runs"sv, JsonObject {});
        auto runs_payload = parse_tool_json_payload(runs_res);
        EXPECT_EQ(runs_payload.get_array("runs"sv)->size(), 2u);

        JsonObject get_run_args;
        get_run_args.set("run_id"sv, async_run_id);
        auto get_run_res = call_mcp_tool(port, token, 8, "get_flow_run"sv, move(get_run_args));
        auto get_run_payload = parse_tool_json_payload(get_run_res);
        EXPECT_EQ(get_run_payload.get_string("status"sv).value_or(String {}), "running"sv);
    }

    {
        JsonObject cancel_args;
        cancel_args.set("run_id"sv, async_run_id);
        auto cancel_res = call_mcp_tool(port, token, 9, "cancel_flow_run"sv, move(cancel_args));
        EXPECT(!cancel_res.get_bool("isError"sv).value_or(true));
        auto cancel_payload = parse_tool_json_payload(cancel_res);
        EXPECT(cancel_payload.get_bool("cancelled"sv).value_or(false));

        JsonObject get_run_args;
        get_run_args.set("run_id"sv, async_run_id);
        auto get_run_res = call_mcp_tool(port, token, 10, "get_flow_run"sv, move(get_run_args));
        auto get_run_payload = parse_tool_json_payload(get_run_res);
        EXPECT_EQ(get_run_payload.get_string("status"sv).value_or(String {}), "cancelled"sv);

        // Cancelling a second time returns an error.
        JsonObject cancel_again_args;
        cancel_again_args.set("run_id"sv, async_run_id);
        auto cancel_again = call_mcp_tool(port, token, 11, "cancel_flow_run"sv, move(cancel_again_args));
        EXPECT(cancel_again.get_bool("isError"sv).value_or(false));
    }

    // 6. delete_flow removes it from store
    {
        JsonObject del_args;
        del_args.set("flow_id"sv, "mcp-stringified"sv);
        auto del_res = call_mcp_tool(port, token, 12, "delete_flow"sv, move(del_args));
        EXPECT(!del_res.get_bool("isError"sv).value_or(true));

        JsonObject get_args;
        get_args.set("flow_id"sv, "mcp-stringified"sv);
        auto get_res = call_mcp_tool(port, token, 13, "get_flow"sv, move(get_args));
        EXPECT(get_res.get_bool("isError"sv).value_or(false));
    }
}

TEST_CASE(mcp_extensions_lifecycle)
{
    Core::EventLoop loop;
    auto temp_dir = make_temp_mcp_dir();
    auto unpacked_dir = make_temp_mcp_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(temp_dir, FileSystem::RecursionMode::Allowed);
        (void)FileSystem::remove(unpacked_dir, FileSystem::RecursionMode::Allowed);
    };

    auto manifest_file = MUST(Core::File::open(LexicalPath::join(unpacked_dir, "manifest.json"sv).string(), Core::File::OpenMode::Write));
    MUST(manifest_file->write_until_depleted(R"JSON({
        "manifest_version": 3,
        "id": "mcp-ext-demo",
        "name": "MCP Extension Demo",
        "version": "1.0.0",
        "permissions": ["storage"],
        "content_scripts": [{ "matches": ["http://*/*"], "js": ["plugin.js"] }]
    })JSON"sv));
    auto js_file = MUST(Core::File::open(LexicalPath::join(unpacked_dir, "plugin.js"sv).string(), Core::File::OpenMode::Write));
    MUST(js_file->write_until_depleted("console.log('mcp extension');"sv));

    FakeMCPDelegate delegate(temp_dir);
    auto token = "mcp-ext-token"_string;
    auto server = MUST(MCP::MCPServer::create(delegate, 0, token));
    auto port = *server->local_port();

    {
        JsonObject args;
        args.set("path"sv, String::from_utf8_without_validation(unpacked_dir.bytes()));
        auto res = call_mcp_tool(port, token, 1, "install_unpacked_extension"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("installed"sv).value_or(false));
        EXPECT_EQ(payload.get_object("extension"sv)->get_string("id"sv).value_or(String {}), "mcp-ext-demo"sv);
    }

    {
        auto res = call_mcp_tool(port, token, 2, "list_extensions"sv, JsonObject {});
        auto payload = parse_tool_json_payload(res);
        EXPECT_EQ(payload.get_array("extensions"sv)->size(), 1u);
    }

    {
        JsonObject args;
        args.set("extension_id"sv, "mcp-ext-demo"sv);
        args.set("tab_id"sv, 42);
        auto res = call_mcp_tool(port, token, 3, "inject_extension"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("injected"sv).value_or(false));
        EXPECT_EQ(payload.get_integer<u64>("tab_id"sv).value_or(0), 42u);
        EXPECT_EQ(payload.get_integer<u64>("injected_groups"sv).value_or(0), 1u);
    }

    {
        JsonObject args;
        args.set("extension_id"sv, "mcp-ext-demo"sv);
        args.set("enabled"sv, false);
        auto res = call_mcp_tool(port, token, 4, "set_extension_enabled"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT(!payload.get_object("extension"sv)->get_bool("enabled"sv).value_or(true));
    }

    {
        JsonObject args;
        args.set("extension_id"sv, "mcp-ext-demo"sv);
        args.set("tab_id"sv, 42);
        auto res = call_mcp_tool(port, token, 5, "inject_extension"sv, move(args));
        EXPECT(res.get_bool("isError"sv).value_or(false));
    }

    {
        JsonObject args;
        args.set("extension_id"sv, "mcp-ext-demo"sv);
        auto res = call_mcp_tool(port, token, 6, "reload_extension"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("reloaded"sv).value_or(false));
    }

    {
        JsonObject args;
        args.set("extension_id"sv, "mcp-ext-demo"sv);
        auto res = call_mcp_tool(port, token, 7, "remove_extension"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("removed"sv).value_or(false));
    }
}

TEST_CASE(mcp_agentic_tools_roundtrip)
{
    Core::EventLoop loop;
    auto temp_dir = make_temp_mcp_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(temp_dir, FileSystem::RecursionMode::Allowed);
    };

    FakeMCPDelegate delegate(temp_dir);
    auto token = "mcp-agentic-token"_string;
    auto server = MUST(MCP::MCPServer::create(delegate, 0, token));
    auto port = *server->local_port();

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        auto res = call_mcp_tool(port, token, 1, "get_page_snapshot"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT_EQ(payload.get_integer<u64>("element_count"sv).value_or(0), 1u);
        EXPECT_EQ(payload.get_array("elements"sv)->at(0).as_object().get_string("ref"sv).value_or(String {}), "@e1"sv);
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("ref"sv, "@e1"sv);
        auto res = call_mcp_tool(port, token, 2, "click"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("clicked"sv).value_or(false));
        EXPECT_EQ(delegate.last_action->ref.value_or(String {}), "@e1"sv);
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("ref"sv, "@e2"sv);
        args.set("text"sv, "hello"sv);
        auto res = call_mcp_tool(port, token, 3, "type_text"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("typed"sv).value_or(false));
        EXPECT_EQ(delegate.last_action->ref.value_or(String {}), "@e2"sv);
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("key"sv, "Enter"sv);
        args.set("ref"sv, "@e2"sv);
        auto res = call_mcp_tool(port, token, 4, "press_key"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("pressed"sv).value_or(false));
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("direction"sv, "down"sv);
        args.set("amount"sv, 400);
        auto res = call_mcp_tool(port, token, 5, "scroll"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("scrolled"sv).value_or(false));
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("ref"sv, "@e1"sv);
        auto res = call_mcp_tool(port, token, 6, "hover"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("hovered"sv).value_or(false));
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("ref"sv, "@e3"sv);
        args.set("value"sv, "opt1"sv);
        auto res = call_mcp_tool(port, token, 7, "select_option"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("selected"sv).value_or(false));
        EXPECT_EQ(payload.get_string("selected_value"sv).value_or(String {}), "opt1"sv);
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("text"sv, "Loaded"sv);
        args.set("timeout_ms"sv, 2000);
        auto res = call_mcp_tool(port, token, 8, "wait_for"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("satisfied"sv).value_or(false));
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        auto res = call_mcp_tool(port, token, 9, "get_console_logs"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT_EQ(payload.get_integer<u64>("count"sv).value_or(0), 1u);
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("only_errors"sv, true);
        auto res = call_mcp_tool(port, token, 10, "get_network_requests"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT_EQ(payload.get_array("requests"sv)->at(0).as_object().get_integer<u64>("status_code"sv).value_or(0), 500u);
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("from_ref"sv, "@e1"sv);
        args.set("delta_x"sv, 120);
        args.set("steps"sv, 8);
        args.set("duration_ms"sv, 200);
        auto res = call_mcp_tool(port, token, 11, "drag"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("dragged"sv).value_or(false));
        EXPECT_EQ(payload.get_integer<u64>("steps"sv).value_or(0), 8u);
        EXPECT_EQ(payload.get_integer<u64>("duration_ms"sv).value_or(0), 200u);
        EXPECT_EQ(delegate.last_action->tool, "drag"sv);
    }

    {
        JsonArray paths;
        paths.must_append("C:\\tmp\\thumb.png"sv);
        paths.must_append("C:\\tmp\\captions.srt"sv);
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("selector"sv, "input[type=file]"sv);
        args.set("paths"sv, move(paths));
        auto res = call_mcp_tool(port, token, 12, "upload_file"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("uploaded"sv).value_or(false));
        EXPECT_EQ(payload.get_integer<u64>("file_count"sv).value_or(0), 2u);
        EXPECT_EQ(delegate.last_action->tool, "upload_file"sv);
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("ref"sv, "@e4"sv);
        args.set("path"sv, "C:\\tmp\\single.mp4"sv);
        auto res = call_mcp_tool(port, token, 13, "upload_file"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("uploaded"sv).value_or(false));
        EXPECT_EQ(payload.get_integer<u64>("file_count"sv).value_or(0), 1u);
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("include_declarative"sv, true);
        auto res = call_mcp_tool(port, token, 14, "list_page_tools"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT_EQ(payload.get_integer<u64>("count"sv).value_or(0), 2u);
        EXPECT_EQ(payload.get_array("tools"sv)->size(), 2u);
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("include_declarative"sv, false);
        auto res = call_mcp_tool(port, token, 15, "list_page_tools"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT_EQ(payload.get_integer<u64>("count"sv).value_or(0), 1u);
        EXPECT_EQ(payload.get_array("tools"sv)->at(0).as_object().get_string("name"sv).value_or(String {}), "search_items"sv);
    }

    {
        JsonObject tool_args;
        tool_args.set("query"sv, "ladybird"sv);
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("name"sv, "search_items"sv);
        args.set("arguments"sv, move(tool_args));
        auto res = call_mcp_tool(port, token, 16, "call_page_tool"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("ok"sv).value_or(false));
        EXPECT_EQ(payload.get_string("tool"sv).value_or(String {}), "search_items"sv);
        EXPECT_EQ(payload.get_object("arguments"sv)->get_string("query"sv).value_or(String {}), "ladybird"sv);
        EXPECT_EQ(delegate.last_action->tool, "call_page_tool"sv);
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("name"sv, "search_items"sv);
        args.set("arguments"sv, "{\"query\":\"webmcp\"}"sv);
        auto res = call_mcp_tool(port, token, 17, "call_page_tool"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_bool("ok"sv).value_or(false));
        EXPECT_EQ(payload.get_object("arguments"sv)->get_string("query"sv).value_or(String {}), "webmcp"sv);
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("name"sv, "delete_item"sv);
        args.set("confirm_consequential"sv, false);
        auto blocked = call_mcp_tool(port, token, 18, "call_page_tool"sv, move(args));
        EXPECT(blocked.get_bool("isError"sv).value_or(false));
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 42);
        args.set("name"sv, "delete_item"sv);
        args.set("confirm_consequential"sv, true);
        auto confirmed = call_mcp_tool(port, token, 19, "call_page_tool"sv, move(args));
        EXPECT(!confirmed.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(confirmed);
        EXPECT(payload.get_bool("ok"sv).value_or(false));
        EXPECT(payload.get_bool("confirmed"sv).value_or(false));
    }
}

TEST_CASE(mcp_upload_path_security_guards)
{
    auto temp_dir = make_temp_mcp_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(temp_dir, FileSystem::RecursionMode::Allowed);
    };

    auto write_dummy = [](ByteString const& path) {
        auto parent = LexicalPath::dirname(path);
        MUST(Core::Directory::create(parent, Core::Directory::CreateDirectories::Yes));
        auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted("data"sv));
    };

    auto allowed_dir = LexicalPath::join(temp_dir, "allowed"sv).string();
    auto photo_path = LexicalPath::join(allowed_dir, "photo.png"sv).string();
    auto outside_path = LexicalPath::join(temp_dir, "outside.txt"sv).string();
    auto ssh_key = LexicalPath::join(temp_dir, ".ssh"sv, "id_rsa"sv).string();
    auto env_file = LexicalPath::join(temp_dir, ".env"sv).string();
    auto cookies_file = LexicalPath::join(temp_dir, "Cookies"sv).string();
    auto profile_db = LexicalPath::join(temp_dir, "Ladybird"sv, "Profiles"sv, "developer"sv, "Cookies.sqlite"sv).string();

    write_dummy(photo_path);
    write_dummy(outside_path);
    write_dummy(ssh_key);
    write_dummy(env_file);
    write_dummy(cookies_file);
    write_dummy(profile_db);

    EXPECT(WebView::validate_mcp_upload_path("photo.png"sv).is_error());
    auto traversal = ByteString::formatted("{}/../outside.txt", allowed_dir);
    EXPECT(WebView::validate_mcp_upload_path(traversal).is_error());
    EXPECT(WebView::validate_mcp_upload_path(allowed_dir).is_error());
    EXPECT(WebView::validate_mcp_upload_path(LexicalPath::join(allowed_dir, "missing.png"sv).string()).is_error());

    EXPECT(WebView::validate_mcp_upload_path(ssh_key).is_error());
    EXPECT(WebView::validate_mcp_upload_path(env_file).is_error());
    EXPECT(WebView::validate_mcp_upload_path(cookies_file).is_error());
    EXPECT(WebView::validate_mcp_upload_path(profile_db).is_error());

    auto appdata_profile = LexicalPath::join(temp_dir, "AppData"sv, "Local"sv, "Ladybird"sv, "Cookies.sqlite"sv).string();
    auto xdg_profile = LexicalPath::join(temp_dir, ".local"sv, "share"sv, "Ladybird"sv, "History.sqlite"sv).string();
    write_dummy(appdata_profile);
    write_dummy(xdg_profile);
    EXPECT(WebView::validate_mcp_upload_path(appdata_profile).is_error());
    EXPECT(WebView::validate_mcp_upload_path(xdg_profile).is_error());

    EXPECT(WebView::validate_mcp_upload_path("\\\\server\\share\\file.txt"sv).is_error());
    EXPECT(WebView::validate_mcp_upload_path("//server/share/file.txt"sv).is_error());
    auto newline_injection = ByteString::formatted("{}\n{}", photo_path, outside_path);
    EXPECT(WebView::validate_mcp_upload_path(newline_injection).is_error());
    auto cr_injection = ByteString::formatted("{}\r{}", photo_path, outside_path);
    EXPECT(WebView::validate_mcp_upload_path(cr_injection).is_error());

    EXPECT(!WebView::validate_mcp_upload_path(photo_path).is_error());
    EXPECT(!WebView::validate_mcp_upload_path(photo_path, allowed_dir).is_error());
    EXPECT(WebView::validate_mcp_upload_path(outside_path, allowed_dir).is_error());
}

TEST_CASE(mcp_skills_store_and_url_matching)
{
    auto temp_dir = make_temp_mcp_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(temp_dir, FileSystem::RecursionMode::Allowed);
    };

    EXPECT(WebView::is_valid_skill_name("youtube-navigation"sv));
    EXPECT(!WebView::is_valid_skill_name("-bad"sv));
    EXPECT(!WebView::is_valid_skill_name("BadName"sv));
    EXPECT(!WebView::is_valid_skill_name("bad--name"sv));
    EXPECT(!WebView::is_safe_skill_relative_path("../secret.txt"sv));
    EXPECT(!WebView::is_safe_skill_relative_path("/abs/path.md"sv));
    EXPECT(!WebView::is_safe_skill_relative_path("a\\b.md"sv));
    EXPECT(WebView::is_safe_skill_relative_path("references/checklist.md"sv));

    auto skills_dir = LexicalPath::join(temp_dir, "skills"sv).string();
    WebView::SkillStore store(skills_dir);

    WebView::SkillDefinition yt;
    yt.name = "youtube-navigation"_string;
    yt.description = "Navigate YouTube & Studio cleanly."_string;
    yt.matches.append("https://www.youtube.com/*"_string);
    yt.matches.append("studio.youtube.com"_string);
    yt.extra_frontmatter.set("license"_string, JsonValue { "MIT"_string });
    yt.body = "# YouTube\n\nRead `references/checklist.md`."_string;

    Vector<WebView::SkillSupportingFile> files;
    files.append({ .relative_path = "references/checklist.md"_string, .content = "# Checklist\n\nCheck video title.\n"_string });

    auto saved = MUST(store.save_skill(yt, move(files)));
    EXPECT_EQ(saved.name, "youtube-navigation"sv);
    EXPECT_EQ(saved.resources.size(), 2uz);

    WebView::SkillStore reloaded(skills_dir);
    auto fetched = reloaded.get_skill("youtube-navigation"sv);
    EXPECT(fetched.has_value());
    EXPECT_EQ(fetched->resources.size(), 2uz);
    EXPECT_EQ(fetched->extra_frontmatter.get("license"sv)->as_string(), "MIT"sv);

    bool found_skill_md = false;
    bool found_ref = false;
    for (auto const& r : fetched->resources) {
        EXPECT(r.digest.starts_with_bytes("sha256:"sv));
        EXPECT(r.size_bytes > 0);
        if (r.uri == "skill://youtube-navigation/SKILL.md"sv)
            found_skill_md = true;
        if (r.uri == "skill://youtube-navigation/references/checklist.md"sv)
            found_ref = true;
    }
    EXPECT(found_skill_md);
    EXPECT(found_ref);

    auto watch_url = URL::Parser::basic_parse("https://www.youtube.com/watch?v=123"sv).value();
    auto studio_url = URL::Parser::basic_parse("https://studio.youtube.com/channel/abc"sv).value();
    auto other_url = URL::Parser::basic_parse("https://example.com/"sv).value();

    EXPECT_EQ(reloaded.matching_skills_for_url(watch_url).size(), 1uz);
    EXPECT_EQ(reloaded.matching_skills_for_url(studio_url).size(), 1uz);
    EXPECT_EQ(reloaded.matching_skills_for_url(other_url).size(), 0uz);

    MUST(reloaded.set_skill_enabled("youtube-navigation"sv, false));
    EXPECT_EQ(reloaded.matching_skills_for_url(watch_url).size(), 0uz);
    EXPECT_EQ(reloaded.list_skills(false).size(), 0uz);
    EXPECT_EQ(reloaded.list_skills(true).size(), 1uz);
}

TEST_CASE(mcp_skills_sep2640_protocol)
{
    Core::EventLoop loop;
    auto temp_dir = make_temp_mcp_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(temp_dir, FileSystem::RecursionMode::Allowed);
    };

    FakeMCPDelegate delegate(temp_dir);
    WebView::SkillDefinition yt;
    yt.name = "youtube-nav"_string;
    yt.description = "YouTube helper skill."_string;
    yt.matches.append("https://www.youtube.com/*"_string);
    yt.body = "# Guide\n\nCheck `references/checklist.md`."_string;
    Vector<WebView::SkillSupportingFile> files;
    files.append({ .relative_path = "references/checklist.md"_string, .content = "Check audio."_string });
    MUST(delegate.skill_store().save_skill(yt, move(files)));

    auto token = "sep2640-test-token"_string;
    auto server = MUST(MCP::MCPServer::create(delegate, 0, token));
    auto port = server->local_port().value();

    {
        JsonObject init_params;
        init_params.set("protocolVersion"sv, "2026-07-28"sv);
        auto init_resp = call_mcp_rpc(port, token, 1, "initialize"sv, move(init_params));
        auto result = init_resp.get_object("result"sv);
        EXPECT(result.has_value());
        EXPECT_EQ(result->get_string("protocolVersion"sv).value_or(String {}), "2026-07-28"sv);
        auto caps = result->get_object("capabilities"sv);
        EXPECT(caps.has_value());
        EXPECT(caps->has("resources"sv));
        auto exts = caps->get_object("extensions"sv);
        EXPECT(exts.has_value());
        auto skills_ext = exts->get_object("io.modelcontextprotocol/skills"sv);
        EXPECT(skills_ext.has_value());
        EXPECT(skills_ext->get_bool("directoryRead"sv).value_or(false));
    }

    {
        auto discover_resp = call_mcp_rpc(port, token, 2, "server/discover"sv, JsonObject {});
        auto caps = discover_resp.get_object("result"sv)->get_object("capabilities"sv);
        EXPECT(caps->has("resources"sv));
        EXPECT(caps->get_object("extensions"sv)->has("io.modelcontextprotocol/skills"sv));
    }

    {
        auto list_resp = call_mcp_rpc(port, token, 3, "skills/list"sv, JsonObject {});
        auto res = list_resp.get_object("result"sv);
        EXPECT_EQ(res->get_string("resultType"sv).value_or(String {}), "complete"sv);
        EXPECT(res->has("ttlMs"sv));
        EXPECT(res->has("cacheScope"sv));
        auto skills = res->get_array("skills"sv);
        EXPECT_EQ(skills->size(), 1uz);
        auto entry = skills->at(0).as_object();
        EXPECT_EQ(entry.get_string("uri"sv).value_or(String {}), "skill://youtube-nav/SKILL.md"sv);
        EXPECT_EQ(entry.get_object("frontmatter"sv)->get_string("name"sv).value_or(String {}), "youtube-nav"sv);
        EXPECT_EQ(entry.get_array("resources"sv)->size(), 2uz);
    }

    {
        JsonObject p;
        p.set("uri"sv, "skill://youtube-nav/SKILL.md"sv);
        auto get_resp = call_mcp_rpc(port, token, 4, "skills/get"sv, move(p));
        auto skill = get_resp.get_object("result"sv)->get_object("skill"sv);
        EXPECT_EQ(skill->get_string("uri"sv).value_or(String {}), "skill://youtube-nav/SKILL.md"sv);

        JsonObject bad_p;
        bad_p.set("uri"sv, "skill://missing-skill/SKILL.md"sv);
        auto err_resp = call_mcp_rpc(port, token, 5, "skills/get"sv, move(bad_p));
        EXPECT(err_resp.has("error"sv));
        EXPECT_EQ(err_resp.get_object("error"sv)->get_integer<i64>("code"sv).value_or(0), -32602);
    }

    {
        JsonObject p;
        p.set("uri"sv, "skill://youtube-nav/references/checklist.md"sv);
        auto read_resp = call_mcp_rpc(port, token, 6, "resources/read"sv, move(p));
        auto contents = read_resp.get_object("result"sv)->get_array("contents"sv);
        EXPECT_EQ(contents->size(), 1uz);
        EXPECT_EQ(contents->at(0).as_object().get_string("text"sv).value_or(String {}), "Check audio."sv);
    }

    {
        JsonObject p;
        p.set("uri"sv, "skill://youtube-nav"sv);
        auto dir_resp = call_mcp_rpc(port, token, 7, "resources/directory/read"sv, move(p));
        auto resources = dir_resp.get_object("result"sv)->get_array("resources"sv);
        EXPECT_EQ(resources->size(), 2uz);

        JsonObject trailing;
        trailing.set("uri"sv, "skill://youtube-nav/references/"sv);
        auto err_resp = call_mcp_rpc(port, token, 8, "resources/directory/read"sv, move(trailing));
        EXPECT(err_resp.has("error"sv));
        EXPECT_EQ(err_resp.get_object("error"sv)->get_integer<i64>("code"sv).value_or(0), -32602);
    }
}

TEST_CASE(mcp_skills_tools_and_write_gate)
{
    Core::EventLoop loop;
    auto temp_dir = make_temp_mcp_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(temp_dir, FileSystem::RecursionMode::Allowed);
    };

    FakeMCPDelegate delegate(temp_dir);
    WebView::SkillDefinition yt = WebView::make_example_skill();
    MUST(delegate.skill_store().save_skill(yt));

    delegate.mock_tabs.append({
        .id = 10,
        .title = "YouTube Studio"_string,
        .url = "https://studio.youtube.com/channel/UC123"_string,
        .is_active = true,
    });
    delegate.mock_tabs.append({
        .id = 11,
        .title = "Example"_string,
        .url = "https://example.com/"_string,
        .is_active = false,
    });

    auto token = "skills-tools-token"_string;
    auto server = MUST(MCP::MCPServer::create(delegate, 0, token));
    auto port = server->local_port().value();

    {
        auto tabs_res = call_mcp_tool(port, token, 1, "list_tabs"sv, JsonObject {});
        auto payload = parse_tool_json_payload(tabs_res);
        auto tabs = payload.get_array("tabs"sv);
        EXPECT_EQ(tabs->size(), 2uz);
        auto yt_tab = tabs->at(0).as_object();
        auto other_tab = tabs->at(1).as_object();
        EXPECT_EQ(yt_tab.get_array("matching_skills"sv)->size(), 1uz);
        EXPECT(!other_tab.has("matching_skills"sv));
    }

    {
        JsonObject args;
        args.set("tab_id"sv, 10);
        auto res = call_mcp_tool(port, token, 2, "list_skills"sv, move(args));
        auto payload = parse_tool_json_payload(res);
        EXPECT_EQ(payload.get_integer<u64>("count"sv).value_or(0), 1u);

        JsonObject args_none;
        args_none.set("tab_id"sv, 11);
        auto res_none = call_mcp_tool(port, token, 3, "list_skills"sv, move(args_none));
        auto payload_none = parse_tool_json_payload(res_none);
        EXPECT_EQ(payload_none.get_integer<u64>("count"sv).value_or(1), 0u);
    }

    {
        JsonObject args;
        args.set("name"sv, "youtube-navigation"sv);
        auto res = call_mcp_tool(port, token, 4, "read_skill"sv, move(args));
        EXPECT(!res.get_bool("isError"sv).value_or(true));
        auto payload = parse_tool_json_payload(res);
        EXPECT(payload.get_string("content"sv).value_or(String {}).contains("YouTube & Studio Playbook"sv));
    }

    {
        JsonObject skill_obj;
        skill_obj.set("name"sv, "agent-created-skill"sv);
        skill_obj.set("description"sv, "Created via MCP."sv);
        skill_obj.set("body"sv, "# Body\n"_string);
        JsonObject args;
        args.set("skill"sv, move(skill_obj));

        auto blocked = call_mcp_tool(port, token, 5, "save_skill"sv, args);
        EXPECT(blocked.get_bool("isError"sv).value_or(false));

        delegate.allow_skill_write = true;
        auto allowed = call_mcp_tool(port, token, 6, "save_skill"sv, move(args));
        EXPECT(!allowed.get_bool("isError"sv).value_or(true));
        EXPECT(delegate.skill_store().get_skill("agent-created-skill"sv).has_value());

        JsonObject del_args;
        del_args.set("name"sv, "agent-created-skill"sv);
        auto del_res = call_mcp_tool(port, token, 7, "delete_skill"sv, move(del_args));
        EXPECT(!del_res.get_bool("isError"sv).value_or(true));
        EXPECT(!delegate.skill_store().get_skill("agent-created-skill"sv).has_value());
    }
}
