/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/StringBuilder.h>
#include <LibWebView/Extensions/ExtensionTypes.h>

namespace WebView {

static bool is_supported_permission(StringView permission)
{
    return permission.is_one_of(
        "storage"sv,
        "tabs"sv,
        "scripting"sv,
        "flows"sv,
        "mcp"sv,
        "activeTab"sv,
        "contextMenus"sv);
}

static bool is_valid_run_at(StringView run_at)
{
    return run_at.is_one_of("document_start"sv, "document_end"sv, "document_idle"sv);
}

static bool wildcard_match(StringView text, StringView pattern)
{
    size_t ti = 0;
    size_t pi = 0;
    Optional<size_t> star_pi;
    size_t star_ti = 0;

    while (ti < text.length()) {
        if (pi < pattern.length() && pattern[pi] == text[ti]) {
            ++pi;
            ++ti;
        } else if (pi < pattern.length() && pattern[pi] == '*') {
            star_pi = pi++;
            star_ti = ti;
        } else if (star_pi.has_value()) {
            pi = *star_pi + 1;
            ti = ++star_ti;
        } else {
            return false;
        }
    }

    while (pi < pattern.length() && pattern[pi] == '*')
        ++pi;

    return pi == pattern.length();
}

bool is_valid_extension_id(StringView id)
{
    if (id.is_empty() || id.length() > 64)
        return false;

    auto first = id[0];
    bool first_ok = (first >= 'a' && first <= 'z') || (first >= '0' && first <= '9');
    if (!first_ok)
        return false;

    for (size_t i = 1; i < id.length(); ++i) {
        auto ch = id[i];
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!ok)
            return false;
    }
    return true;
}

String sanitize_extension_id_from_name(StringView raw)
{
    StringBuilder builder;
    bool prev_dash = false;
    for (auto byte : raw.bytes()) {
        auto ch = static_cast<char>(byte);
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch - 'A' + 'a');
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            builder.append(ch);
            prev_dash = false;
        } else if ((ch == '-' || ch == '_' || ch == ' ' || ch == '.') && !builder.is_empty() && !prev_dash) {
            builder.append(ch == '_' ? '_' : '-');
            prev_dash = true;
        }
        if (builder.length() >= 48)
            break;
    }
    auto out = MUST(builder.to_string());
    auto trimmed = out.bytes_as_string_view().trim("-"sv, TrimMode::Right);
    if (trimmed.is_empty())
        return "extension"_string;
    auto candidate = MUST(String::from_utf8(trimmed));
    if (!is_valid_extension_id(candidate))
        return "extension"_string;
    return candidate;
}

bool is_safe_relative_extension_path(StringView rel_path)
{
    if (rel_path.is_empty())
        return false;
    if (rel_path.starts_with('/') || rel_path.starts_with('\\'))
        return false;
    for (auto ch : rel_path.bytes()) {
        if (ch == '\0' || ch == '\\' || ch == ':')
            return false;
    }
    for (auto segment : rel_path.split_view('/', SplitBehavior::KeepEmpty)) {
        if (segment.is_empty() || segment == "."sv || segment == ".."sv)
            return false;
    }
    return true;
}

bool url_matches_extension_pattern(URL::URL const& url, StringView pattern)
{
    auto scheme = url.scheme();
    if (scheme != "http"sv && scheme != "https"sv)
        return false;

    if (pattern == "<all_urls>"sv)
        return true;

    auto scheme_sep = pattern.find("://"sv);
    if (!scheme_sep.has_value())
        return false;

    auto scheme_pat = pattern.substring_view(0, *scheme_sep);
    if (scheme_pat != "*"sv && !scheme_pat.equals_ignoring_ascii_case(scheme))
        return false;

    auto rest = pattern.substring_view(*scheme_sep + 3);
    auto path_sep = rest.find('/');
    if (!path_sep.has_value())
        return false;

    auto host_pat = rest.substring_view(0, *path_sep);
    auto path_pat = rest.substring_view(*path_sep);
    if (host_pat.is_empty() || path_pat.is_empty())
        return false;

    auto host = url.serialized_host();
    if (host.is_empty())
        return false;

    if (host_pat != "*"sv) {
        if (host_pat.starts_with("*."sv)) {
            auto base_domain = host_pat.substring_view(2);
            if (base_domain.is_empty() || base_domain.contains('*'))
                return false;
            bool exact_base = host.equals_ignoring_ascii_case(base_domain);
            bool subdomain = host.length() > base_domain.length() + 1
                && host.substring_view(host.length() - base_domain.length()).equals_ignoring_ascii_case(base_domain)
                && host[host.length() - base_domain.length() - 1] == '.';
            if (!exact_base && !subdomain)
                return false;
        } else if (host_pat.contains('*')) {
            return false;
        } else if (!host.equals_ignoring_ascii_case(host_pat)) {
            return false;
        }
    }

    StringBuilder full_path;
    auto path_part = url.serialize_path();
    full_path.append(path_part.is_empty() ? "/"sv : path_part);
    if (auto query = url.query(); query.has_value()) {
        full_path.append('?');
        full_path.append(*query);
    }

    return wildcard_match(full_path.string_view(), path_pat);
}

