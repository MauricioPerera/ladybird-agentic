/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Hex.h>
#include <AK/JsonArray.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/File.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibFileSystem/FileSystem.h>
#include <LibURL/Parser.h>
#include <LibWebView/AppMode/AppManifest.h>

namespace WebView {

namespace {

constexpr int MIN_WINDOW_WIDTH = 200;
constexpr int MAX_WINDOW_WIDTH = 7680;
constexpr int MIN_WINDOW_HEIGHT = 200;
constexpr int MAX_WINDOW_HEIGHT = 4320;
constexpr int MIN_BOUND_WIDTH = 160;
constexpr int MIN_BOUND_HEIGHT = 160;

String source_bound_storage_id(StringView app_id, StringView source_kind, StringView canonical_source)
{
    auto source = ByteString::formatted("ladybird-app-storage:v1:{}:{}", source_kind, canonical_source);
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, source.bytes());
    auto app_id_prefix = app_id.substring_view(0, min(app_id.length(), 31uz));
    return MUST(String::formatted("{}-{}", app_id_prefix, encode_hex(digest.bytes().slice(0, 16))));
}

bool is_path_separator(char ch)
{
    return ch == '/' || ch == '\\';
}

bool paths_equal(StringView a, StringView b)
{
#if defined(AK_OS_WINDOWS)
    return a.equals_ignoring_ascii_case(b);
#else
    return a == b;
#endif
}

bool is_confined_path(StringView real_root, StringView real_candidate)
{
    while (real_root.length() > 1 && is_path_separator(real_root[real_root.length() - 1])) {
#if defined(AK_OS_WINDOWS)
        if (real_root.length() == 3 && real_root[1] == ':')
            break;
#endif
        real_root = real_root.substring_view(0, real_root.length() - 1);
    }

    if (paths_equal(real_candidate, real_root))
        return true;

    if (real_candidate.length() <= real_root.length())
        return false;

    if (!paths_equal(real_candidate.substring_view(0, real_root.length()), real_root))
        return false;

#if defined(AK_OS_WINDOWS)
    if (real_root.length() == 3 && real_root[1] == ':' && is_path_separator(real_root[2]))
        return true;
#endif
    if (real_root == "/"sv)
        return true;

    return is_path_separator(real_candidate[real_root.length()]);
}

bool is_valid_permission_subdir(StringView subdir)
{
    if (subdir.is_empty() || subdir.length() > 64)
        return false;
    for (auto ch : subdir) {
        if (!is_ascii_alphanumeric(ch) && ch != '-' && ch != '_')
            return false;
    }
    return true;
}

ErrorOr<Vector<String>> parse_permission_subdirs(JsonObject const& permissions_obj, StringView key)
{
    Vector<String> result;
    if (!permissions_obj.has(key))
        return result;

    auto array = permissions_obj.get_array(key);
    if (!array.has_value())
        return Error::from_string_literal("Manifest permissions subdirectory list must be an array");

    for (auto const& value : array->values()) {
        if (!value.is_string())
            return Error::from_string_literal("Manifest permission subdirectory entry must be a string");
        auto dir_name = value.as_string();
        if (!is_valid_permission_subdir(dir_name))
            return Error::from_string_literal("Manifest permission subdirectory name must contain only [a-zA-Z0-9_-]");
        if (!result.contains_slow(dir_name))
            result.append(move(dir_name));
    }
    return result;
}

ErrorOr<AppManifest> parse_local_manifest_object(JsonObject const& obj, ByteString const& root_directory)
{
    auto name = obj.get_string("name"sv);
    if (!name.has_value() || name->is_empty() || name->bytes_as_string_view().length() > 128)
        return Error::from_string_literal("Manifest requires a non-empty 'name' (max 128 chars)");

    String id;
    if (obj.has("id"sv)) {
        auto raw_id = obj.get_string("id"sv);
        if (!raw_id.has_value() || !AppManifest::is_valid_app_id(*raw_id))
            return Error::from_string_literal("Manifest 'id' must start with [a-z0-9] and contain only [a-z0-9_-] (1..64 chars)");
        id = *raw_id;
    } else {
        id = TRY(AppManifest::sanitize_app_id(*name));
    }

    String version = "0.1.0"_string;
    if (obj.has("version"sv)) {
        auto raw_version = obj.get_string("version"sv);
        if (!raw_version.has_value() || raw_version->is_empty())
            return Error::from_string_literal("Manifest 'version' must be a non-empty string");
        version = *raw_version;
    }

    String entry = "index.html"_string;
    if (obj.has("entry"sv)) {
        auto raw_entry = obj.get_string("entry"sv);
        if (!raw_entry.has_value() || raw_entry->is_empty())
            return Error::from_string_literal("Manifest 'entry' must be a non-empty relative path");
        entry = *raw_entry;
    }

    auto resolved_entry = TRY(AppManifest::resolve_confined_subpath(root_directory, entry));
    if (!FileSystem::is_regular_file(resolved_entry))
        return Error::from_string_literal("Manifest 'entry' does not point to an existing regular file");

    Optional<ByteString> icon_path;
    if (obj.has("icon"sv) && !obj.get("icon"sv)->is_null()) {
        auto raw_icon = obj.get_string("icon"sv);
        if (!raw_icon.has_value() || raw_icon->is_empty())
            return Error::from_string_literal("Manifest 'icon' must be a non-empty relative path");
        auto resolved_icon = TRY(AppManifest::resolve_confined_subpath(root_directory, *raw_icon));
        if (!FileSystem::is_regular_file(resolved_icon))
            return Error::from_string_literal("Manifest 'icon' does not point to an existing regular file");
        icon_path = move(resolved_icon);
    }

    AppWindowConfig window;
    if (obj.has("window"sv)) {
        auto win_obj = obj.get_object("window"sv);
        if (!win_obj.has_value())
            return Error::from_string_literal("Manifest 'window' must be an object");
        if (auto width = win_obj->get_i64("width"sv); width.has_value())
            window.width = static_cast<int>(clamp(*width, static_cast<i64>(MIN_WINDOW_WIDTH), static_cast<i64>(MAX_WINDOW_WIDTH)));
        if (auto height = win_obj->get_i64("height"sv); height.has_value())
            window.height = static_cast<int>(clamp(*height, static_cast<i64>(MIN_WINDOW_HEIGHT), static_cast<i64>(MAX_WINDOW_HEIGHT)));
        if (auto min_width = win_obj->get_i64("minWidth"sv); min_width.has_value())
            window.min_width = static_cast<int>(clamp(*min_width, static_cast<i64>(MIN_BOUND_WIDTH), static_cast<i64>(window.width)));
        else
            window.min_width = min(window.min_width, window.width);
        if (auto min_height = win_obj->get_i64("minHeight"sv); min_height.has_value())
            window.min_height = static_cast<int>(clamp(*min_height, static_cast<i64>(MIN_BOUND_HEIGHT), static_cast<i64>(window.height)));
        else
            window.min_height = min(window.min_height, window.height);
        if (auto resizable = win_obj->get_bool("resizable"sv); resizable.has_value())
            window.resizable = *resizable;
        if (auto show_tab_bar = win_obj->get_bool("showTabBar"sv); show_tab_bar.has_value())
            window.show_tab_bar = *show_tab_bar;
    }

    AppMCPConfig mcp;
    if (obj.has("mcp"sv)) {
        auto mcp_obj = obj.get_object("mcp"sv);
        if (!mcp_obj.has_value())
            return Error::from_string_literal("Manifest 'mcp' must be an object");
        if (auto enabled = mcp_obj->get_bool("enabled"sv); enabled.has_value())
            mcp.enabled = *enabled;
        if (mcp_obj->has("port"sv) && !mcp_obj->get("port"sv)->is_null()) {
            auto port = mcp_obj->get_u64("port"sv);
            if (!port.has_value() || *port > 65535)
                return Error::from_string_literal("Manifest 'mcp.port' must be an integer between 0 and 65535");
            mcp.port = static_cast<u16>(*port);
        }
        if (auto allow_webmcp = mcp_obj->get_bool("allowWebMCP"sv); allow_webmcp.has_value())
            mcp.allow_webmcp = *allow_webmcp;
        if (mcp_obj->has("access"sv) && !mcp_obj->get("access"sv)->is_null()) {
            auto access = mcp_obj->get_string("access"sv);
            if (!access.has_value())
                return Error::from_string_literal("Manifest 'mcp.access' must be a string ('full' or 'webmcp-only')");
            if (*access == "full"sv)
                mcp.access = AppMCPAccess::Full;
            else if (*access == "webmcp-only"sv)
                mcp.access = AppMCPAccess::WebMCPOnly;
            else
                return Error::from_string_literal("Manifest 'mcp.access' must be 'full' or 'webmcp-only'");
        }
        if (mcp.access == AppMCPAccess::WebMCPOnly && !mcp.allow_webmcp)
            return Error::from_string_literal("Manifest 'mcp.access' = 'webmcp-only' requires 'mcp.allowWebMCP' to be true");
        if (mcp_obj->has("allowedDomains"sv)) {
            auto domains = mcp_obj->get_array("allowedDomains"sv);
            if (!domains.has_value())
                return Error::from_string_literal("Manifest 'mcp.allowedDomains' must be an array");
            for (auto const& item : domains->values()) {
                if (!item.is_string() || item.as_string().is_empty())
                    return Error::from_string_literal("Manifest 'mcp.allowedDomains' entries must be non-empty strings");
                mcp.allowed_domains.append(item.as_string());
            }
        }
    }

    Optional<u16> preferred_port;
    auto parse_preferred_port = [&](JsonObject const& container, StringView key) -> ErrorOr<Optional<u16>> {
        if (!container.has(key) || container.get(key)->is_null())
            return Optional<u16> {};
        auto port = container.get_u64(key);
        if (!port.has_value() || *port < 1024 || *port > 65535)
            return Error::from_string_literal("Manifest storage port must be an integer between 1024 and 65535");
        return static_cast<u16>(*port);
    };
    if (obj.has("storage"sv)) {
        auto storage_obj = obj.get_object("storage"sv);
        if (!storage_obj.has_value())
            return Error::from_string_literal("Manifest 'storage' must be an object");
        preferred_port = TRY(parse_preferred_port(*storage_obj, "port"sv));
    } else if (obj.has("port"sv)) {
        preferred_port = TRY(parse_preferred_port(obj, "port"sv));
    }

    AppPermissions permissions;
    if (obj.has("permissions"sv)) {
        auto perm_obj = obj.get_object("permissions"sv);
        if (!perm_obj.has_value())
            return Error::from_string_literal("Manifest 'permissions' must be an object");
        permissions.fs_read = TRY(parse_permission_subdirs(*perm_obj, "fsRead"sv));
        permissions.fs_write = TRY(parse_permission_subdirs(*perm_obj, "fsWrite"sv));
        if (auto storage_perm = perm_obj->get_bool("storage"sv); storage_perm.has_value())
            permissions.storage = *storage_perm;
        if (auto clipboard_write = perm_obj->get_bool("clipboardWrite"sv); clipboard_write.has_value())
            permissions.clipboard_write = *clipboard_write;
        if (auto system_info = perm_obj->get_bool("systemInfo"sv); system_info.has_value())
            permissions.system_info = *system_info;
        if (perm_obj->has("dynamicServices"sv) && !perm_obj->get("dynamicServices"sv)->is_null()) {
            auto dynamic_services = perm_obj->get_bool("dynamicServices"sv);
            if (!dynamic_services.has_value())
                return Error::from_string_literal("Manifest 'permissions.dynamicServices' must be a boolean");
            permissions.allow_dynamic_services = *dynamic_services;
        }
        if (perm_obj->has("network"sv) && !perm_obj->get("network"sv)->is_null()) {
            auto network = perm_obj->get_string("network"sv);
            if (!network.has_value())
                return Error::from_string_literal("Manifest 'permissions.network' must be 'unrestricted' or 'offline'");
            if (*network == "unrestricted"sv)
                permissions.network = AppNetworkPolicy::Unrestricted;
            else if (*network == "offline"sv)
                permissions.network = AppNetworkPolicy::Offline;
            else
                return Error::from_string_literal("Manifest 'permissions.network' must be 'unrestricted' or 'offline'");
        }
        if (perm_obj->has("crossOriginIsolated"sv) && !perm_obj->get("crossOriginIsolated"sv)->is_null()) {
            auto coi = perm_obj->get_bool("crossOriginIsolated"sv);
            if (!coi.has_value())
                return Error::from_string_literal("Manifest 'permissions.crossOriginIsolated' must be a boolean");
            permissions.cross_origin_isolated = *coi;
        }
    }

    Vector<AppServiceConfig> services;
    if (obj.has("services"sv) && !obj.get("services"sv)->is_null()) {
        auto svc_obj = obj.get_object("services"sv);
        if (!svc_obj.has_value())
            return Error::from_string_literal("Manifest 'services' must be an object");

        ErrorOr<void> svc_err;
        svc_obj->for_each_member([&](String const& svc_id, JsonValue const& raw_cfg) {
            if (svc_err.is_error())
                return;
            if (!raw_cfg.is_object()) {
                svc_err = Error::from_string_literal("Manifest service entry must be an object");
                return;
            }
            auto parsed_svc = AppManifest::parse_service_config(svc_id, raw_cfg.as_object());
            if (parsed_svc.is_error()) {
                svc_err = parsed_svc.release_error();
                return;
            }
            services.append(parsed_svc.release_value());
        });
        TRY(svc_err);
    }

    auto storage_id = source_bound_storage_id(id, "local"sv, root_directory.view());

    return AppManifest {
        .id = move(id),
        .storage_id = move(storage_id),
        .name = *name,
        .version = move(version),
        .entry = move(entry),
        .remote_url = {},
        .root_directory = root_directory,
        .icon_path = move(icon_path),
        .preferred_port = preferred_port,
        .window = move(window),
        .mcp = move(mcp),
        .permissions = move(permissions),
        .services = move(services),
    };
}

}

