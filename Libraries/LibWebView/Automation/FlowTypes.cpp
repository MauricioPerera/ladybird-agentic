/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibWebView/Automation/FlowTypes.h>

namespace WebView {

static u32 clamp_timeout_ms(i64 value)
{
    if (value < 100)
        return 100;
    if (value > 120000)
        return 120000;
    return static_cast<u32>(value);
}

static u32 clamp_sleep_ms(i64 value)
{
    if (value < 0)
        return 0;
    if (value > 60000)
        return 60000;
    return static_cast<u32>(value);
}

bool is_valid_flow_id(StringView id)
{
    if (id.is_empty() || id.length() > 64)
        return false;

    auto first = id[0];
    bool first_valid = (first >= 'a' && first <= 'z') || (first >= '0' && first <= '9');
    if (!first_valid)
        return false;

    for (size_t i = 1; i < id.length(); ++i) {
        auto ch = id[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!ok)
            return false;
    }
    return true;
}

bool is_valid_flow_source(StringView source)
{
    if (source == "user"sv || source == "mcp"sv || source == "unknown"sv)
        return true;
    if (source.starts_with("extension:"sv))
        return is_valid_flow_id(source.substring_view("extension:"sv.length()));
    return false;
}

StringView flow_action_name(FlowStep::Action action)
{
    switch (action) {
    case FlowStep::Action::Open:
        return "open"sv;
    case FlowStep::Action::Reload:
        return "reload"sv;
    case FlowStep::Action::Back:
        return "back"sv;
    case FlowStep::Action::Forward:
        return "forward"sv;
    case FlowStep::Action::Click:
        return "click"sv;
    case FlowStep::Action::Fill:
        return "fill"sv;
    case FlowStep::Action::Press:
        return "press"sv;
    case FlowStep::Action::WaitFor:
        return "wait_for"sv;
    case FlowStep::Action::AssertText:
        return "assert_text"sv;
    case FlowStep::Action::AssertVisible:
        return "assert_visible"sv;
    case FlowStep::Action::AssertUrl:
        return "assert_url"sv;
    case FlowStep::Action::Extract:
        return "extract"sv;
    case FlowStep::Action::Eval:
        return "eval"sv;
    case FlowStep::Action::Sleep:
        return "sleep"sv;
    case FlowStep::Action::Screenshot:
        return "screenshot"sv;
    case FlowStep::Action::UploadFile:
        return "upload_file"sv;
    }
    VERIFY_NOT_REACHED();
}

StringView flow_locator_kind_name(FlowLocator::Kind kind)
{
    switch (kind) {
    case FlowLocator::Kind::Css:
        return "css"sv;
    case FlowLocator::Kind::XPath:
        return "xpath"sv;
    case FlowLocator::Kind::Role:
        return "role"sv;
    case FlowLocator::Kind::Label:
        return "label"sv;
    case FlowLocator::Kind::Placeholder:
        return "placeholder"sv;
    case FlowLocator::Kind::Text:
        return "text"sv;
    case FlowLocator::Kind::TestId:
        return "test_id"sv;
    }
    VERIFY_NOT_REACHED();
}

static Optional<FlowStep::Action> parse_step_action(StringView action)
{
    if (action == "open"sv)
        return FlowStep::Action::Open;
    if (action == "reload"sv)
        return FlowStep::Action::Reload;
    if (action == "back"sv)
        return FlowStep::Action::Back;
    if (action == "forward"sv)
        return FlowStep::Action::Forward;
    if (action == "click"sv)
        return FlowStep::Action::Click;
    if (action == "fill"sv)
        return FlowStep::Action::Fill;
    if (action == "press"sv)
        return FlowStep::Action::Press;
    if (action == "wait_for"sv)
        return FlowStep::Action::WaitFor;
    if (action == "assert_text"sv)
        return FlowStep::Action::AssertText;
    if (action == "assert_visible"sv)
        return FlowStep::Action::AssertVisible;
    if (action == "assert_url"sv)
        return FlowStep::Action::AssertUrl;
    if (action == "extract"sv)
        return FlowStep::Action::Extract;
    if (action == "eval"sv)
        return FlowStep::Action::Eval;
    if (action == "sleep"sv)
        return FlowStep::Action::Sleep;
    if (action == "screenshot"sv)
        return FlowStep::Action::Screenshot;
    if (action == "upload_file"sv)
        return FlowStep::Action::UploadFile;
    return {};
}

static ErrorOr<FlowLocator> parse_locator(JsonValue const& value)
{
    if (!value.is_object())
        return Error::from_string_literal("Step locator must be an object");

    auto const& obj = value.as_object();
    auto kind_str = obj.get_string("kind"sv).value_or("css"_string);

    FlowLocator locator;
    if (kind_str == "css"sv)
        locator.kind = FlowLocator::Kind::Css;
    else if (kind_str == "xpath"sv)
        locator.kind = FlowLocator::Kind::XPath;
    else if (kind_str == "role"sv)
        locator.kind = FlowLocator::Kind::Role;
    else if (kind_str == "label"sv)
        locator.kind = FlowLocator::Kind::Label;
    else if (kind_str == "placeholder"sv)
        locator.kind = FlowLocator::Kind::Placeholder;
    else if (kind_str == "text"sv)
        locator.kind = FlowLocator::Kind::Text;
    else if (kind_str == "test_id"sv || kind_str == "testId"sv)
        locator.kind = FlowLocator::Kind::TestId;
    else
        return Error::from_string_literal("Unsupported locator kind");

    if (auto exact = obj.get_bool("exact"sv); exact.has_value())
        locator.exact = *exact;

    if (auto nth = obj.get_integer<i64>("nth"sv); nth.has_value()) {
        if (*nth < 0 || *nth > 1000)
            return Error::from_string_literal("Locator 'nth' must be between 0 and 1000");
        locator.nth = static_cast<size_t>(*nth);
    }

    if (locator.kind == FlowLocator::Kind::Role) {
        auto role = obj.get_string("role"sv);
        if (!role.has_value() || role->bytes_as_string_view().trim_whitespace().is_empty())
            return Error::from_string_literal("Role locator requires a non-empty 'role' field");
        locator.role = MUST(role->trim_ascii_whitespace());
        if (auto name = obj.get_string("name"sv); name.has_value() && !name->is_empty())
            locator.name = *name;
        else if (auto val = obj.get_string("value"sv); val.has_value() && !val->is_empty())
            locator.name = *val;
    } else {
        auto val = obj.get_string("value"sv);
        if (!val.has_value() || val->is_empty())
            return Error::from_string_literal("Locator requires a non-empty 'value' field");
        locator.value = *val;
    }

    return locator;
}

static JsonObject serialize_locator(FlowLocator const& locator)
{
    JsonObject obj;
    obj.set("kind"sv, flow_locator_kind_name(locator.kind));
    if (locator.kind == FlowLocator::Kind::Role) {
        obj.set("role"sv, locator.role);
        if (locator.name.has_value())
            obj.set("name"sv, *locator.name);
    } else {
        obj.set("value"sv, locator.value);
    }
    if (locator.exact)
        obj.set("exact"sv, true);
    if (locator.nth.has_value())
        obj.set("nth"sv, static_cast<u64>(*locator.nth));
    return obj;
}

static ErrorOr<FlowStep> parse_step(JsonValue const& value)
{
    if (!value.is_object())
        return Error::from_string_literal("Each flow step must be an object");

    auto const& obj = value.as_object();
    auto action_str = obj.get_string("action"sv);
    if (!action_str.has_value())
        return Error::from_string_literal("Step missing 'action' string");

    auto parsed_action = parse_step_action(*action_str);
    if (!parsed_action.has_value())
        return Error::from_string_literal("Unsupported step action");

    FlowStep step;
    step.action = *parsed_action;
    if (auto label = obj.get_string("label"sv); label.has_value() && !label->is_empty())
        step.label = *label;
    if (auto url = obj.get_string("url"sv); url.has_value())
        step.url = *url;
    if (auto maybe_locator = obj.get("locator"sv); maybe_locator.has_value() && !maybe_locator->is_null())
        step.locator = TRY(parse_locator(*maybe_locator));
    if (auto text = obj.get_string("text"sv); text.has_value())
        step.text = *text;
    if (auto paths = obj.get_array("paths"sv); paths.has_value()) {
        for (auto const& entry : paths->values()) {
            if (!entry.is_string())
                return Error::from_string_literal("Step 'paths' entries must be strings");
            step.paths.append(entry.as_string());
        }
    } else if (auto path = obj.get_string("path"sv); path.has_value()) {
        step.paths.append(*path);
    }
    if (auto key = obj.get_string("key"sv); key.has_value())
        step.key = *key;
    if (auto clear = obj.get_bool("clear"sv); clear.has_value())
        step.clear = *clear;
    if (auto contains = obj.get_string("contains"sv); contains.has_value())
        step.contains = *contains;
    if (auto equals = obj.get_string("equals"sv); equals.has_value())
        step.equals = *equals;
    if (auto wait_for_load = obj.get_bool("wait_for_load"sv); wait_for_load.has_value())
        step.wait_for_load = *wait_for_load;
    if (auto script = obj.get_string("script"sv); script.has_value())
        step.script = *script;
    if (auto save_as = obj.get_string("save_as"sv); save_as.has_value() && !save_as->is_empty())
        step.save_as = *save_as;
    else if (auto name = obj.get_string("name"sv); name.has_value() && !name->is_empty())
        step.save_as = *name;
    if (auto timeout_ms = obj.get_integer<i64>("timeout_ms"sv); timeout_ms.has_value())
        step.timeout_ms = clamp_timeout_ms(*timeout_ms);
    if (auto ms = obj.get_integer<i64>("ms"sv); ms.has_value())
        step.ms = clamp_sleep_ms(*ms);

    switch (step.action) {
    case FlowStep::Action::Open:
        if (!step.url.has_value() || step.url->bytes_as_string_view().trim_whitespace().is_empty())
            return Error::from_string_literal("Step 'open' requires a non-empty 'url'");
        break;
    case FlowStep::Action::Click:
    case FlowStep::Action::WaitFor:
    case FlowStep::Action::AssertVisible:
    case FlowStep::Action::Extract:
        if (!step.locator.has_value())
            return Error::from_string_literal("Step requires a 'locator' object");
        break;
    case FlowStep::Action::Fill:
        if (!step.locator.has_value())
            return Error::from_string_literal("Step 'fill' requires a 'locator' object");
        if (!step.text.has_value())
            return Error::from_string_literal("Step 'fill' requires a 'text' string");
        break;
    case FlowStep::Action::Press:
        if (!step.key.has_value() || step.key->is_empty())
            return Error::from_string_literal("Step 'press' requires a non-empty 'key' string");
        break;
    case FlowStep::Action::AssertText:
        if (!step.locator.has_value())
            return Error::from_string_literal("Step 'assert_text' requires a 'locator' object");
        if (!step.contains.has_value() && !step.equals.has_value())
            return Error::from_string_literal("Step 'assert_text' requires 'contains' or 'equals'");
        break;
    case FlowStep::Action::AssertUrl:
        if (!step.contains.has_value() && !step.equals.has_value())
            return Error::from_string_literal("Step 'assert_url' requires 'contains' or 'equals'");
        break;
    case FlowStep::Action::Eval:
        if (!step.script.has_value() || step.script->bytes_as_string_view().trim_whitespace().is_empty())
            return Error::from_string_literal("Step 'eval' requires a non-empty 'script'");
        break;
    case FlowStep::Action::Reload:
    case FlowStep::Action::Back:
    case FlowStep::Action::Forward:
    case FlowStep::Action::Sleep:
    case FlowStep::Action::Screenshot:
        break;
    case FlowStep::Action::UploadFile:
        if (!step.locator.has_value())
            return Error::from_string_literal("Step 'upload_file' requires a 'locator' object");
        if (step.paths.is_empty())
            return Error::from_string_literal("Step 'upload_file' requires a non-empty 'path' or 'paths'");
        for (auto const& p : step.paths) {
            if (p.bytes_as_string_view().trim_whitespace().is_empty())
                return Error::from_string_literal("Step 'upload_file' paths must not be empty");
        }
        break;
    }

    return step;
}

ErrorOr<FlowDefinition> parse_flow_definition(JsonValue const& value)
{
    if (!value.is_object())
        return Error::from_string_literal("Flow definition must be a JSON object");

    auto const& obj = value.as_object();

    FlowDefinition flow;
    if (auto version = obj.get_integer<i64>("schema_version"sv); version.has_value()) {
        if (*version != 1)
            return Error::from_string_literal("Unsupported flow schema_version (expected 1)");
        flow.schema_version = 1;
    }

    auto id = obj.get_string("id"sv);
    if (!id.has_value() || !is_valid_flow_id(*id))
        return Error::from_string_literal("Invalid flow 'id': expected 1..64 lowercase [a-z0-9_-] starting with [a-z0-9]");
    flow.id = *id;

    auto name = obj.get_string("name"sv);
    if (!name.has_value() || name->bytes_as_string_view().trim_whitespace().is_empty())
        return Error::from_string_literal("Flow requires a non-empty 'name'");
    flow.name = MUST(name->trim_ascii_whitespace());

    if (auto desc = obj.get_string("description"sv); desc.has_value())
        flow.description = *desc;

    if (auto source = obj.get_string("source"sv); source.has_value() && is_valid_flow_source(*source))
        flow.source = *source;
    else
        flow.source = "unknown"_string;

    if (auto timeout_ms = obj.get_integer<i64>("default_timeout_ms"sv); timeout_ms.has_value())
        flow.default_timeout_ms = clamp_timeout_ms(*timeout_ms);

    if (auto created_at = obj.get_integer<i64>("created_at_ms"sv); created_at.has_value())
        flow.created_at_ms = *created_at;
    if (auto updated_at = obj.get_integer<i64>("updated_at_ms"sv); updated_at.has_value())
        flow.updated_at_ms = *updated_at;

    if (auto vars_obj = obj.get_object("variables"sv); vars_obj.has_value()) {
        vars_obj->for_each_member([&](String const& key, JsonValue const& val) {
            if (val.is_string())
                flow.variables.set(key, val.as_string());
            else if (val.is_number() || val.is_bool())
                flow.variables.set(key, val.serialized());
        });
    }

    auto steps_arr = obj.get_array("steps"sv);
    if (!steps_arr.has_value() || steps_arr->is_empty())
        return Error::from_string_literal("Flow requires a non-empty 'steps' array");

    flow.steps.ensure_capacity(steps_arr->size());
    for (auto const& step_val : steps_arr->values())
        flow.steps.append(TRY(parse_step(step_val)));

    return flow;
}

JsonObject serialize_flow_definition(FlowDefinition const& flow)
{
    JsonObject obj;
    obj.set("schema_version"sv, static_cast<u64>(flow.schema_version));
    obj.set("id"sv, flow.id);
    obj.set("name"sv, flow.name);
    obj.set("description"sv, flow.description);
    obj.set("source"sv, flow.source);
    obj.set("default_timeout_ms"sv, static_cast<u64>(flow.default_timeout_ms));
    obj.set("created_at_ms"sv, flow.created_at_ms);
    obj.set("updated_at_ms"sv, flow.updated_at_ms);

    JsonObject vars;
    for (auto const& [k, v] : flow.variables)
        vars.set(k, v);
    obj.set("variables"sv, move(vars));

    JsonArray steps;
    steps.ensure_capacity(flow.steps.size());
    for (auto const& step : flow.steps) {
        JsonObject s;
        s.set("action"sv, flow_action_name(step.action));
        if (step.label.has_value())
            s.set("label"sv, *step.label);
        if (step.url.has_value())
            s.set("url"sv, *step.url);
        if (step.locator.has_value())
            s.set("locator"sv, serialize_locator(*step.locator));
        if (step.text.has_value())
            s.set("text"sv, *step.text);
        if (!step.paths.is_empty()) {
            JsonArray paths_arr;
            paths_arr.ensure_capacity(step.paths.size());
            for (auto const& p : step.paths)
                paths_arr.must_append(p);
            s.set("paths"sv, move(paths_arr));
        }
        if (step.key.has_value())
            s.set("key"sv, *step.key);
        if (step.action == FlowStep::Action::Fill)
            s.set("clear"sv, step.clear);
        if (step.contains.has_value())
            s.set("contains"sv, *step.contains);
        if (step.equals.has_value())
            s.set("equals"sv, *step.equals);
        if (step.action == FlowStep::Action::Open || step.action == FlowStep::Action::Reload)
            s.set("wait_for_load"sv, step.wait_for_load);
        if (step.script.has_value())
            s.set("script"sv, *step.script);
        if (step.save_as.has_value())
            s.set("save_as"sv, *step.save_as);
        if (step.timeout_ms.has_value())
            s.set("timeout_ms"sv, static_cast<u64>(*step.timeout_ms));
        if (step.ms.has_value())
            s.set("ms"sv, static_cast<u64>(*step.ms));
        steps.must_append(move(s));
    }
    obj.set("steps"sv, move(steps));
    return obj;
}

JsonObject serialize_flow_summary(FlowDefinition const& flow)
{
    JsonObject obj;
    obj.set("id"sv, flow.id);
    obj.set("name"sv, flow.name);
    obj.set("description"sv, flow.description);
    obj.set("source"sv, flow.source);
    obj.set("step_count"sv, static_cast<u64>(flow.steps.size()));
    obj.set("default_timeout_ms"sv, static_cast<u64>(flow.default_timeout_ms));
    obj.set("updated_at_ms"sv, flow.updated_at_ms);
    return obj;
}

JsonObject serialize_flow_run(FlowRunRecord const& run)
{
    JsonObject obj;
    obj.set("run_id"sv, run.run_id);
    obj.set("flow_id"sv, run.flow_id);
    obj.set("flow_name"sv, run.flow_name);
    obj.set("status"sv, run.status);
    if (run.tab_id.has_value())
        obj.set("tab_id"sv, *run.tab_id);
    obj.set("started_at_ms"sv, run.started_at_ms);
    if (run.finished_at_ms.has_value())
        obj.set("finished_at_ms"sv, *run.finished_at_ms);
    obj.set("duration_ms"sv, run.duration_ms);
    obj.set("current_step_index"sv, static_cast<u64>(run.current_step_index));
    obj.set("total_steps"sv, static_cast<u64>(run.total_steps));
    if (run.error.has_value())
        obj.set("error"sv, *run.error);
    obj.set("outputs"sv, run.outputs);

    JsonArray steps;
    steps.ensure_capacity(run.steps.size());
    for (auto const& step : run.steps) {
        JsonObject s;
        s.set("index"sv, static_cast<u64>(step.index));
        s.set("action"sv, step.action);
        if (step.label.has_value())
            s.set("label"sv, *step.label);
        s.set("status"sv, step.status);
        s.set("duration_ms"sv, step.duration_ms);
        if (step.error.has_value())
            s.set("error"sv, *step.error);
        if (!step.output.is_null())
            s.set("output"sv, step.output);
        if (step.screenshot_path.has_value())
            s.set("screenshot_path"sv, *step.screenshot_path);
        steps.must_append(move(s));
    }
    obj.set("steps"sv, move(steps));
    return obj;
}

Vector<String> extract_flow_template_variables(StringView input)
{
    Vector<String> keys;
    if (input.is_empty())
        return keys;

    size_t pos = 0;
    while (pos < input.length()) {
        auto open = input.find("{{"sv, pos);
        if (!open.has_value())
            break;
        auto close = input.find("}}"sv, *open + 2);
        if (!close.has_value())
            break;
        auto raw_key = input.substring_view(*open + 2, *close - (*open + 2));
        auto key = MUST(String::from_utf8(raw_key.trim_whitespace().bytes()));
        if (!key.is_empty() && !keys.contains_slow(key))
            keys.append(move(key));
        pos = *close + 2;
    }
    return keys;
}

String interpolate_flow_variables(StringView input, HashMap<String, String> const& vars)
{
    if (input.is_empty() || vars.is_empty())
        return String::from_utf8_without_validation(input.bytes());

    StringBuilder builder;
    size_t pos = 0;
    while (pos < input.length()) {
        auto open = input.find("{{"sv, pos);
        if (!open.has_value()) {
            builder.append(input.substring_view(pos));
            break;
        }
        builder.append(input.substring_view(pos, *open - pos));
        auto close = input.find("}}"sv, *open + 2);
        if (!close.has_value()) {
            builder.append(input.substring_view(*open));
            break;
        }
        auto raw_key = input.substring_view(*open + 2, *close - (*open + 2));
        auto key = raw_key.trim_whitespace();
        if (auto found = vars.get(key); found.has_value()) {
            builder.append(*found);
        } else {
            builder.append(input.substring_view(*open, (*close + 2) - *open));
        }
        pos = *close + 2;
    }
    return MUST(builder.to_string());
}

String build_locator_action_script(StringView operation, FlowLocator const& locator, JsonObject const& params)
{
    JsonObject config;
    config.set("operation"sv, operation);
    config.set("locator"sv, serialize_locator(locator));
    config.set("params"sv, params);
    auto config_json = JsonValue { move(config) }.serialized();

    static constexpr auto script_prefix = R"JS(return (function(cfg) {
  function norm(s) {
    return String(s == null ? '' : s).replace(/\s+/g, ' ').trim();
  }
  function isVisible(el) {
    if (!el || !el.isConnected) return false;
    var style = window.getComputedStyle(el);
    if (!style || style.display === 'none' || style.visibility === 'hidden' || parseFloat(style.opacity || '1') === 0) return false;
    var rect = el.getBoundingClientRect();
    return rect.width > 0 && rect.height > 0;
  }
  function isDisabled(el) {
    return Boolean(el.disabled || el.getAttribute('aria-disabled') === 'true');
  }
  function matchText(actual, expected, exact) {
    var a = norm(actual);
    var e = norm(expected);
    if (exact) return a === e;
    return a.toLowerCase().indexOf(e.toLowerCase()) !== -1;
  }
  function implicitRole(el) {
    var tag = el.tagName ? el.tagName.toLowerCase() : '';
    if (tag === 'button') return 'button';
    if (tag === 'a' && el.hasAttribute('href')) return 'link';
    if (tag === 'input') {
      var type = (el.getAttribute('type') || 'text').toLowerCase();
      if (type === 'button' || type === 'submit' || type === 'reset') return 'button';
      if (type === 'checkbox') return 'checkbox';
      if (type === 'radio') return 'radio';
      if (type === 'search') return 'searchbox';
      if (type !== 'hidden') return 'textbox';
    }
    if (tag === 'textarea') return 'textbox';
    if (tag === 'select') return 'combobox';
    if (tag === 'img') return 'img';
    if (tag === 'nav') return 'navigation';
    if (tag === 'main') return 'main';
    if (tag === 'dialog') return 'dialog';
    if (tag === 'ul' || tag === 'ol') return 'list';
    if (tag === 'li') return 'listitem';
    if (tag === 'table') return 'table';
    if (tag === 'tr') return 'row';
    if (tag === 'td') return 'cell';
    if (/^h[1-6]$/.test(tag)) return 'heading';
    return '';
  }
  function queryAllDeep(root, sel, out, visited) {
    out = out || [];
    visited = visited || new Set();
    if (!root || visited.has(root)) return out;
    visited.add(root);
    try {
      var direct = root.querySelectorAll(sel);
      for (var i = 0; i < direct.length; ++i) out.push(direct[i]);
    } catch (e) {}
    var all = root.querySelectorAll('*');
    for (var i = 0; i < all.length; ++i) {
      if (all[i].shadowRoot) queryAllDeep(all[i].shadowRoot, sel, out, visited);
    }
    return out;
  }
  function walkElementsDeep(root, out, visited) {
    out = out || [];
    visited = visited || new Set();
    if (!root || visited.has(root)) return out;
    visited.add(root);
    var all = root.querySelectorAll('*');
    for (var i = 0; i < all.length; ++i) {
      out.push(all[i]);
      if (all[i].shadowRoot) walkElementsDeep(all[i].shadowRoot, out, visited);
    }
    return out;
  }
  function labelTextFor(el) {
    var rootNode = typeof el.getRootNode === 'function' ? el.getRootNode() : document;
    if (el.getAttribute) {
      var ariaLabel = el.getAttribute('aria-label');
      if (ariaLabel) return ariaLabel;
      var labelledBy = el.getAttribute('aria-labelledby');
      if (labelledBy) {
        var parts = labelledBy.trim().split(/\s+/).map(function(id) {
          var target = (rootNode && rootNode.getElementById ? rootNode.getElementById(id) : null) || document.getElementById(id);
          return target ? norm(target.textContent) : '';
        }).filter(Boolean);
        if (parts.length > 0) return parts.join(' ');
      }
    }
    if (el.id) {
      var labels = rootNode && rootNode.querySelectorAll ? rootNode.querySelectorAll('label') : [];
      for (var i = 0; i < labels.length; ++i) {
        if (labels[i].getAttribute('for') === el.id)
          return norm(labels[i].textContent);
      }
      if (rootNode !== document) {
        var docLabels = document.querySelectorAll('label');
        for (var j = 0; j < docLabels.length; ++j) {
          if (docLabels[j].getAttribute('for') === el.id)
            return norm(docLabels[j].textContent);
        }
      }
    }
    var parent = el.closest ? el.closest('label') : null;
    if (parent) return norm(parent.textContent);
    return '';
  }
  function accessibleName(el) {
    var lbl = labelTextFor(el);
    if (lbl) return lbl;
    var tag = el.tagName ? el.tagName.toLowerCase() : '';
    if (tag === 'input' && ['button', 'submit', 'reset'].indexOf((el.getAttribute('type') || '').toLowerCase()) !== -1)
      return el.value || '';
    if (el.getAttribute) {
      var alt = el.getAttribute('alt');
      if (alt) return alt;
      var placeholder = el.getAttribute('placeholder');
      if (placeholder) return placeholder;
      var title = el.getAttribute('title');
      if (title) return title;
    }
    return norm(el.innerText !== undefined ? el.innerText : el.textContent);
  }
  function resolveAll(loc) {
    var kind = loc.kind || 'css';
    var exact = Boolean(loc.exact);
    if (kind === 'css') {
      return queryAllDeep(document, loc.value);
    }
    if (kind === 'xpath') {
      var snap = document.evaluate(loc.value, document, null, XPathResult.ORDERED_NODE_SNAPSHOT_TYPE, null);
      var out = [];
      for (var i = 0; i < snap.snapshotLength; ++i) {
        var n = snap.snapshotItem(i);
        if (n && n.nodeType === 1) out.push(n);
      }
      return out;
    }
    if (kind === 'test_id') {
      var all = queryAllDeep(document, '[data-testid]');
      var matches = [];
      for (var i = 0; i < all.length; ++i) {
        if (all[i].getAttribute('data-testid') === loc.value)
          matches.push(all[i]);
      }
      return matches;
    }
    if (kind === 'placeholder') {
      var inputs = queryAllDeep(document, 'input, textarea');
      var matches = [];
      for (var i = 0; i < inputs.length; ++i) {
        var ph = inputs[i].getAttribute('placeholder');
        if (ph !== null && matchText(ph, loc.value, exact))
          matches.push(inputs[i]);
      }
      return matches;
    }
    if (kind === 'label') {
      var controls = queryAllDeep(document, 'input, textarea, select, button, [role], [contenteditable="true"]');
      var matches = [];
      for (var i = 0; i < controls.length; ++i) {
        var lbl = labelTextFor(controls[i]);
        if (lbl && matchText(lbl, loc.value, exact))
          matches.push(controls[i]);
      }
      return matches;
    }
    if (kind === 'role') {
      var wantRole = String(loc.role || '').toLowerCase();
      var candidates = walkElementsDeep(document);
      var matches = [];
      for (var i = 0; i < candidates.length; ++i) {
        var el = candidates[i];
        var r = (el.getAttribute('role') || implicitRole(el)).toLowerCase();
        if (r !== wantRole) continue;
        if (!isVisible(el)) continue;
        if (loc.name !== undefined && loc.name !== null && loc.name !== '') {
          if (!matchText(accessibleName(el), loc.name, exact)) continue;
        }
        matches.push(el);
      }
      return matches;
    }
    if (kind === 'text') {
      var all = walkElementsDeep(document);
      var rawMatches = [];
      for (var i = 0; i < all.length; ++i) {
        var el = all[i];
        var tag = el.tagName ? el.tagName.toLowerCase() : '';
        if (tag === 'html' || tag === 'body' || tag === 'head' || tag === 'script' || tag === 'style') continue;
        if (!isVisible(el)) continue;
        var txt = el.innerText !== undefined ? el.innerText : el.textContent;
        if (matchText(txt, loc.value, exact)) rawMatches.push(el);
      }
      return rawMatches.filter(function(el) {
        for (var j = 0; j < rawMatches.length; ++j) {
          if (rawMatches[j] !== el && el.contains(rawMatches[j])) return false;
        }
        return true;
      });
    }
    throw new Error('Unsupported locator kind: ' + kind);
  }
  function isFileInput(node) {
    return Boolean(node
      && String(node.tagName || '').toLowerCase() === 'input'
      && String(node.getAttribute('type') || node.type || '').toLowerCase() === 'file');
  }
  function resolveFileInput(node) {
    if (!node) return null;
    if (isFileInput(node)) return node;
    if (String(node.tagName || '').toLowerCase() === 'label' && isFileInput(node.control))
      return node.control;
    if (typeof node.closest === 'function') {
      var lbl = node.closest('label');
      if (lbl && isFileInput(lbl.control)) return lbl.control;
    }
    var inside = queryAllDeep(node, 'input[type="file"]');
    if (inside.length > 0) return inside[0];
    if (node.shadowRoot) {
      var inShadow = queryAllDeep(node.shadowRoot, 'input[type="file"]');
      if (inShadow.length > 0) return inShadow[0];
    }
    var root = typeof node.getRootNode === 'function' ? node.getRootNode() : null;
    while (root && root !== document) {
      var inRoot = queryAllDeep(root, 'input[type="file"]');
      if (inRoot.length > 0) return inRoot[0];
      var host = root.host;
      if (!host) break;
      if (host.parentElement) {
        var inSibling = queryAllDeep(host.parentElement, 'input[type="file"]');
        if (inSibling.length > 0) return inSibling[0];
      }
      root = typeof host.getRootNode === 'function' ? host.getRootNode() : null;
    }
    if (typeof node.closest === 'function') {
      var container = node.closest('ytcp-uploads-file-picker, [role="dialog"], dialog, form');
      if (container) {
        var inContainer = queryAllDeep(container, 'input[type="file"]');
        if (inContainer.length > 0) return inContainer[0];
      }
    }
    return null;
  }
  function setNativeValue(el, value) {
    var proto = el instanceof HTMLTextAreaElement ? HTMLTextAreaElement.prototype : (el instanceof HTMLInputElement ? HTMLInputElement.prototype : null);
    var desc = proto ? Object.getOwnPropertyDescriptor(proto, 'value') : null;
    if (desc && desc.set) desc.set.call(el, value);
    else el.value = value;
  }
  try {
    var matches = resolveAll(cfg.locator || {});
    var visibleMatches = matches.filter(isVisible);
    var pool = (cfg.operation === 'extract' || cfg.operation === 'assert_text' || cfg.operation === 'resolve_upload_input') && visibleMatches.length === 0 ? matches : visibleMatches;
    if (pool.length === 0) {
      return { ok: false, retryable: true, error: matches.length > 0 ? 'Matched element is hidden' : 'Element not found' };
    }
    var idx = typeof cfg.locator.nth === 'number' ? cfg.locator.nth : 0;
    if (idx < 0 || idx >= pool.length) {
      return { ok: false, retryable: true, error: 'Matched element index ' + idx + ' out of bounds (count=' + pool.length + ')' };
    }
    var el = pool[idx];
    var op = cfg.operation;
    var p = cfg.params || {};
    if (op === 'resolve_upload_input') {
      var fileInput = resolveFileInput(el);
      if (!fileInput) {
        return { ok: false, retryable: true, error: 'Matched element has no associated <input type="file">' };
      }
      return { ok: true, element: fileInput };
    }
    if (op === 'wait_for' || op === 'assert_visible') {
      return { ok: true, count: pool.length };
    }
    if (op === 'click') {
      if (isDisabled(el)) return { ok: false, retryable: true, error: 'Element is disabled' };
      if (el.scrollIntoView) el.scrollIntoView({ block: 'center', inline: 'center' });
      if (el.focus) el.focus();
      el.click();
      return { ok: true };
    }
    if (op === 'fill') {
      if (isDisabled(el)) return { ok: false, retryable: true, error: 'Element is disabled' };
      if (el.scrollIntoView) el.scrollIntoView({ block: 'center', inline: 'center' });
      if (el.focus) el.focus();
      var text = String(p.text == null ? '' : p.text);
      var shouldClear = p.clear !== false;
      if (el.isContentEditable) {
        el.textContent = shouldClear ? text : (String(el.textContent || '') + text);
      } else {
        var nextVal = shouldClear ? text : (String(el.value || '') + text);
        setNativeValue(el, nextVal);
      }
      el.dispatchEvent(new Event('input', { bubbles: true }));
      el.dispatchEvent(new Event('change', { bubbles: true }));
      return { ok: true };
    }
    if (op === 'press') {
      if (el.scrollIntoView) el.scrollIntoView({ block: 'center', inline: 'center' });
      if (el.focus) el.focus();
      var k = String(p.key || '');
      var evInit = { key: k, bubbles: true, cancelable: true };
      var notCancelled = el.dispatchEvent(new KeyboardEvent('keydown', evInit));
      if (k === 'Enter' || k.length === 1)
        el.dispatchEvent(new KeyboardEvent('keypress', evInit));
      el.dispatchEvent(new KeyboardEvent('keyup', evInit));
      if (k === 'Enter' && notCancelled && el.form) {
        if (typeof el.form.requestSubmit === 'function') el.form.requestSubmit();
        else el.form.submit();
      }
      return { ok: true };
    }
    if (op === 'extract' || op === 'assert_text') {
      var val = '';
      var tag = el.tagName ? el.tagName.toLowerCase() : '';
      if (tag === 'input' || tag === 'textarea' || tag === 'select')
        val = String(el.value == null ? '' : el.value);
      else
        val = norm(el.innerText !== undefined ? el.innerText : el.textContent);
      if (op === 'extract')
        return { ok: true, text: val };
      if (p.equals !== undefined && p.equals !== null && norm(val) !== norm(p.equals))
        return { ok: false, retryable: true, error: 'Text "' + val + '" did not equal "' + p.equals + '"', text: val };
      if (p.contains !== undefined && p.contains !== null && val.indexOf(String(p.contains)) === -1)
        return { ok: false, retryable: true, error: 'Text "' + val + '" did not contain "' + p.contains + '"', text: val };
      return { ok: true, text: val };
    }
    return { ok: false, retryable: false, error: 'Unknown locator operation: ' + op };
  } catch (err) {
    return { ok: false, retryable: false, error: String(err && err.message ? err.message : err) };
  }
})(
)JS"sv;

    StringBuilder builder;
    builder.append(script_prefix);
    builder.append(config_json);
    builder.append(");"sv);
    return MUST(builder.to_string());
}