static ErrorOr<Vector<String>> parse_string_list(JsonObject const& obj, StringView key)
{
    Vector<String> items;
    if (!obj.has(key))
        return items;
    auto arr = obj.get_array(key);
    if (!arr.has_value())
        return Error::from_string_literal("Expected array of strings");
    for (auto const& val : arr->values()) {
        if (!val.is_string())
            return Error::from_string_literal("Expected string element in array");
        items.append(val.as_string());
    }
    return items;
}

static ErrorOr<Vector<String>> parse_safe_path_list(JsonObject const& obj, StringView key)
{
    auto paths = TRY(parse_string_list(obj, key));
    for (auto const& path : paths) {
        if (!is_safe_relative_extension_path(path))
            return Error::from_string_literal("Extension manifest contains unsafe or invalid relative path");
    }
    return paths;
}

ErrorOr<ExtensionManifest> parse_extension_manifest(JsonValue const& value, StringView fallback_id)
{
    if (!value.is_object())
        return Error::from_string_literal("Extension manifest must be a JSON object");

    auto const& obj = value.as_object();
    auto manifest_version = obj.get_integer<u32>("manifest_version"sv);
    if (!manifest_version.has_value() || *manifest_version != 3)
        return Error::from_string_literal("Extension manifest must specify manifest_version: 3");

    auto name = obj.get_string("name"sv);
    if (!name.has_value() || name->bytes_as_string_view().trim_whitespace().is_empty())
        return Error::from_string_literal("Extension manifest requires non-empty 'name'");

    auto version = obj.get_string("version"sv);
    if (!version.has_value() || version->bytes_as_string_view().trim_whitespace().is_empty())
        return Error::from_string_literal("Extension manifest requires non-empty 'version'");

    Optional<String> resolved_id;
    if (auto explicit_id = obj.get_string("id"sv); explicit_id.has_value() && !explicit_id->is_empty())
        resolved_id = *explicit_id;
    else if (auto ladybird = obj.get_object("ladybird"sv); ladybird.has_value()) {
        if (auto lb_id = ladybird->get_string("id"sv); lb_id.has_value() && !lb_id->is_empty())
            resolved_id = *lb_id;
    }

    if (!resolved_id.has_value()) {
        if (!fallback_id.is_empty())
            resolved_id = MUST(String::from_utf8(fallback_id));
        else
            resolved_id = sanitize_extension_id_from_name(*name);
    }

    if (!is_valid_extension_id(*resolved_id))
        return Error::from_string_literal("Invalid extension id (must be lowercase a-z0-9, '-' or '_', max 64 chars)");

    ExtensionManifest manifest;
    manifest.manifest_version = *manifest_version;
    manifest.id = *resolved_id;
    manifest.name = *name;
    manifest.version = *version;
    manifest.description = obj.get_string("description"sv).value_or(String {});
    manifest.permissions = TRY(parse_string_list(obj, "permissions"sv));
    for (auto const& perm : manifest.permissions) {
        if (!is_supported_permission(perm))
            return Error::from_string_literal("Unsupported extension permission in manifest");
    }
    manifest.host_permissions = TRY(parse_string_list(obj, "host_permissions"sv));

    if (obj.has("content_scripts"sv)) {
        auto scripts = obj.get_array("content_scripts"sv);
        if (!scripts.has_value())
            return Error::from_string_literal("'content_scripts' must be an array");

        for (auto const& item : scripts->values()) {
            if (!item.is_object())
                return Error::from_string_literal("Each content_scripts entry must be an object");
            auto const& cs_obj = item.as_object();

            ExtensionContentScript cs;
            cs.matches = TRY(parse_string_list(cs_obj, "matches"sv));
            if (cs.matches.is_empty())
                return Error::from_string_literal("content_scripts entry requires non-empty 'matches'");

            cs.exclude_matches = TRY(parse_string_list(cs_obj, "exclude_matches"sv));
            cs.js = TRY(parse_safe_path_list(cs_obj, "js"sv));
            cs.css = TRY(parse_safe_path_list(cs_obj, "css"sv));
            if (cs.js.is_empty() && cs.css.is_empty())
                return Error::from_string_literal("content_scripts entry requires at least one 'js' or 'css' file");

            if (auto run_at = cs_obj.get_string("run_at"sv); run_at.has_value()) {
                if (!is_valid_run_at(*run_at))
                    return Error::from_string_literal("Invalid content_scripts 'run_at' (expected document_start, document_end, or document_idle)");
                cs.run_at = *run_at;
            }
            manifest.content_scripts.append(move(cs));
        }
    }

    if (auto action_obj = obj.get_object("action"sv); action_obj.has_value()) {
        ExtensionAction action;
        action.default_title = action_obj->get_string("default_title"sv).copy();
        if (auto popup = action_obj->get_string("default_popup"sv); popup.has_value()) {
            if (!is_safe_relative_extension_path(*popup))
                return Error::from_string_literal("Invalid action.default_popup path");
            action.default_popup = *popup;
        }
        action.default_flow_id = action_obj->get_string("default_flow_id"sv).copy();
        manifest.action = move(action);
    }

    if (auto ladybird_obj = obj.get_object("ladybird"sv); ladybird_obj.has_value()) {
        manifest.ladybird_flow_files = TRY(parse_safe_path_list(*ladybird_obj, "flows"sv));
    }

    return manifest;
}