ErrorOr<AppServiceConfig> AppManifest::parse_service_config(StringView service_id, JsonObject const& cfg)
{
    if (!is_valid_app_id(service_id))
        return Error::from_string_literal("Service key must start with [a-z0-9] and contain only [a-z0-9_-]");

    auto raw_default_url = cfg.get_string("defaultUrl"sv);
    if (!raw_default_url.has_value() || raw_default_url->is_empty())
        return Error::from_string_literal("Service requires non-empty 'defaultUrl'");
    auto parsed_default = URL::Parser::basic_parse(*raw_default_url);
    if (!parsed_default.has_value() || !parsed_default->scheme().is_one_of("http"sv, "https"sv) || !parsed_default->host().has_value())
        return Error::from_string_literal("Service 'defaultUrl' must be a valid http: or https: URL");

    String svc_id = TRY(String::from_utf8(service_id));
    String label = svc_id;
    if (cfg.has("label"sv) && !cfg.get("label"sv)->is_null()) {
        auto raw_label = cfg.get_string("label"sv);
        if (!raw_label.has_value() || raw_label->is_empty())
            return Error::from_string_literal("Service 'label' must be a non-empty string");
        label = *raw_label;
    }

    String container;
    if (cfg.has("container"sv) && !cfg.get("container"sv)->is_null()) {
        auto raw_container = cfg.get_string("container"sv);
        if (!raw_container.has_value())
            return Error::from_string_literal("Service 'container' must be a string");
        if (!raw_container->is_empty()) {
            if (!is_valid_app_id(*raw_container))
                return Error::from_string_literal("Service 'container' must be a valid container id");
            container = *raw_container;
        }
    }

    auto raw_origins = cfg.get_array("origins"sv);
    if (!raw_origins.has_value() || raw_origins->is_empty())
        return Error::from_string_literal("Service requires a non-empty 'origins' array");

    Vector<String> origins;
    for (auto const& origin_val : raw_origins->values()) {
        if (!origin_val.is_string() || origin_val.as_string().is_empty())
            return Error::from_string_literal("Service 'origins' entries must be non-empty URLs/origins");
        auto parsed_origin = URL::Parser::basic_parse(origin_val.as_string());
        if (!parsed_origin.has_value() || !parsed_origin->scheme().is_one_of("http"sv, "https"sv) || !parsed_origin->host().has_value())
            return Error::from_string_literal("Service 'origins' entry must be a valid http: or https: origin");
        auto ser = parsed_origin->origin().serialize();
        if (!origins.contains_slow(ser))
            origins.append(move(ser));
    }

    auto default_origin = parsed_default->origin().serialize();
    if (!origins.contains_slow(default_origin))
        return Error::from_string_literal("Service 'defaultUrl' origin must be listed in 'origins'");

    bool allow_navigate = true;
    bool allow_notifications = false;
    bool allow_read = false;
    bool allow_webmcp = false;
    bool allow_eval = false;
    bool allow_flows = false;
    bool allow_network = false;
    if (cfg.has("permissions"sv) && !cfg.get("permissions"sv)->is_null()) {
        auto perm_arr = cfg.get_array("permissions"sv);
        if (!perm_arr.has_value())
            return Error::from_string_literal("Service 'permissions' must be an array");
        allow_navigate = false;
        for (auto const& perm_val : perm_arr->values()) {
            if (!perm_val.is_string())
                return Error::from_string_literal("Service permissions entries must be strings");
            auto perm_str = perm_val.as_string();
            if (perm_str == "open"sv)
                continue;
            if (perm_str == "navigate"sv)
                allow_navigate = true;
            else if (perm_str == "notifications"sv)
                allow_notifications = true;
            else if (perm_str == "read"sv)
                allow_read = true;
            else if (perm_str == "webmcp"sv)
                allow_webmcp = true;
            else if (perm_str == "eval"sv)
                allow_eval = true;
            else if (perm_str == "flows"sv)
                allow_flows = true;
            else if (perm_str == "network"sv)
                allow_network = true;
            else
                return Error::from_string_literal("Service permission must be one of: open, navigate, notifications, read, webmcp, eval, flows, network");
        }
    }

    return AppServiceConfig {
        .id = move(svc_id),
        .label = move(label),
        .default_url = parsed_default.release_value(),
        .origins = move(origins),
        .container = move(container),
        .allow_navigate = allow_navigate,
        .allow_notifications = allow_notifications,
        .allow_read = allow_read,
        .allow_webmcp = allow_webmcp,
        .allow_eval = allow_eval,
        .allow_flows = allow_flows,
        .allow_network = allow_network,
    };
}