FlowDefinition make_example_flow()
{
    auto now_ms = UnixDateTime::now().milliseconds_since_epoch();

    FlowDefinition flow;
    flow.schema_version = 1;
    flow.id = "example-smoke"_string;
    flow.name = "Example Domain Smoke Check"_string;
    flow.description = "Opens example.com, verifies the heading with a semantic role locator, extracts it, and captures document.title."_string;
    flow.default_timeout_ms = 10000;
    flow.created_at_ms = now_ms;
    flow.updated_at_ms = now_ms;
    flow.variables.set("target_url"_string, "http://example.com"_string);
    flow.variables.set("expected_heading"_string, "Example Domain"_string);

    FlowStep open_step;
    open_step.action = FlowStep::Action::Open;
    open_step.label = "Open example page"_string;
    open_step.url = "{{target_url}}"_string;
    open_step.wait_for_load = true;
    flow.steps.append(move(open_step));

    FlowStep wait_step;
    wait_step.action = FlowStep::Action::WaitFor;
    wait_step.label = "Wait for main heading"_string;
    FlowLocator wait_locator;
    wait_locator.kind = FlowLocator::Kind::Role;
    wait_locator.role = "heading"_string;
    wait_locator.name = "{{expected_heading}}"_string;
    wait_step.locator = move(wait_locator);
    flow.steps.append(move(wait_step));

    FlowStep assert_step;
    assert_step.action = FlowStep::Action::AssertText;
    assert_step.label = "Verify heading text"_string;
    FlowLocator h1_locator;
    h1_locator.kind = FlowLocator::Kind::Css;
    h1_locator.value = "h1"_string;
    assert_step.locator = h1_locator;
    assert_step.contains = "{{expected_heading}}"_string;
    flow.steps.append(move(assert_step));

    FlowStep extract_step;
    extract_step.action = FlowStep::Action::Extract;
    extract_step.label = "Extract heading"_string;
    extract_step.locator = move(h1_locator);
    extract_step.save_as = "heading"_string;
    flow.steps.append(move(extract_step));

    FlowStep eval_step;
    eval_step.action = FlowStep::Action::Eval;
    eval_step.label = "Read document title"_string;
    eval_step.script = "return document.title;"_string;
    eval_step.save_as = "title"_string;
    flow.steps.append(move(eval_step));

    return flow;
}

}