JsonObject serialize_extension(InstalledExtension const& ext)
{
    JsonObject obj;
    obj.set("id"sv, ext.manifest.id);
    obj.set("name"sv, ext.manifest.name);
    obj.set("version"sv, ext.manifest.version);
    obj.set("description"sv, ext.manifest.description);
    obj.set("manifest_version"sv, static_cast<u64>(ext.manifest.manifest_version));
    obj.set("root_path"sv, String::from_utf8_without_validation(ext.root_path.bytes()));
    obj.set("enabled"sv, ext.enabled);
    obj.set("allow_in_private"sv, ext.allow_in_private);
    obj.set("installed_at_ms"sv, ext.installed_at_ms);
    obj.set("updated_at_ms"sv, ext.updated_at_ms);
    if (ext.last_error.has_value())
        obj.set("last_error"sv, *ext.last_error);

    JsonArray permissions;
    for (auto const& perm : ext.manifest.permissions)
        permissions.must_append(perm);
    obj.set("permissions"sv, move(permissions));

    JsonArray host_permissions;
    for (auto const& hp : ext.manifest.host_permissions)
        host_permissions.must_append(hp);
    obj.set("host_permissions"sv, move(host_permissions));

    JsonArray content_scripts;
    for (auto const& cs : ext.manifest.content_scripts) {
        JsonObject cs_obj;
        JsonArray matches;
        for (auto const& m : cs.matches)
            matches.must_append(m);
        cs_obj.set("matches"sv, move(matches));

        JsonArray js;
        for (auto const& f : cs.js)
            js.must_append(f);
        cs_obj.set("js"sv, move(js));

        JsonArray css;
        for (auto const& f : cs.css)
            css.must_append(f);
        cs_obj.set("css"sv, move(css));
        cs_obj.set("run_at"sv, cs.run_at);
        content_scripts.must_append(move(cs_obj));
    }
    obj.set("content_scripts"sv, move(content_scripts));

    if (ext.manifest.action.has_value()) {
        JsonObject action;
        if (ext.manifest.action->default_title.has_value())
            action.set("default_title"sv, *ext.manifest.action->default_title);
        if (ext.manifest.action->default_popup.has_value())
            action.set("default_popup"sv, *ext.manifest.action->default_popup);
        if (ext.manifest.action->default_flow_id.has_value())
            action.set("default_flow_id"sv, *ext.manifest.action->default_flow_id);
        obj.set("action"sv, move(action));
    }

    JsonArray flow_files;
    for (auto const& ff : ext.manifest.ladybird_flow_files)
        flow_files.must_append(ff);
    obj.set("flow_files"sv, move(flow_files));

    JsonArray flow_ids;
    for (auto const& fid : ext.registered_flow_ids)
        flow_ids.must_append(fid);
    obj.set("registered_flow_ids"sv, move(flow_ids));

    return obj;
}