JsonObject AppManifest::serialize_service_config(AppServiceConfig const& cfg)
{
    JsonObject obj;
    obj.set("id"sv, cfg.id);
    obj.set("label"sv, cfg.label);
    obj.set("defaultUrl"sv, cfg.default_url.to_string());
    JsonArray origins;
    for (auto const& o : cfg.origins)
        origins.must_append(o);
    obj.set("origins"sv, move(origins));
    obj.set("container"sv, cfg.container);
    JsonArray perms;
    perms.must_append("open"sv);
    if (cfg.allow_navigate)
        perms.must_append("navigate"sv);
    if (cfg.allow_notifications)
        perms.must_append("notifications"sv);
    if (cfg.allow_read)
        perms.must_append("read"sv);
    if (cfg.allow_webmcp)
        perms.must_append("webmcp"sv);
    if (cfg.allow_eval)
        perms.must_append("eval"sv);
    if (cfg.allow_flows)
        perms.must_append("flows"sv);
    if (cfg.allow_network)
        perms.must_append("network"sv);
    obj.set("permissions"sv, move(perms));
    return obj;
}

Optional<AppServiceConfig const&> AppManifest::find_service(StringView service_id) const
{
    for (auto const& svc : services) {
        if (svc.id == service_id)
            return svc;
    }
    return {};
}

