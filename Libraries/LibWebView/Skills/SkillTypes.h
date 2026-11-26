/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>

namespace WebView {

constexpr size_t MAX_SKILL_FILES = 512;
constexpr u64 MAX_SKILL_TOTAL_BYTES = 16 * 1024 * 1024;

struct WEBVIEW_API SkillFileEntry {
    String relative_path;
    String uri;
    String mime_type;
    String digest;
    u64 size_bytes { 0 };
};

struct WEBVIEW_API SkillSupportingFile {
    String relative_path;
    String content;
};

struct WEBVIEW_API SkillResourceContent {
    String uri;
    String mime_type;
    String text;
    String digest;
    u64 size_bytes { 0 };
};

struct WEBVIEW_API SkillDirectoryEntry {
    String uri;
    String name;
    String mime_type;
};

struct WEBVIEW_API SkillDefinition {
    String name;
    String description;
    Vector<String> matches;
    bool enabled { true };
    HashMap<String, JsonValue> extra_frontmatter;
    String body;
    String raw_skill_md;
    Vector<SkillFileEntry> resources;
    i64 created_at_ms { 0 };
    i64 updated_at_ms { 0 };
};

struct WEBVIEW_API ParsedSkillUri {
    String name;
    String relative_path;
};

WEBVIEW_API bool is_valid_skill_name(StringView name);
WEBVIEW_API bool is_safe_skill_relative_path(StringView path);
WEBVIEW_API String skill_uri(StringView name, StringView relative_path = "SKILL.md"sv);
WEBVIEW_API Optional<ParsedSkillUri> parse_skill_uri(StringView uri, bool allow_empty_relative_path = false);
WEBVIEW_API String guess_skill_mime_type(StringView relative_path);

WEBVIEW_API ErrorOr<SkillDefinition> parse_skill_markdown(StringView raw_markdown, Optional<StringView> expected_name = {});
WEBVIEW_API String render_skill_markdown(SkillDefinition const& skill);

WEBVIEW_API bool skill_matches_url(SkillDefinition const& skill, URL::URL const& url);
WEBVIEW_API String suggest_match_pattern_for_url(URL::URL const& url);

WEBVIEW_API JsonObject serialize_skill_frontmatter(SkillDefinition const& skill);
WEBVIEW_API JsonObject serialize_skill_mcp_entry(SkillDefinition const& skill);
WEBVIEW_API JsonObject serialize_skill_match_hint(SkillDefinition const& skill);
WEBVIEW_API JsonObject serialize_skill_for_ui(SkillDefinition const& skill, Vector<SkillSupportingFile> const& supporting_files = {});

WEBVIEW_API ErrorOr<SkillDefinition> parse_skill_json(JsonValue const& value, Vector<SkillSupportingFile>& out_supporting_files);
WEBVIEW_API SkillDefinition make_example_skill(Optional<String> site_url = {});

}
