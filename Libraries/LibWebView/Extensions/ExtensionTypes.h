/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API ExtensionContentScript {
    Vector<String> matches;
    Vector<String> exclude_matches;
    Vector<String> js;
    Vector<String> css;
    String run_at { "document_idle"_string };
};

struct WEBVIEW_API ExtensionAction {
    Optional<String> default_title;
    Optional<String> default_popup;
    Optional<String> default_flow_id;
};

struct WEBVIEW_API ExtensionManifest {
    u32 manifest_version { 3 };
    String id;
    String name;
    String version;
    String description;
    Vector<String> permissions;
    Vector<String> host_permissions;
    Vector<ExtensionContentScript> content_scripts;
    Optional<ExtensionAction> action;
    Vector<String> ladybird_flow_files;
};

struct WEBVIEW_API InstalledExtension {
    ExtensionManifest manifest;
    ByteString root_path;
    bool enabled { true };
    bool allow_in_private { false };
    i64 installed_at_ms { 0 };
    i64 updated_at_ms { 0 };
    Optional<String> last_error;
    Vector<String> registered_flow_ids;
};

WEBVIEW_API bool is_valid_extension_id(StringView id);
WEBVIEW_API String sanitize_extension_id_from_name(StringView raw);
WEBVIEW_API bool is_safe_relative_extension_path(StringView rel_path);
WEBVIEW_API bool url_matches_extension_pattern(URL::URL const& url, StringView pattern);
WEBVIEW_API ErrorOr<ExtensionManifest> parse_extension_manifest(JsonValue const& value, StringView fallback_id = {});
WEBVIEW_API JsonObject serialize_extension(InstalledExtension const&);
WEBVIEW_API String build_extension_injection_script(StringView extension_id, StringView extension_name, Vector<String> const& css_contents, Vector<String> const& js_contents, JsonObject const& storage_snapshot);

}