bool AppManifest::is_service_origin_allowed(AppServiceConfig const& service, URL::URL const& url)
{
    if (!url.scheme().is_one_of("http"sv, "https"sv) || !url.host().has_value())
        return false;
    return is_service_origin_allowed(service, url.origin().serialize());
}

bool AppManifest::is_service_origin_allowed(AppServiceConfig const& service, StringView serialized_origin)
{
    for (auto const& allowed : service.origins) {
        if (allowed == serialized_origin)
            return true;
    }
    return false;
}

AppModeOptions AppManifest::to_options(StringView account_id, StringView account_label) const
{
    auto resolved_account_id = account_id.is_empty() ? "default"_string : MUST(String::from_utf8(account_id));
    auto resolved_account_label = account_label.is_empty() ? "Default"_string : MUST(String::from_utf8(account_label));
    return {
        .app_id = id,
        .account_id = move(resolved_account_id),
        .account_label = move(resolved_account_label),
        .title = name,
        .icon_path = icon_path,
        .width = window.width,
        .height = window.height,
        .min_width = window.min_width,
        .min_height = window.min_height,
        .resizable = window.resizable,
        .show_tab_bar = window.show_tab_bar,
    };
}

String AppManifest::profile_name_for_account(StringView app_id, StringView account_id)
{
    if (account_id.is_empty() || account_id == "default"sv)
        return MUST(String::formatted("app-{}", app_id));
    return MUST(String::formatted("app-{}--{}", app_id, account_id));
}