String build_extension_injection_script(StringView extension_id, StringView extension_name, Vector<String> const& css_contents, Vector<String> const& js_contents, JsonObject const& storage_snapshot)
{
    JsonArray css_array;
    for (auto const& css : css_contents)
        css_array.must_append(css);

    JsonObject cfg;
    cfg.set("id"sv, extension_id);
    cfg.set("name"sv, extension_name);
    cfg.set("css"sv, move(css_array));
    cfg.set("storage"sv, storage_snapshot);

    StringBuilder builder;
    builder.append("return (function(__cfg) {\n"sv);
    builder.append("  var __errors = [];\n"sv);
    builder.append("  var __injectedCss = 0;\n"sv);
    builder.append("  var __injectedJs = 0;\n"sv);
    builder.append("  try {\n"sv);
    builder.append("    var __root = document.head || document.documentElement;\n"sv);
    builder.append("    if (__root && __cfg.css && __cfg.css.length > 0) {\n"sv);
    builder.append("      var __existing = document.querySelectorAll('style[data-ladybird-ext-id]');\n"sv);
    builder.append("      for (var i = 0; i < __existing.length; i++) {\n"sv);
    builder.append("        if (__existing[i].getAttribute('data-ladybird-ext-id') === __cfg.id) __existing[i].remove();\n"sv);
    builder.append("      }\n"sv);
    builder.append("      for (var j = 0; j < __cfg.css.length; j++) {\n"sv);
    builder.append("        var __style = document.createElement('style');\n"sv);
    builder.append("        __style.setAttribute('data-ladybird-ext-id', __cfg.id);\n"sv);
    builder.append("        __style.textContent = __cfg.css[j];\n"sv);
    builder.append("        __root.appendChild(__style);\n"sv);
    builder.append("        __injectedCss++;\n"sv);
    builder.append("      }\n"sv);
    builder.append("    }\n"sv);
    builder.append("  } catch (e) {\n"sv);
    builder.append("    __errors.push(String(e && e.message ? e.message : e));\n"sv);
    builder.append("  }\n"sv);

    builder.append("  var __store = __cfg.storage || {};\n"sv);
    builder.append("  var __api = {\n"sv);
    builder.append("    id: __cfg.id,\n"sv);
    builder.append("    name: __cfg.name,\n"sv);
    builder.append("    runtime: { id: __cfg.id },\n"sv);
    builder.append("    storage: {\n"sv);
    builder.append("      local: {\n"sv);
    builder.append("        _snapshot: __store,\n"sv);
    builder.append("        get: function(keys) {\n"sv);
    builder.append("          var out = {};\n"sv);
    builder.append("          if (keys === null || keys === undefined) {\n"sv);
    builder.append("            for (var k in __store) { if (Object.prototype.hasOwnProperty.call(__store, k)) out[k] = __store[k]; }\n"sv);
    builder.append("          } else if (typeof keys === 'string') {\n"sv);
    builder.append("            if (Object.prototype.hasOwnProperty.call(__store, keys)) out[keys] = __store[keys];\n"sv);
    builder.append("          } else if (Array.isArray(keys)) {\n"sv);
    builder.append("            for (var i = 0; i < keys.length; i++) { var key = keys[i]; if (Object.prototype.hasOwnProperty.call(__store, key)) out[key] = __store[key]; }\n"sv);
    builder.append("          } else if (typeof keys === 'object') {\n"sv);
    builder.append("            for (var k2 in keys) { if (Object.prototype.hasOwnProperty.call(keys, k2)) out[k2] = Object.prototype.hasOwnProperty.call(__store, k2) ? __store[k2] : keys[k2]; }\n"sv);
    builder.append("          }\n"sv);
    builder.append("          return Promise.resolve(out);\n"sv);
    builder.append("        }\n"sv);
    builder.append("      }\n"sv);
    builder.append("    }\n"sv);
    builder.append("  };\n"sv);

    for (auto const& js : js_contents) {
        builder.append("  try {\n"sv);
        builder.append("    (function(browser, ladybirdExt) {\n"sv);
        builder.append(js);
        builder.append("\n    })(__api, __api);\n"sv);
        builder.append("    __injectedJs++;\n"sv);
        builder.append("  } catch (e) {\n"sv);
        builder.append("    __errors.push(String(e && e.message ? e.message : e));\n"sv);
        builder.append("  }\n"sv);
    }

    builder.append("  return { ok: __errors.length === 0, extension_id: __cfg.id, injected_css: __injectedCss, injected_js: __injectedJs, errors: __errors };\n"sv);
    builder.append("})("sv);
    builder.append(JsonValue { move(cfg) }.serialized());
    builder.append(");"sv);
    return MUST(builder.to_string());
}

}
