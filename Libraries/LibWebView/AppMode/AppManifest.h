/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>
#include <LibWebView/Options.h>

namespace WebView {

struct WEBVIEW_API AppWindowConfig {
    int width { 1080 };
    int height { 720 };
    int min_width { 360 };
    int min_height { 240 };
    bool resizable { true };
    bool show_tab_bar { false };
};

struct WEBVIEW_API AppServiceConfig {
    String id;
    String label;
    URL::URL default_url;
    Vector<String> origins {};
    String container {};
    bool allow_navigate { true };
    bool allow_notifications { false };
    bool allow_read { false };
    bool allow_webmcp { false };
    bool allow_eval { false };
    bool allow_flows { false };
    bool allow_network { false };
};

enum class AppMCPAccess : u8 {
    Full,
    WebMCPOnly,
};

enum class AppNetworkPolicy : u8 {
    Unrestricted,
    Offline,
};

struct WEBVIEW_API AppMCPConfig {
    bool enabled { true };
    Optional<u16> port {};
    bool allow_webmcp { true };
    AppMCPAccess access { AppMCPAccess::Full };
    Vector<String> allowed_domains {};
};

struct WEBVIEW_API AppPermissions {
    Vector<String> fs_read {};
    Vector<String> fs_write {};
    bool storage { true };
    bool clipboard_write { false };
    bool system_info { true };
    bool allow_dynamic_services { false };
    AppNetworkPolicy network { AppNetworkPolicy::Unrestricted };
    bool cross_origin_isolated { false };
};

struct WEBVIEW_API AppManifest {
    String id;
    // Persistent authority is keyed by this source-bound identifier, never by the manifest-controlled display id.
    String storage_id;
    String name;
    String version { "0.1.0"_string };
    String entry { "index.html"_string };
    Optional<URL::URL> remote_url {};
    ByteString root_directory {};
    Optional<ByteString> icon_path {};
    Optional<u16> preferred_port {};
    AppWindowConfig window {};
    AppMCPConfig mcp {};
    AppPermissions permissions {};
    Vector<AppServiceConfig> services {};

    bool is_remote() const { return remote_url.has_value(); }
    Optional<AppServiceConfig const&> find_service(StringView service_id) const;
    static ErrorOr<AppServiceConfig> parse_service_config(StringView service_id, JsonObject const& cfg);
    static JsonObject serialize_service_config(AppServiceConfig const& cfg);
    static bool is_service_origin_allowed(AppServiceConfig const&, URL::URL const&);
    static bool is_service_origin_allowed(AppServiceConfig const&, StringView serialized_origin);
    AppModeOptions to_options(StringView account_id = "default"sv, StringView account_label = "Default"sv) const;

    static ErrorOr<AppManifest> load_from_file(ByteString const& manifest_path);
    static ErrorOr<AppManifest> load_from_directory(ByteString const& directory_path);
    static ErrorOr<AppManifest> from_remote_url(StringView raw_url);
    static ErrorOr<String> sanitize_app_id(StringView raw);
    static String profile_name_for_account(StringView app_id, StringView account_id);
    static bool is_valid_app_id(StringView id);
    static ErrorOr<ByteString> resolve_confined_subpath(ByteString const& root_directory, StringView relative_path, bool allow_missing_leaf = false);
};

}