bool AppManifest::is_valid_app_id(StringView id)
{
    if (id.is_empty() || id.length() > 64)
        return false;
    auto first = id[0];
    if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9')))
        return false;
    for (auto ch : id) {
        bool valid = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!valid)
            return false;
    }
    return true;
}

ErrorOr<String> AppManifest::sanitize_app_id(StringView raw)
{
    StringBuilder builder;
    bool prev_dash = false;
    for (auto ch : raw) {
        if (ch >= 'A' && ch <= 'Z')
            ch = static_cast<char>(ch + ('a' - 'A'));
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            builder.append(ch);
            prev_dash = false;
        } else if (ch == '-' || ch == '_' || ch == ' ') {
            if (!builder.is_empty() && !prev_dash) {
                builder.append('-');
                prev_dash = true;
            }
        } else {
            if (!builder.is_empty() && !prev_dash) {
                builder.append('-');
                prev_dash = true;
            }
        }
        if (builder.length() >= 64)
            break;
    }

    auto sanitized = builder.string_view().trim("-"sv, TrimMode::Both);
    if (!is_valid_app_id(sanitized))
        return Error::from_string_literal("Unable to derive valid app id from input");
    return String::from_utf8(sanitized);
}

ErrorOr<ByteString> AppManifest::resolve_confined_subpath(ByteString const& root_directory, StringView relative_path, bool allow_missing_leaf)
{
    if (relative_path.contains('\0') || relative_path.contains(':'))
        return Error::from_string_literal("Invalid path characters");
    if (relative_path.is_empty() || is_path_separator(relative_path[0]))
        return Error::from_string_literal("Path must be relative to the root directory");

    auto real_root = TRY(FileSystem::real_path(root_directory));
    if (!FileSystem::is_directory(real_root))
        return Error::from_string_literal("Root path is not a directory");

    Vector<StringView> segments;
    for (auto part : relative_path.split_view_if(is_path_separator, SplitBehavior::KeepEmpty)) {
        if (part.is_empty() || part == "."sv)
            continue;
        if (part == ".."sv)
            return Error::from_string_literal("Path traversal ('..') is not allowed");
        segments.append(part);
    }

    if (segments.is_empty())
        return Error::from_string_literal("Relative path resolves to root directory");

    ByteString current = real_root;
    for (size_t i = 0; i < segments.size(); ++i) {
        current = LexicalPath::join(current, segments[i]).string();
        bool is_leaf = (i + 1 == segments.size());

        if (allow_missing_leaf) {
            if (FileSystem::is_link(current))
                return Error::from_string_literal("Symbolic links are not allowed inside writable app data");
            if (FileSystem::exists(current)) {
                auto real_step = TRY(FileSystem::real_path(current));
                if (!is_confined_path(real_root, real_step))
                    return Error::from_string_literal("Path escapes root directory");
                if (!is_leaf && !FileSystem::is_directory(real_step))
                    return Error::from_string_literal("Parent path component is not a directory");
            }
        }
    }

    if (!allow_missing_leaf) {
        if (!FileSystem::exists(current))
            return Error::from_errno(ENOENT);
        auto real_target = TRY(FileSystem::real_path(current));
        if (!is_confined_path(real_root, real_target))
            return Error::from_string_literal("Path escapes root directory");
        return real_target;
    }

    return current;
}

