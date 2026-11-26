/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/JsonArray.h>
#include <AK/QuickSort.h>
#include <AK/StringBuilder.h>
#include <LibURL/Parser.h>
#include <LibWebView/Extensions/ExtensionTypes.h>
#include <LibWebView/Skills/SkillTypes.h>

namespace WebView {

bool is_valid_skill_name(StringView name)
{
    if (name.is_empty() || name.length() > 64)
        return false;
    if (name.starts_with('-') || name.ends_with('-'))
        return false;
    bool prev_hyphen = false;
    for (auto ch : name.bytes()) {
        bool valid_char = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
        if (!valid_char)
            return false;
        if (ch == '-' && prev_hyphen)
            return false;
        prev_hyphen = (ch == '-');
    }
    return true;
}

bool is_safe_skill_relative_path(StringView path)
{
    if (path.is_empty() || path.starts_with('/') || path.ends_with('/'))
        return false;
    if (path.contains('\\') || path.contains(':') || path.contains("//"sv))
        return false;
    for (u8 byte : path.bytes()) {
        if (byte < 0x20 || byte == 0x7f)
            return false;
    }
    for (auto segment : path.split_view('/', SplitBehavior::KeepEmpty)) {
        if (segment.is_empty() || segment == "."sv || segment == ".."sv)
            return false;
    }
    return true;
}

String skill_uri(StringView name, StringView relative_path)
{
    if (relative_path.is_empty())
        return MUST(String::formatted("skill://{}", name));
    return MUST(String::formatted("skill://{}/{}", name, relative_path));
}

Optional<ParsedSkillUri> parse_skill_uri(StringView uri, bool allow_empty_relative_path)
{
    constexpr auto prefix = "skill://"sv;
    if (!uri.starts_with(prefix) || uri.ends_with('/'))
        return {};

    auto rest = uri.substring_view(prefix.length());
    if (rest.is_empty())
        return {};

    auto slash = rest.find('/');
    if (!slash.has_value()) {
        if (!allow_empty_relative_path || !is_valid_skill_name(rest))
            return {};
        return ParsedSkillUri {
            .name = MUST(String::from_utf8(rest)),
            .relative_path = String {},
        };
    }

    auto name = rest.substring_view(0, *slash);
    auto rel_path = rest.substring_view(*slash + 1);
    if (!is_valid_skill_name(name) || !is_safe_skill_relative_path(rel_path))
        return {};

    return ParsedSkillUri {
        .name = MUST(String::from_utf8(name)),
        .relative_path = MUST(String::from_utf8(rel_path)),
    };
}

String guess_skill_mime_type(StringView relative_path)
{
    if (relative_path.ends_with(".md"sv, CaseSensitivity::CaseInsensitive) || relative_path.ends_with(".markdown"sv, CaseSensitivity::CaseInsensitive))
        return "text/markdown"_string;
    if (relative_path.ends_with(".json"sv, CaseSensitivity::CaseInsensitive))
        return "application/json"_string;
    if (relative_path.ends_with(".yaml"sv, CaseSensitivity::CaseInsensitive) || relative_path.ends_with(".yml"sv, CaseSensitivity::CaseInsensitive))
        return "application/yaml"_string;
    if (relative_path.ends_with(".js"sv, CaseSensitivity::CaseInsensitive) || relative_path.ends_with(".mjs"sv, CaseSensitivity::CaseInsensitive))
        return "text/javascript"_string;
    if (relative_path.ends_with(".py"sv, CaseSensitivity::CaseInsensitive))
        return "text/x-python"_string;
    if (relative_path.ends_with(".html"sv, CaseSensitivity::CaseInsensitive) || relative_path.ends_with(".htm"sv, CaseSensitivity::CaseInsensitive))
        return "text/html"_string;
    if (relative_path.ends_with(".css"sv, CaseSensitivity::CaseInsensitive))
        return "text/css"_string;
    return "text/plain"_string;
}

static StringView trim_ascii_ws(StringView sv)
{
    return sv.trim(" \t\r\n"sv);
}

static ErrorOr<String> unquote_yaml_scalar(StringView raw, bool* out_was_quoted = nullptr)
{
    auto trimmed = trim_ascii_ws(raw);
    if (out_was_quoted)
        *out_was_quoted = false;

    if (trimmed.length() >= 2 && trimmed.starts_with('"') && trimmed.ends_with('"')) {
        if (out_was_quoted)
            *out_was_quoted = true;
        auto inner = trimmed.substring_view(1, trimmed.length() - 2);
        StringBuilder sb;
        for (size_t i = 0; i < inner.length(); ++i) {
            if (inner[i] == '\\' && i + 1 < inner.length()) {
                auto next = inner[++i];
                switch (next) {
                case 'n':
                    sb.append('\n');
                    break;
                case 'r':
                    sb.append('\r');
                    break;
                case 't':
                    sb.append('\t');
                    break;
                case '\\':
                    sb.append('\\');
                    break;
                case '"':
                    sb.append('"');
                    break;
                default:
                    sb.append(next);
                    break;
                }
            } else {
                sb.append(inner[i]);
            }
        }
        return sb.to_string();
    }

    if (trimmed.length() >= 2 && trimmed.starts_with('\'') && trimmed.ends_with('\'')) {
        if (out_was_quoted)
            *out_was_quoted = true;
        auto inner = trimmed.substring_view(1, trimmed.length() - 2);
        StringBuilder sb;
        for (size_t i = 0; i < inner.length(); ++i) {
            if (inner[i] == '\'' && i + 1 < inner.length() && inner[i + 1] == '\'') {
                sb.append('\'');
                ++i;
            } else {
                sb.append(inner[i]);
            }
        }
        return sb.to_string();
    }

    return String::from_utf8(trimmed);
}

static ErrorOr<Vector<String>> parse_inline_yaml_array(StringView raw)
{
    auto trimmed = trim_ascii_ws(raw);
    if (!trimmed.starts_with('[') || !trimmed.ends_with(']'))
        return Error::from_string_literal("Expected inline YAML array");

    auto inner = trim_ascii_ws(trimmed.substring_view(1, trimmed.length() - 2));
    Vector<String> items;
    if (inner.is_empty())
        return items;

    size_t start = 0;
    bool in_single = false;
    bool in_double = false;
    bool escaped = false;
    for (size_t i = 0; i <= inner.length(); ++i) {
        if (i == inner.length() || (inner[i] == ',' && !in_single && !in_double)) {
            auto token = trim_ascii_ws(inner.substring_view(start, i - start));
            if (!token.is_empty())
                items.append(TRY(unquote_yaml_scalar(token)));
            start = i + 1;
            continue;
        }
        auto ch = inner[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (in_double) {
            if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                in_double = false;
        } else if (in_single) {
            if (ch == '\'')
                in_single = false;
        } else {
            if (ch == '"')
                in_double = true;
            else if (ch == '\'')
                in_single = true;
        }
    }
    return items;
}

static JsonValue scalar_to_json_value(StringView value, bool was_quoted)
{
    if (!was_quoted) {
        if (value == "true"sv)
            return JsonValue { true };
        if (value == "false"sv)
            return JsonValue { false };
    }
    return JsonValue { String::from_utf8_without_validation(value.bytes()) };
}

ErrorOr<SkillDefinition> parse_skill_markdown(StringView raw_markdown, Optional<StringView> expected_name)
{
    if (raw_markdown.is_empty())
        return Error::from_string_literal("SKILL.md is empty");

    auto first_nl = raw_markdown.find('\n');
    if (!first_nl.has_value())
        return Error::from_string_literal("SKILL.md must begin with YAML frontmatter delimited by '---'");

    auto first_line = trim_ascii_ws(raw_markdown.substring_view(0, *first_nl));
    if (first_line != "---"sv)
        return Error::from_string_literal("SKILL.md must begin with '---' YAML frontmatter fence");

    size_t pos = *first_nl + 1;
    Optional<size_t> fm_end_start;
    Optional<size_t> body_start;

    while (pos <= raw_markdown.length()) {
        auto next_nl = raw_markdown.find('\n', pos);
        size_t line_end = next_nl.value_or(raw_markdown.length());
        auto line = raw_markdown.substring_view(pos, line_end - pos);
        if (line.ends_with('\r'))
            line = line.substring_view(0, line.length() - 1);

        if (trim_ascii_ws(line) == "---"sv) {
            fm_end_start = pos;
            body_start = next_nl.has_value() ? (*next_nl + 1) : raw_markdown.length();
            break;
        }
        if (!next_nl.has_value())
            break;
        pos = *next_nl + 1;
    }

    if (!fm_end_start.has_value() || !body_start.has_value())
        return Error::from_string_literal("Missing closing '---' YAML frontmatter delimiter");

    SkillDefinition skill;
    skill.raw_skill_md = TRY(String::from_utf8(raw_markdown));

    auto fm_block = raw_markdown.substring_view(*first_nl + 1, *fm_end_start - (*first_nl + 1));
    Vector<StringView> fm_lines;
    for (auto line : fm_block.split_view('\n', SplitBehavior::KeepEmpty)) {
        if (line.ends_with('\r'))
            line = line.substring_view(0, line.length() - 1);
        fm_lines.append(line);
    }

    auto assign_list_key = [&](StringView key, Vector<String> values) {
        if (key == "matches"sv) {
            skill.matches = move(values);
        } else {
            JsonArray arr;
            for (auto const& v : values)
                arr.must_append(v);
            skill.extra_frontmatter.set(String::from_utf8_without_validation(key.bytes()), JsonValue { move(arr) });
        }
    };

    for (size_t i = 0; i < fm_lines.size(); ++i) {
        auto line = fm_lines[i];
        auto trimmed = trim_ascii_ws(line);
        if (trimmed.is_empty() || trimmed.starts_with('#'))
            continue;

        auto colon = line.find(':');
        if (!colon.has_value())
            continue;

        auto key = trim_ascii_ws(line.substring_view(0, *colon));
        if (key.is_empty())
            continue;

        auto val_part = trim_ascii_ws(line.substring_view(*colon + 1));
        if (val_part.is_empty()) {
            // Look ahead for block list `- item`
            Vector<String> items;
            size_t j = i + 1;
            while (j < fm_lines.size()) {
                auto next_trimmed = trim_ascii_ws(fm_lines[j]);
                if (next_trimmed.is_empty()) {
                    ++j;
                    continue;
                }
                if (next_trimmed.starts_with("- "sv) || next_trimmed == "-"sv) {
                    auto item_raw = next_trimmed.length() > 1 ? trim_ascii_ws(next_trimmed.substring_view(1)) : StringView {};
                    if (!item_raw.is_empty())
                        items.append(TRY(unquote_yaml_scalar(item_raw)));
                    ++j;
                    continue;
                }
                break;
            }
            if (j > i + 1) {
                assign_list_key(key, move(items));
                i = j - 1;
            }
            continue;
        }

        if (val_part.starts_with('[') && val_part.ends_with(']')) {
            auto items = TRY(parse_inline_yaml_array(val_part));
            assign_list_key(key, move(items));
            continue;
        }

        bool was_quoted = false;
        auto scalar = TRY(unquote_yaml_scalar(val_part, &was_quoted));
        if (key == "name"sv) {
            skill.name = move(scalar);
        } else if (key == "description"sv) {
            skill.description = move(scalar);
        } else if (key == "enabled"sv) {
            auto lower = MUST(scalar.to_lowercase());
            if (lower == "false"sv || lower == "no"sv || lower == "0"sv)
                skill.enabled = false;
            else
                skill.enabled = true;
        } else if (key == "matches"sv) {
            if (!scalar.is_empty())
                skill.matches.append(move(scalar));
        } else {
            skill.extra_frontmatter.set(String::from_utf8_without_validation(key.bytes()), scalar_to_json_value(scalar, was_quoted));
        }
    }

    if (!is_valid_skill_name(skill.name))
        return Error::from_string_literal("Skill frontmatter 'name' must be 1-64 lowercase alphanumeric/hyphen characters");
    if (expected_name.has_value() && skill.name != *expected_name)
        return Error::from_string_literal("Skill frontmatter 'name' must match parent directory name");
    if (trim_ascii_ws(skill.description).is_empty())
        return Error::from_string_literal("Skill frontmatter 'description' must be non-empty");

    auto raw_body = raw_markdown.substring_view(*body_start);
    while (raw_body.starts_with('\r') || raw_body.starts_with('\n'))
        raw_body = raw_body.substring_view(1);
    while (raw_body.ends_with('\r') || raw_body.ends_with('\n'))
        raw_body = raw_body.substring_view(0, raw_body.length() - 1);
    skill.body = TRY(String::from_utf8(raw_body));

    return skill;
}

static void append_yaml_quoted(StringBuilder& sb, StringView value)
{
    sb.append('"');
    for (auto ch : value.bytes()) {
        switch (ch) {
        case '\\':
            sb.append("\\\\"sv);
            break;
        case '"':
            sb.append("\\\""sv);
            break;
        case '\n':
            sb.append("\\n"sv);
            break;
        case '\r':
            sb.append("\\r"sv);
            break;
        case '\t':
            sb.append("\\t"sv);
            break;
        default:
            sb.append(ch);
            break;
        }
    }
    sb.append('"');
}

String render_skill_markdown(SkillDefinition const& skill)
{
    StringBuilder sb;
    sb.append("---\n"sv);
    sb.appendff("name: {}\n", skill.name);
    sb.append("description: "sv);
    append_yaml_quoted(sb, skill.description);
    sb.append('\n');
    sb.appendff("enabled: {}\n", skill.enabled ? "true"sv : "false"sv);
    if (!skill.matches.is_empty()) {
        sb.append("matches:\n"sv);
        for (auto const& pattern : skill.matches) {
            auto trimmed = trim_ascii_ws(pattern);
            if (trimmed.is_empty())
                continue;
            sb.append("  - "sv);
            append_yaml_quoted(sb, trimmed);
            sb.append('\n');
        }
    }

    Vector<String> extra_keys;
    extra_keys.ensure_capacity(skill.extra_frontmatter.size());
    for (auto const& [k, _] : skill.extra_frontmatter) {
        if (k == "name"sv || k == "description"sv || k == "enabled"sv || k == "matches"sv)
            continue;
        extra_keys.unchecked_append(k);
    }
    quick_sort(extra_keys, [](String const& a, String const& b) { return a < b; });
    for (auto const& key : extra_keys) {
        auto val = skill.extra_frontmatter.get(key);
        if (!val.has_value())
            continue;
        if (val->is_array()) {
            sb.appendff("{}:\n", key);
            for (auto const& item : val->as_array().values()) {
                sb.append("  - "sv);
                if (item.is_string()) {
                    append_yaml_quoted(sb, item.as_string());
                } else {
                    auto serialized = item.serialized();
                    append_yaml_quoted(sb, serialized);
                }
                sb.append('\n');
            }
        } else if (val->is_bool()) {
            sb.appendff("{}: {}\n", key, val->as_bool() ? "true"sv : "false"sv);
        } else if (val->is_string()) {
            sb.appendff("{}: ", key);
            append_yaml_quoted(sb, val->as_string());
            sb.append('\n');
        } else {
            sb.appendff("{}: ", key);
            append_yaml_quoted(sb, val->serialized());
            sb.append('\n');
        }
    }

    sb.append("---\n\n"sv);
    auto body = trim_ascii_ws(skill.body);
    if (!body.is_empty()) {
        sb.append(skill.body.bytes_as_string_view().trim("\r\n"sv));
        sb.append('\n');
    }
    return MUST(sb.to_string());
}

bool skill_matches_url(SkillDefinition const& skill, URL::URL const& url)
{
    if (!skill.enabled)
        return false;
    auto scheme = url.scheme();
    if (scheme != "http"sv && scheme != "https"sv)
        return false;

    for (auto const& raw_pattern : skill.matches) {
        auto pattern = trim_ascii_ws(raw_pattern);
        if (pattern.is_empty())
            continue;
        if (pattern == "*"sv || pattern == "<all_urls>"sv)
            return true;

        if (!pattern.contains("://"sv)) {
            ByteString normalized;
            if (pattern.contains('/'))
                normalized = ByteString::formatted("*://{}", pattern);
            else
                normalized = ByteString::formatted("*://{}/*", pattern);
            if (url_matches_extension_pattern(url, normalized))
                return true;
            continue;
        }

        if (url_matches_extension_pattern(url, pattern))
            return true;
    }
    return false;
}

String suggest_match_pattern_for_url(URL::URL const& url)
{
    auto host = url.serialized_host();
    if (host.is_empty())
        return "*"_string;
    auto scheme = url.scheme();
    if (scheme != "http"sv && scheme != "https"sv)
        scheme = "https"sv;
    return MUST(String::formatted("{}://{}/*", scheme, host));
}

JsonObject serialize_skill_frontmatter(SkillDefinition const& skill)
{
    JsonObject fm;
    fm.set("name"sv, skill.name);
    fm.set("description"sv, skill.description);
    fm.set("enabled"sv, skill.enabled);

    JsonArray matches_json;
    for (auto const& m : skill.matches)
        matches_json.must_append(m);
    fm.set("matches"sv, move(matches_json));

    for (auto const& [k, v] : skill.extra_frontmatter) {
        if (k == "name"sv || k == "description"sv || k == "enabled"sv || k == "matches"sv)
            continue;
        fm.set(k, v);
    }
    return fm;
}

JsonObject serialize_skill_mcp_entry(SkillDefinition const& skill)
{
    JsonObject entry;
    entry.set("uri"sv, skill_uri(skill.name, "SKILL.md"sv));
    entry.set("frontmatter"sv, serialize_skill_frontmatter(skill));

    JsonArray resources_json;
    for (auto const& res : skill.resources) {
        JsonObject r;
        r.set("uri"sv, res.uri);
        r.set("digest"sv, res.digest);
        r.set("size"sv, res.size_bytes);
        resources_json.must_append(move(r));
    }
    entry.set("resources"sv, move(resources_json));
    return entry;
}

JsonObject serialize_skill_match_hint(SkillDefinition const& skill)
{
    JsonObject hint;
    hint.set("name"sv, skill.name);
    hint.set("uri"sv, skill_uri(skill.name, "SKILL.md"sv));
    hint.set("description"sv, skill.description);
    return hint;
}

JsonObject serialize_skill_for_ui(SkillDefinition const& skill, Vector<SkillSupportingFile> const& supporting_files)
{
    JsonObject obj;
    obj.set("name"sv, skill.name);
    obj.set("uri"sv, skill_uri(skill.name, "SKILL.md"sv));
    obj.set("description"sv, skill.description);
    obj.set("enabled"sv, skill.enabled);
    obj.set("body"sv, skill.body);
    obj.set("rawSkillMd"sv, skill.raw_skill_md.is_empty() ? render_skill_markdown(skill) : skill.raw_skill_md);
    obj.set("createdAtMs"sv, skill.created_at_ms);
    obj.set("updatedAtMs"sv, skill.updated_at_ms);

    JsonArray matches_json;
    for (auto const& m : skill.matches)
        matches_json.must_append(m);
    obj.set("matches"sv, move(matches_json));

    JsonArray resources_json;
    for (auto const& res : skill.resources) {
        JsonObject r;
        r.set("relativePath"sv, res.relative_path);
        r.set("uri"sv, res.uri);
        r.set("mimeType"sv, res.mime_type);
        r.set("digest"sv, res.digest);
        r.set("size"sv, res.size_bytes);
        resources_json.must_append(move(r));
    }
    obj.set("resources"sv, move(resources_json));

    JsonArray files_json;
    for (auto const& f : supporting_files) {
        JsonObject item;
        item.set("path"sv, f.relative_path);
        item.set("content"sv, f.content);
        files_json.must_append(move(item));
    }
    obj.set("supportingFiles"sv, move(files_json));

    if (!skill.extra_frontmatter.is_empty()) {
        JsonObject extra;
        for (auto const& [k, v] : skill.extra_frontmatter)
            extra.set(k, v);
        obj.set("extraFrontmatter"sv, move(extra));
    }
    return obj;
}

ErrorOr<SkillDefinition> parse_skill_json(JsonValue const& value, Vector<SkillSupportingFile>& out_supporting_files)
{
    out_supporting_files.clear();
    if (!value.is_object())
        return Error::from_string_literal("Expected JSON object for skill definition");

    auto const& obj = value.as_object();
    auto name = obj.get_string("name"sv);
    if (!name.has_value() || !is_valid_skill_name(*name))
        return Error::from_string_literal("Invalid skill 'name' (expected 1-64 lowercase alphanumeric/hyphen chars, e.g. 'youtube-navigation')");

    auto description = obj.get_string("description"sv);
    if (!description.has_value() || trim_ascii_ws(*description).is_empty())
        return Error::from_string_literal("Skill 'description' must be non-empty");

    SkillDefinition skill;
    skill.name = *name;
    skill.description = MUST(String::from_utf8(trim_ascii_ws(*description)));
    skill.enabled = obj.get_bool("enabled"sv).value_or(true);
    skill.body = obj.get_string("body"sv).value_or(String {});

    if (auto matches_arr = obj.get_array("matches"sv); matches_arr.has_value()) {
        for (auto const& m : matches_arr->values()) {
            if (!m.is_string())
                return Error::from_string_literal("Each 'matches' entry must be a string");
            auto trimmed = trim_ascii_ws(m.as_string());
            if (!trimmed.is_empty())
                skill.matches.append(MUST(String::from_utf8(trimmed)));
        }
    }

    if (auto extra = obj.get_object("extraFrontmatter"sv); extra.has_value()) {
        extra->for_each_member([&](String const& k, JsonValue const& v) {
            if (k != "name"sv && k != "description"sv && k != "enabled"sv && k != "matches"sv)
                skill.extra_frontmatter.set(k, v);
        });
    } else if (auto extra_snake = obj.get_object("extra_frontmatter"sv); extra_snake.has_value()) {
        extra_snake->for_each_member([&](String const& k, JsonValue const& v) {
            if (k != "name"sv && k != "description"sv && k != "enabled"sv && k != "matches"sv)
                skill.extra_frontmatter.set(k, v);
        });
    }

    auto files_arr = obj.get_array("supportingFiles"sv);

    auto process_files_array = [&](JsonArray const& arr) -> ErrorOr<void> {
        u64 total_bytes = skill.body.bytes().size() + 512;
        if (arr.size() + 1 > MAX_SKILL_FILES)
            return Error::from_string_literal("Too many files in skill (max 512)");
        for (auto const& entry : arr.values()) {
            if (!entry.is_object())
                return Error::from_string_literal("Each supporting file must be an object with 'path' and 'content'");
            auto const& fobj = entry.as_object();
            auto path = fobj.get_string("path"sv).copy();
            if (!path.has_value())
                path = fobj.get_string("relative_path"sv).copy();
            if (!path.has_value() || !is_safe_skill_relative_path(*path) || *path == "SKILL.md"sv)
                return Error::from_string_literal("Invalid supporting file relative path");
            auto content = fobj.get_string("content"sv).value_or(String {});
            total_bytes += content.bytes().size();
            if (total_bytes > MAX_SKILL_TOTAL_BYTES)
                return Error::from_string_literal("Skill exceeds 16 MiB maximum size");
            out_supporting_files.append({
                .relative_path = *path,
                .content = move(content),
            });
        }
        return {};
    };

    if (files_arr.has_value()) {
        TRY(process_files_array(*files_arr));
    } else if (auto snake_files_arr = obj.get_array("supporting_files"sv); snake_files_arr.has_value()) {
        TRY(process_files_array(*snake_files_arr));
    }

    skill.raw_skill_md = render_skill_markdown(skill);
    return skill;
}

static String slugify_host(StringView host)
{
    StringBuilder sb;
    bool prev_dash = false;
    for (auto byte : host.bytes()) {
        char ch = static_cast<char>(to_ascii_lowercase(byte));
        bool alnum = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
        if (alnum) {
            sb.append(ch);
            prev_dash = false;
        } else if (!prev_dash && !sb.is_empty()) {
            sb.append('-');
            prev_dash = true;
        }
        if (sb.length() >= 48)
            break;
    }
    auto s = sb.string_view().trim("-"sv);
    if (s.is_empty())
        return "site-navigation"_string;
    return MUST(String::formatted("{}-guide", s));
}

SkillDefinition make_example_skill(Optional<String> site_url)
{
    if (site_url.has_value() && !site_url->is_empty()) {
        if (auto parsed = URL::Parser::basic_parse(*site_url); parsed.has_value() && parsed->scheme().is_one_of("http"sv, "https"sv)) {
            SkillDefinition skill;
            auto host = parsed->serialized_host();
            skill.name = slugify_host(host);
            skill.description = MUST(String::formatted("Navigation and interaction playbook for {}.", host));
            skill.matches.append(suggest_match_pattern_for_url(*parsed));
            skill.enabled = true;
            skill.body = MUST(String::formatted(
                "# {} Navigation Guide\n\n"
                "Use this skill when automating or inspecting `{}`.\n\n"
                "## Workflow\n"
                "1. Call `list_page_tools` first to see if WebMCP tools are registered on the page.\n"
                "2. Otherwise call `get_page_snapshot` to map visible interactive elements to `@e1` refs.\n"
                "3. Prefer deterministic selectors/labels documented below before scrolling or guessing.\n\n"
                "## Key Actions\n"
                "- Document the main search box, navigation tabs, and primary actions here.\n",
                host,
                host));
            skill.raw_skill_md = render_skill_markdown(skill);
            return skill;
        }
    }

    SkillDefinition yt;
    yt.name = "youtube-navigation"_string;
    yt.description = "How to search, navigate channels/videos, and inspect YouTube Studio analytics in Ladybird."_string;
    yt.matches.append("https://www.youtube.com/*"_string);
    yt.matches.append("https://studio.youtube.com/*"_string);
    yt.enabled = true;
    yt.body = "# YouTube & Studio Playbook\n\n"
              "Follow these steps when assisting the user on YouTube:\n\n"
              "## Watch & Search (`www.youtube.com`)\n"
              "1. Call `get_page_snapshot` to locate the search input (`input[name='search_query']`), video links, and player controls.\n"
              "2. Use `type_text` + `press_key` (`Enter`) to search without manually guessing coordinates.\n\n"
              "## YouTube Studio (`studio.youtube.com`)\n"
              "1. Wait for the dashboard/analytics SPA to settle (`wait_for`) before reading KPI cards.\n"
              "2. To switch date ranges (e.g. 28 days vs 365 days), click the date-picker button via snapshot refs instead of crafting `/period-...` URLs directly.\n"
              "3. Read `references/selectors.md` for verified element hints if DOM snapshots are noisy.\n"_string;
    yt.raw_skill_md = render_skill_markdown(yt);
    return yt;
}

}