ErrorOr<AppManifest> AppManifest::load_from_file(ByteString const& manifest_path)
{
    auto real_manifest = TRY(FileSystem::real_path(manifest_path));
    if (!FileSystem::is_regular_file(real_manifest))
        return Error::from_string_literal("App manifest path is not a regular file");

    auto root_directory = TRY(FileSystem::real_path(LexicalPath { real_manifest }.parent().string()));
    auto file = TRY(Core::File::open(real_manifest, Core::File::OpenMode::Read));
    auto content = TRY(file->read_until_eof());
    auto json = TRY(JsonValue::from_string(content));
    if (!json.is_object())
        return Error::from_string_literal("App manifest JSON must be an object");

    return parse_local_manifest_object(json.as_object(), root_directory);
}

ErrorOr<AppManifest> AppManifest::load_from_directory(ByteString const& directory_path)
{
    auto real_dir = TRY(FileSystem::real_path(directory_path));
    if (!FileSystem::is_directory(real_dir))
        return Error::from_string_literal("App directory does not exist");

    auto manifest_candidate = LexicalPath::join(real_dir, "ladybird-app.json"sv).string();
    if (FileSystem::is_regular_file(manifest_candidate))
        return load_from_file(manifest_candidate);

    auto index_candidate = LexicalPath::join(real_dir, "index.html"sv).string();
    auto resolved_index = TRY(resolve_confined_subpath(real_dir, "index.html"sv));
    if (!FileSystem::is_regular_file(resolved_index))
        return Error::from_string_literal("Directory contains neither ladybird-app.json nor index.html");

    auto basename = LexicalPath::basename(real_dir);
    auto id = sanitize_app_id(basename).value_or("local-app"_string);
    auto name = TRY(String::from_utf8(basename.is_empty() ? "Ladybird App"sv : basename.view()));
    auto storage_id = source_bound_storage_id(id, "local"sv, real_dir.view());

    return AppManifest {
        .id = move(id),
        .storage_id = move(storage_id),
        .name = move(name),
        .version = "0.1.0"_string,
        .entry = "index.html"_string,
        .remote_url = {},
        .root_directory = move(real_dir),
        .icon_path = {},
        .window = {},
        .mcp = { .enabled = true, .port = {}, .allow_webmcp = true, .allowed_domains = {} },
        .permissions = { .fs_read = {}, .fs_write = {}, .storage = true, .clipboard_write = false, .system_info = false },
    };
}

ErrorOr<AppManifest> AppManifest::from_remote_url(StringView raw_url)
{
    auto parsed = URL::Parser::basic_parse(raw_url);
    if (!parsed.has_value() || !parsed->scheme().is_one_of("http"sv, "https"sv) || !parsed->host().has_value())
        return Error::from_string_literal("Remote --app URL must be a valid http: or https: URL with a host");

    auto host = parsed->serialized_host();
    auto id = sanitize_app_id(host).value_or("remote-app"_string);
    auto storage_id = source_bound_storage_id(id, "remote"sv, parsed->origin().serialize());

    return AppManifest {
        .id = move(id),
        .storage_id = move(storage_id),
        .name = TRY(String::from_utf8(host)),
        .version = "0.1.0"_string,
        .entry = ""_string,
        .remote_url = parsed.release_value(),
        .root_directory = {},
        .icon_path = {},
        .window = {},
        .mcp = { .enabled = true, .port = {}, .allow_webmcp = true, .allowed_domains = {} },
        .permissions = { .fs_read = {}, .fs_write = {}, .storage = false, .clipboard_write = false, .system_info = false },
    };
}

}
