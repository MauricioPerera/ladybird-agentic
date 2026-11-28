/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <AK/CharacterTypes.h>
#include <AK/Hex.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/StringBuilder.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Socket.h>
#include <LibCore/TCPServer.h>
#include <LibCore/Timer.h>
#include <LibCore/Version.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/HttpRequest.h>
#include <LibHTTP/Status.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebView/AppMode/AppAccountRegistry.h>
#include <LibWebView/AppMode/AppModeServer.h>
#include <LibWebView/Application.h>
#include <time.h>

namespace WebView {

static constexpr size_t MAX_APP_REQUEST_BYTES = 8 * 1024 * 1024;
static constexpr size_t MAX_APP_IPC_FILE_BYTES = 4 * 1024 * 1024;
static constexpr size_t MAX_APP_BINARY_FILE_BYTES = 128 * 1024 * 1024;
static constexpr size_t MAX_ACTIVE_APP_CONNECTIONS = 64;
static constexpr size_t MAX_APP_SSE_CLIENTS = 16;
static constexpr size_t MAX_SERVICE_NOTIFICATIONS = 200;
static constexpr size_t MAX_SERVICE_NETWORK_EVENTS = 200;
static constexpr size_t MAX_SERVICE_EVENTS = 250;
static constexpr size_t MAX_SERVICE_TRIGGERS = 100;
static constexpr size_t MAX_DYNAMIC_SERVICES = 64;
static constexpr size_t MAX_SERVICE_EVAL_BYTES = 64 * 1024;
static constexpr size_t MAX_WEBHOOK_BODY_BYTES = 64 * 1024;

static bool match_cron_field(StringView expr_part, int min_val, int max_val, Optional<int> target_val, bool wrap_seven_to_zero = false)
{
    if (expr_part.is_empty())
        return false;

    bool any_part = false;
    bool matched = false;
    bool valid = true;

    expr_part.for_each_split_view(',', SplitBehavior::KeepEmpty, [&](StringView item) {
        if (!valid)
            return;
        any_part = true;
        if (item.is_empty()) {
            valid = false;
            return;
        }

        StringView range_part = item;
        int step = 1;
        if (auto slash = item.find('/'); slash.has_value()) {
            range_part = item.substring_view(0, *slash);
            auto step_str = item.substring_view(*slash + 1);
            if (item.substring_view(*slash + 1).contains('/')) {
                valid = false;
                return;
            }
            auto parsed_step = step_str.to_number<int>();
            if (!parsed_step.has_value() || *parsed_step <= 0) {
                valid = false;
                return;
            }
            step = *parsed_step;
        }

        int start = 0;
        int end = 0;
        if (range_part == "*"sv) {
            start = min_val;
            end = max_val;
        } else if (auto dash = range_part.find('-'); dash.has_value()) {
            auto lhs = range_part.substring_view(0, *dash).to_number<int>();
            auto rhs = range_part.substring_view(*dash + 1).to_number<int>();
            if (!lhs.has_value() || !rhs.has_value() || *lhs < min_val || *rhs > max_val || *lhs > *rhs) {
                valid = false;
                return;
            }
            start = *lhs;
            end = *rhs;
        } else {
            auto single = range_part.to_number<int>();
            if (!single.has_value() || *single < min_val || *single > max_val) {
                valid = false;
                return;
            }
            start = *single;
            end = item.find('/').has_value() ? max_val : *single;
        }

        if (target_val.has_value()) {
            for (int v = start; v <= end; v += step) {
                int normalized = (wrap_seven_to_zero && v == 7) ? 0 : v;
                if (normalized == *target_val) {
                    matched = true;
                    break;
                }
            }
        }
    });

    if (!valid || !any_part)
        return false;
    return target_val.has_value() ? matched : true;
}

static bool split_cron_expression(StringView expr, Vector<StringView, 5>& out_fields)
{
    out_fields.clear();
    expr.trim_whitespace().for_each_split_view(' ', SplitBehavior::Nothing, [&](StringView part) {
        for (auto sub : part.split_view('\t', SplitBehavior::Nothing))
            out_fields.append(sub);
    });
    return out_fields.size() == 5;
}

static JsonObject service_notification_to_json(AppServiceNotification const& n)
{
    JsonObject obj;
    obj.set("seq"sv, n.seq);
    obj.set("timestampMs"sv, n.timestamp_ms);
    obj.set("serviceId"sv, n.service_id);
    if (n.tab_id.has_value())
        obj.set("tabId"sv, *n.tab_id);
    else
        obj.set("tabId"sv, JsonValue {});
    obj.set("origin"sv, n.origin);
    obj.set("eventType"sv, n.event_type);
    obj.set("title"sv, n.title);
    obj.set("body"sv, n.body);
    obj.set("tag"sv, n.tag);
    return obj;
}

static JsonObject service_network_event_to_json(AppServiceNetworkEvent const& ev)
{
    JsonObject obj;
    obj.set("seq"sv, ev.seq);
    obj.set("timestampMs"sv, ev.timestamp_ms);
    obj.set("serviceId"sv, ev.service_id);
    obj.set("tabId"sv, ev.tab_id);
    obj.set("requestId"sv, ev.request_id);
    obj.set("url"sv, ev.url);
    obj.set("method"sv, ev.method);
    if (ev.initiator_type.has_value())
        obj.set("initiatorType"sv, *ev.initiator_type);
    else
        obj.set("initiatorType"sv, JsonValue {});
    if (ev.status_code.has_value())
        obj.set("statusCode"sv, *ev.status_code);
    else
        obj.set("statusCode"sv, JsonValue {});
    obj.set("bodySize"sv, ev.body_size);
    if (ev.error.has_value())
        obj.set("error"sv, *ev.error);
    else
        obj.set("error"sv, JsonValue {});
    return obj;
}

static JsonObject service_event_to_json(AppServiceEvent const& ev)
{
    JsonObject obj;
    obj.set("seq"sv, ev.seq);
    obj.set("timestampMs"sv, ev.timestamp_ms);
    obj.set("type"sv, ev.type);
    obj.set("serviceId"sv, ev.service_id);
    if (ev.tab_id.has_value())
        obj.set("tabId"sv, *ev.tab_id);
    else
        obj.set("tabId"sv, JsonValue {});
    obj.set("data"sv, ev.data);
    return obj;
}

static JsonObject service_tab_state_to_json(AppServiceConfig const& cfg, Optional<AppServiceTabState const&> live, bool is_dynamic = false)
{
    JsonObject obj;
    obj.set("id"sv, cfg.id);
    obj.set("label"sv, cfg.label);
    obj.set("defaultUrl"sv, cfg.default_url.to_string());
    JsonArray origins;
    for (auto const& o : cfg.origins)
        origins.must_append(o);
    obj.set("origins"sv, move(origins));

    JsonObject perms;
    perms.set("open"sv, true);
    perms.set("navigate"sv, cfg.allow_navigate);
    perms.set("notifications"sv, cfg.allow_notifications);
    perms.set("read"sv, cfg.allow_read);
    perms.set("webmcp"sv, cfg.allow_webmcp);
    perms.set("eval"sv, cfg.allow_eval);
    perms.set("flows"sv, cfg.allow_flows);
    perms.set("network"sv, cfg.allow_network);
    obj.set("permissions"sv, move(perms));

    obj.set("dynamic"sv, is_dynamic);
    obj.set("configuredContainer"sv, cfg.container);
    obj.set("open"sv, live.has_value() && live->open);
    if (live.has_value() && live->open) {
        obj.set("tabId"sv, live->tab_id);
        obj.set("url"sv, live->url);
        obj.set("title"sv, live->title);
        obj.set("container"sv, live->container);
        obj.set("originAllowed"sv, live->origin_allowed);
    } else {
        obj.set("tabId"sv, JsonValue {});
        obj.set("url"sv, JsonValue {});
        obj.set("title"sv, JsonValue {});
        obj.set("container"sv, cfg.container);
        obj.set("originAllowed"sv, false);
    }
    return obj;
}

static JsonObject trigger_to_json(AppTriggerConfig const& trigger)
{
    JsonObject obj;
    obj.set("id"sv, trigger.id);
    obj.set("name"sv, trigger.name);
    obj.set("enabled"sv, trigger.enabled);
    obj.set("type"sv, trigger.type);
    obj.set("serviceId"sv, trigger.service_id);
    obj.set("flowId"sv, trigger.flow_id);
    obj.set("intervalMs"sv, trigger.interval_ms);
    obj.set("cooldownMs"sv, trigger.cooldown_ms);
    if (trigger.url_contains.has_value())
        obj.set("urlContains"sv, *trigger.url_contains);
    if (trigger.method.has_value())
        obj.set("method"sv, *trigger.method);
    if (trigger.status_code.has_value())
        obj.set("statusCode"sv, *trigger.status_code);
    if (trigger.title_contains.has_value())
        obj.set("titleContains"sv, *trigger.title_contains);
    if (trigger.body_contains.has_value())
        obj.set("bodyContains"sv, *trigger.body_contains);
    obj.set("openIfClosed"sv, trigger.open_if_closed);
    obj.set("activate"sv, trigger.activate);
    obj.set("closeOnFinish"sv, trigger.close_on_finish);
    obj.set("clientHandled"sv, trigger.client_handled);
    if (trigger.webhook_secret.has_value() && !trigger.webhook_secret->is_empty())
        obj.set("webhookSecret"sv, *trigger.webhook_secret);
    if (trigger.cron_expr.has_value() && !trigger.cron_expr->is_empty())
        obj.set("cronExpr"sv, *trigger.cron_expr);

    JsonObject vars;
    for (auto const& [k, v] : trigger.variables)
        vars.set(k, v);
    obj.set("variables"sv, move(vars));
    obj.set("lastFiredAtMs"sv, trigger.last_fired_at_ms);
    if (trigger.last_run_id.has_value())
        obj.set("lastRunId"sv, *trigger.last_run_id);
    else
        obj.set("lastRunId"sv, JsonValue {});
    obj.set("fireCount"sv, trigger.fire_count);
    return obj;
}

static ErrorOr<AppTriggerConfig> parse_trigger_config(JsonObject const& obj)
{
    auto id = obj.get_string("id"sv);
    if (!id.has_value() || !AppManifest::is_valid_app_id(*id))
        return Error::from_string_literal("Trigger 'id' must start with [a-z0-9] and contain only [a-z0-9_-]");

    auto type = obj.get_string("type"sv);
    if (!type.has_value() || !type->bytes_as_string_view().is_one_of("interval"sv, "notification"sv, "network"sv, "navigation"sv, "webhook"sv, "cron"sv))
        return Error::from_string_literal("Trigger 'type' must be one of: interval, notification, network, navigation, webhook, cron");

    auto service_id = obj.get_string("serviceId"sv);
    if (!service_id.has_value() || service_id->is_empty())
        return Error::from_string_literal("Trigger requires non-empty 'serviceId'");

    auto flow_id = obj.get_string("flowId"sv);
    if (!flow_id.has_value() || !is_valid_flow_id(*flow_id))
        return Error::from_string_literal("Trigger requires valid 'flowId'");

    String name = *id;
    if (auto raw_name = obj.get_string("name"sv); raw_name.has_value() && !raw_name->is_empty())
        name = *raw_name;

    Optional<String> cron_expr;
    if (auto raw_cron = obj.get_string("cronExpr"sv); raw_cron.has_value()) {
        auto trimmed_view = raw_cron->bytes_as_string_view().trim_whitespace();
        if (!trimmed_view.is_empty()) {
            if (!AppModeServer::is_valid_cron_expression(trimmed_view))
                return Error::from_string_literal("Trigger 'cronExpr' must be a valid 5-field cron expression");
            cron_expr = TRY(String::from_utf8(trimmed_view));
        }
    }
    if (*type == "cron"sv && !cron_expr.has_value())
        return Error::from_string_literal("Cron trigger requires a valid 5-field 'cronExpr'");

    u64 interval_ms = obj.get_u64("intervalMs"sv).value_or(60000);
    if (*type == "interval"sv && (interval_ms < 5000 || interval_ms > 86400000ULL))
        return Error::from_string_literal("Interval trigger 'intervalMs' must be between 5000 and 86400000");
    if (interval_ms < 5000)
        interval_ms = 5000;
    if (interval_ms > 86400000ULL)
        interval_ms = 86400000ULL;

    u64 cooldown_ms = obj.get_u64("cooldownMs"sv).value_or(3000);
    if (cooldown_ms < 500 || cooldown_ms > 3600000ULL)
        return Error::from_string_literal("Trigger 'cooldownMs' must be between 500 and 3600000");

    AppTriggerConfig cfg;
    cfg.id = *id;
    cfg.name = move(name);
    cfg.enabled = obj.get_bool("enabled"sv).value_or(true);
    cfg.type = *type;
    cfg.service_id = *service_id;
    cfg.flow_id = *flow_id;
    cfg.interval_ms = static_cast<u32>(interval_ms);
    cfg.cooldown_ms = static_cast<u32>(cooldown_ms);
    if (auto v = obj.get_string("urlContains"sv); v.has_value() && !v->is_empty())
        cfg.url_contains = *v;
    if (auto v = obj.get_string("method"sv); v.has_value() && !v->is_empty())
        cfg.method = *v;
    if (auto v = obj.get_u64("statusCode"sv); v.has_value() && *v >= 100 && *v <= 599)
        cfg.status_code = static_cast<u32>(*v);
    if (auto v = obj.get_string("titleContains"sv); v.has_value() && !v->is_empty())
        cfg.title_contains = *v;
    if (auto v = obj.get_string("bodyContains"sv); v.has_value() && !v->is_empty())
        cfg.body_contains = *v;
    cfg.open_if_closed = obj.get_bool("openIfClosed"sv).value_or(false);
    cfg.activate = obj.get_bool("activate"sv).value_or(false);
    cfg.close_on_finish = obj.get_bool("closeOnFinish"sv).value_or(false);
    cfg.client_handled = obj.get_bool("clientHandled"sv).value_or(false);
    if (auto v = obj.get_string("webhookSecret"sv); v.has_value() && !v->is_empty())
        cfg.webhook_secret = *v;
    cfg.cron_expr = move(cron_expr);

    if (obj.has("variables"sv) && !obj.get("variables"sv)->is_null()) {
        auto vars_obj = obj.get_object("variables"sv);
        if (!vars_obj.has_value())
            return Error::from_string_literal("Trigger 'variables' must be an object");
        if (vars_obj->size() > 32)
            return Error::from_string_literal("Trigger 'variables' cannot exceed 32 entries");
        vars_obj->for_each_member([&](String const& key, JsonValue const& val) {
            if (val.is_string())
                cfg.variables.set(key, val.as_string());
            else if (val.is_number() || val.is_bool())
                cfg.variables.set(key, val.serialized());
        });
    }
    if (auto v = obj.get_i64("lastFiredAtMs"sv); v.has_value() && *v > 0)
        cfg.last_fired_at_ms = *v;
    if (auto v = obj.get_string("lastRunId"sv); v.has_value() && !v->is_empty())
        cfg.last_run_id = *v;
    cfg.fire_count = obj.get_u64("fireCount"sv).value_or(0);
    return cfg;
}

static StringView mcp_access_mode_name(AppMCPAccess access)
{
    switch (access) {
    case AppMCPAccess::Full:
        return "full"sv;
    case AppMCPAccess::WebMCPOnly:
        return "webmcp-only"sv;
    }
    VERIFY_NOT_REACHED();
}

static StringView network_policy_name(AppNetworkPolicy policy)
{
    switch (policy) {
    case AppNetworkPolicy::Unrestricted:
        return "unrestricted"sv;
    case AppNetworkPolicy::Offline:
        return "offline"sv;
    }
    VERIFY_NOT_REACHED();
}

static bool is_ascii_digits(StringView view)
{
    if (view.is_empty())
        return false;
    for (auto ch : view) {
        if (!is_ascii_digit(ch))
            return false;
    }
    return true;
}

static bool is_allowed_loopback_host(StringView host)
{
    host = host.trim_whitespace();
    if (host.starts_with('[')) {
        auto close = host.find(']');
        if (!close.has_value())
            return false;
        auto inner = host.substring_view(1, *close - 1);
        auto rest = host.substring_view(*close + 1);
        if (!rest.is_empty()) {
            if (!rest.starts_with(':'))
                return false;
            auto port_part = rest.substring_view(1);
            auto port = port_part.to_number<u32>();
            if (!is_ascii_digits(port_part) || !port.has_value() || *port < 1 || *port > 65535)
                return false;
        }
        return inner == "::1"sv;
    }
    if (auto colon = host.find_last(':'); colon.has_value()) {
        auto port_part = host.substring_view(*colon + 1);
        auto port = port_part.to_number<u32>();
        if (!is_ascii_digits(port_part) || !port.has_value() || *port < 1 || *port > 65535)
            return false;
        host = host.substring_view(0, *colon);
    }
    return host.equals_ignoring_ascii_case("localhost"sv) || host == "127.0.0.1"sv;
}

static Optional<StringView> find_header(HTTP::HttpRequest const& request, StringView name)
{
    if (auto it = request.headers().headers().find_if([&](auto const& header) { return header.name.equals_ignoring_ascii_case(name); }); !it.is_end())
        return it->value.view();
    return {};
}

static size_t count_headers(HTTP::HttpRequest const& request, StringView name)
{
    size_t count = 0;
    for (auto const& header : request.headers().headers()) {
        if (header.name.equals_ignoring_ascii_case(name))
            ++count;
    }
    return count;
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

static StringView mime_type_for_path(StringView path)
{
    auto ext = LexicalPath::extension(path);
    if (ext.equals_ignoring_ascii_case("html"sv) || ext.equals_ignoring_ascii_case("htm"sv))
        return "text/html; charset=utf-8"sv;
    if (ext.equals_ignoring_ascii_case("js"sv) || ext.equals_ignoring_ascii_case("mjs"sv))
        return "application/javascript; charset=utf-8"sv;
    if (ext.equals_ignoring_ascii_case("css"sv))
        return "text/css; charset=utf-8"sv;
    if (ext.equals_ignoring_ascii_case("json"sv))
        return "application/json; charset=utf-8"sv;
    if (ext.equals_ignoring_ascii_case("svg"sv))
        return "image/svg+xml"sv;
    if (ext.equals_ignoring_ascii_case("png"sv))
        return "image/png"sv;
    if (ext.equals_ignoring_ascii_case("jpg"sv) || ext.equals_ignoring_ascii_case("jpeg"sv))
        return "image/jpeg"sv;
    if (ext.equals_ignoring_ascii_case("webp"sv))
        return "image/webp"sv;
    if (ext.equals_ignoring_ascii_case("ico"sv))
        return "image/x-icon"sv;
    if (ext.equals_ignoring_ascii_case("wasm"sv))
        return "application/wasm"sv;
    if (ext.equals_ignoring_ascii_case("woff2"sv))
        return "font/woff2"sv;
    if (ext.equals_ignoring_ascii_case("txt"sv))
        return "text/plain; charset=utf-8"sv;
    if (ext.equals_ignoring_ascii_case("md"sv))
        return "text/markdown; charset=utf-8"sv;
    if (ext.equals_ignoring_ascii_case("wav"sv))
        return "audio/wav"sv;
    if (ext.equals_ignoring_ascii_case("mp3"sv))
        return "audio/mpeg"sv;
    if (ext.equals_ignoring_ascii_case("ogg"sv))
        return "audio/ogg"sv;
    if (ext.equals_ignoring_ascii_case("flac"sv))
        return "audio/flac"sv;
    if (ext.equals_ignoring_ascii_case("m4a"sv) || ext.equals_ignoring_ascii_case("aac"sv))
        return "audio/mp4"sv;
    if (ext.equals_ignoring_ascii_case("webm"sv))
        return "audio/webm"sv;
    if (ext.equals_ignoring_ascii_case("mp4"sv))
        return "video/mp4"sv;
    if (ext.equals_ignoring_ascii_case("webmanifest"sv))
        return "application/manifest+json"sv;
    if (ext.equals_ignoring_ascii_case("asesx"sv))
        return "application/json; charset=utf-8"sv;
    return "application/octet-stream"sv;
}

struct ByteRangeSpec {
    u64 start { 0 };
    u64 end { 0 };
};

static Optional<ByteRangeSpec> parse_byte_range(StringView header_value, u64 total_size)
{
    if (total_size == 0)
        return {};
    auto trimmed = header_value.trim_whitespace();
    if (trimmed.length() < 7 || !trimmed.substring_view(0, 6).equals_ignoring_ascii_case("bytes="sv))
        return {};
    auto spec = trimmed.substring_view(6).trim_whitespace();
    if (spec.is_empty() || spec.contains(','))
        return {};
    auto dash = spec.find('-');
    if (!dash.has_value())
        return {};
    auto left = spec.substring_view(0, *dash).trim_whitespace();
    auto right = spec.substring_view(*dash + 1).trim_whitespace();
    if (left.is_empty() && right.is_empty())
        return {};
    if (left.is_empty()) {
        auto suffix = right.to_number<u64>();
        if (!is_ascii_digits(right) || !suffix.has_value() || *suffix == 0)
            return {};
        u64 start = total_size > *suffix ? total_size - *suffix : 0;
        return ByteRangeSpec { start, total_size - 1 };
    }
    auto start = left.to_number<u64>();
    if (!is_ascii_digits(left) || !start.has_value() || *start >= total_size)
        return {};
    if (right.is_empty())
        return ByteRangeSpec { *start, total_size - 1 };
    auto end = right.to_number<u64>();
    if (!is_ascii_digits(right) || !end.has_value() || *end < *start)
        return {};
    return ByteRangeSpec { *start, min(*end, total_size - 1) };
}

static Optional<String> extract_query_param(StringView query, StringView key)
{
    if (query.starts_with('?'))
        query = query.substring_view(1);
    if (auto h = query.find('#'); h.has_value())
        query = query.substring_view(0, *h);
    for (auto part : query.split_view('&')) {
        auto eq = part.find('=');
        auto k = eq.has_value() ? part.substring_view(0, *eq) : part;
        if (k != key)
            continue;
        auto raw_val = eq.has_value() ? part.substring_view(*eq + 1) : StringView {};
        auto decoded = URL::percent_decode(raw_val);
        if (auto str = String::from_utf8(decoded.bytes()); !str.is_error())
            return str.release_value();
        return {};
    }
    return {};
}

static StringView platform_name()
{
#if defined(AK_OS_WINDOWS)
    return "windows"sv;
#elif defined(AK_OS_MACOS)
    return "macos"sv;
#elif defined(AK_OS_LINUX)
    return "linux"sv;
#else
    return "unknown"sv;
#endif
}

static ErrorOr<ByteString> resolve_permitted_data_path(ByteString const& data_directory, Vector<String> const& allowed_subdirs, StringView raw_path, bool allow_missing_leaf)
{
    if (allowed_subdirs.is_empty())
        return Error::from_errno(EACCES);

    if (raw_path.contains('\0') || raw_path.contains('\\') || raw_path.contains(':'))
        return Error::from_errno(EACCES);

    Vector<StringView> clean_segments;
    for (auto segment : raw_path.split_view('/', SplitBehavior::KeepEmpty)) {
        if (segment.is_empty() || segment == "."sv)
            continue;
        if (segment == ".."sv)
            return Error::from_errno(EACCES);
        clean_segments.append(segment);
    }

    if (clean_segments.is_empty())
        return Error::from_errno(EACCES);

    auto top_dir = clean_segments.first();
    bool permitted = false;
    for (auto const& allowed : allowed_subdirs) {
        if (allowed == top_dir) {
            permitted = true;
            break;
        }
    }
    if (!permitted)
        return Error::from_errno(EACCES);

    StringBuilder normalized;
    for (size_t i = 0; i < clean_segments.size(); ++i) {
        if (i > 0)
            normalized.append('/');
        normalized.append(clean_segments[i]);
    }

    auto resolved_or_error = AppManifest::resolve_confined_subpath(data_directory, normalized.string_view(), allow_missing_leaf);
    if (resolved_or_error.is_error())
        return resolved_or_error.release_error();
    return resolved_or_error.release_value();
}

class AppModeServer::Connection final
    : public RefCounted<Connection>
    , public Weakable<Connection> {
public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullRefPtr<Connection>> create(AppModeServer& server, NonnullOwnPtr<Core::TCPSocket> socket)
    {
        auto buffered_socket = TRY(Core::BufferedTCPSocket::create(move(socket)));
        TRY(buffered_socket->set_blocking(false));
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
        if (m_sse_keepalive_timer) {
            m_sse_keepalive_timer->stop();
            m_sse_keepalive_timer.clear();
        }
        m_socket->on_ready_to_read = {};
        m_socket->close();
    }

    bool is_sse_stream() const { return m_is_sse_stream; }

    void send_sse_event(StringView event_name, StringView data)
    {
        if (m_closed || !m_is_sse_stream)
            return;
        if (event_name.contains('\r') || event_name.contains('\n'))
            return;

        StringBuilder builder;
        builder.appendff("event: {}\n", event_name);
        auto normalized_data = data.replace("\r\n"sv, "\n"sv, ReplaceMode::All).replace("\r"sv, "\n"sv, ReplaceMode::All);
        normalized_data.view().for_each_split_view('\n', SplitBehavior::KeepEmpty, [&](StringView line) {
            builder.appendff("data: {}\n", line);
        });
        builder.append('\n');

        if (auto result = m_socket->write_until_depleted(builder.string_view().bytes()); result.is_error()) {
            close_later();
        }
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

private:
    Connection(AppModeServer& server, NonnullOwnPtr<Core::BufferedTCPSocket> socket)
        : m_server(server.make_weak_ptr())
        , m_socket(move(socket))
    {
        m_read_timeout_timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(30), [weak_self = make_weak_ptr()] {
            if (!weak_self || weak_self->m_closed || weak_self->m_request_handled)
                return;
            weak_self->send_ipc_error(408, "Request Timeout"sv);
        });
        m_read_timeout_timer->start();

        m_socket->on_ready_to_read = [this] {
            if (auto result = on_ready_to_read(); result.is_error()) {
                if (m_is_sse_stream || m_request_handled) {
                    close_later();
                    return;
                }
                warnln("AppMode connection error: {}", result.error());
                send_ipc_error(500, "Internal Server Error"sv);
            }
        };
    }

    ErrorOr<void> on_ready_to_read()
    {
        if (m_closed)
            return {};

        if (m_is_sse_stream) {
            auto buffer = TRY(ByteBuffer::create_uninitialized(256));
            if (TRY(m_socket->can_read_without_blocking())) {
                auto data = TRY(m_socket->read_some(buffer));
                if (data.is_empty() || !data.is_empty() || m_socket->is_eof()) {
                    close_later();
                    return {};
                }
            }
            if (m_socket->is_eof())
                close_later();
            return {};
        }

        if (m_request_handled)
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
            TRY(m_remaining_request.try_append(StringView { data }));
            bool is_binary_put = m_remaining_request.string_view().starts_with("PUT /__ladybird_fs/"sv);
            size_t limit = is_binary_put ? (MAX_APP_BINARY_FILE_BYTES + 65536) : MAX_APP_REQUEST_BYTES;
            if (m_remaining_request.length() > limit) {
                send_ipc_error(413, "Request exceeded maximum allowed size"sv);
                return {};
            }

            if (m_socket->is_eof())
                break;
        }

        if (m_remaining_request.is_empty()) {
            if (m_socket->is_eof())
                close_later();
            return {};
        }

        if (m_remaining_request.string_view().starts_with("PUT /__ladybird_fs/"sv)) {
            if (auto handled = try_handle_binary_fs_put(); handled)
                return {};
            if (m_socket->is_eof())
                send_ipc_error(400, "Incomplete binary upload request"sv);
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
                send_ipc_error(413, "Request too large"sv);
                return {};
            case HTTP::HttpRequest::ParseError::UnsupportedMethod:
                send_ipc_error(405, "Method Not Allowed"sv);
                return {};
            case HTTP::HttpRequest::ParseError::InvalidURL:
            case HTTP::HttpRequest::ParseError::OutOfMemory:
                send_ipc_error(400, HTTP::HttpRequest::parse_error_to_string(parsed_request.error()));
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

    bool try_handle_binary_fs_put()
    {
        auto raw = m_remaining_request.string_view();
        auto header_end = raw.find("\r\n\r\n"sv);
        if (!header_end.has_value()) {
            if (raw.length() > 65536) {
                m_request_handled = true;
                send_ipc_error(431, "Request headers too large"sv);
                return true;
            }
            return false;
        }

        auto header_block = raw.substring_view(0, *header_end);
        auto first_line_end = header_block.find("\r\n"sv);
        if (!first_line_end.has_value()) {
            m_request_handled = true;
            send_ipc_error(400, "Malformed HTTP request line"sv);
            return true;
        }

        auto request_line = header_block.substring_view(0, *first_line_end);
        auto parts = request_line.split_view(' ');
        if (parts.size() < 3 || parts[0] != "PUT"sv) {
            m_request_handled = true;
            send_ipc_error(400, "Malformed PUT request line"sv);
            return true;
        }
        auto resource = parts[1];

        size_t host_count = 0;
        Optional<StringView> host_hdr;
        Optional<StringView> origin_hdr;
        Optional<StringView> token_hdr;
        Optional<u64> content_length;
        bool has_transfer_encoding = false;

        auto rest_headers = header_block.substring_view(*first_line_end + 2);
        for (auto line : rest_headers.split_view("\r\n"sv)) {
            if (line.is_empty())
                continue;
            auto colon = line.find(':');
            if (!colon.has_value())
                continue;
            auto name = line.substring_view(0, *colon).trim_whitespace();
            auto value = line.substring_view(*colon + 1).trim_whitespace();
            if (name.equals_ignoring_ascii_case("Host"sv)) {
                ++host_count;
                host_hdr = value;
            } else if (name.equals_ignoring_ascii_case("Origin"sv)) {
                origin_hdr = value;
            } else if (name.equals_ignoring_ascii_case("X-Ladybird-App-Token"sv)) {
                token_hdr = value;
            } else if (name.equals_ignoring_ascii_case("Transfer-Encoding"sv)) {
                has_transfer_encoding = true;
            } else if (name.equals_ignoring_ascii_case("Content-Length"sv)) {
                auto parsed_len = value.to_number<u64>();
                if (!is_ascii_digits(value) || !parsed_len.has_value()) {
                    m_request_handled = true;
                    send_ipc_error(400, "Invalid Content-Length header"sv);
                    return true;
                }
                content_length = parsed_len;
            }
        }

        if (has_transfer_encoding) {
            m_request_handled = true;
            send_ipc_error(501, "Chunked Transfer-Encoding is not supported for binary FS uploads"sv);
            return true;
        }
        if (host_count != 1 || !host_hdr.has_value() || !is_allowed_loopback_host(*host_hdr)) {
            m_request_handled = true;
            send_ipc_error(403, "Forbidden: Invalid or non-loopback Host header"sv);
            return true;
        }
        if (!content_length.has_value()) {
            m_request_handled = true;
            send_ipc_error(411, "Length Required"sv);
            return true;
        }
        if (*content_length > MAX_APP_BINARY_FILE_BYTES) {
            m_request_handled = true;
            send_ipc_error(413, "Binary upload exceeds 128 MiB limit"sv);
            return true;
        }

        size_t body_offset = *header_end + 4;
        if (raw.length() < body_offset + static_cast<size_t>(*content_length)) {
            if (m_socket->is_eof()) {
                m_request_handled = true;
                send_ipc_error(400, "Truncated binary upload body"sv);
                return true;
            }
            return false;
        }

        m_request_handled = true;
        if (m_read_timeout_timer) {
            m_read_timeout_timer->stop();
            m_read_timeout_timer.clear();
        }

        if (!m_server) {
            m_remaining_request.clear();
            close_later();
            return true;
        }
        if (!origin_hdr.has_value() || origin_hdr->trim_whitespace() != m_server->origin()) {
            m_remaining_request.clear();
            send_ipc_error(403, "Forbidden: Missing or mismatched Origin header"sv);
            return true;
        }
        if (!token_hdr.has_value() || !constant_time_equals(token_hdr->trim_whitespace().bytes(), m_server->session_token().bytes())) {
            m_remaining_request.clear();
            send_ipc_error(403, "Forbidden: Invalid app IPC session token"sv);
            return true;
        }

        auto path_only = resource;
        if (auto q = path_only.find('?'); q.has_value())
            path_only = path_only.substring_view(0, *q);
        if (auto h = path_only.find('#'); h.has_value())
            path_only = path_only.substring_view(0, *h);
        if (!path_only.starts_with("/__ladybird_fs/"sv)) {
            m_remaining_request.clear();
            send_ipc_error(404, "Not found"sv);
            return true;
        }

        auto decoded_rel = URL::percent_decode(path_only.substring_view("/__ladybird_fs/"sv.length()));
        auto rel_utf8 = String::from_utf8(decoded_rel.bytes());
        if (rel_utf8.is_error()) {
            m_remaining_request.clear();
            send_ipc_error(400, "Invalid UTF-8 path"sv);
            return true;
        }
        auto resolved = resolve_permitted_data_path(m_server->data_directory(), m_server->manifest().permissions.fs_write, rel_utf8.value(), true);
        if (resolved.is_error()) {
            m_remaining_request.clear();
            send_ipc_error(403, "File write path denied"sv);
            return true;
        }
        auto resolved_path = resolved.release_value();
        if (FileSystem::is_directory(resolved_path)) {
            m_remaining_request.clear();
            send_ipc_error(400, "Target path is a directory"sv);
            return true;
        }
        auto parent = LexicalPath { resolved_path }.parent();
        if (auto dir_res = Core::Directory::create(parent, Core::Directory::CreateDirectories::Yes); dir_res.is_error()) {
            m_remaining_request.clear();
            send_ipc_error(500, "Unable to create parent directory"sv);
            return true;
        }
        auto file = Core::File::open(resolved_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
        if (file.is_error()) {
            m_remaining_request.clear();
            send_ipc_error(500, "Unable to open file for writing"sv);
            return true;
        }

        auto body_bytes = raw.bytes().slice(body_offset, static_cast<size_t>(*content_length));
        if (!body_bytes.is_empty()) {
            if (auto write_res = file.value()->write_until_depleted(body_bytes); write_res.is_error()) {
                m_remaining_request.clear();
                send_ipc_error(500, "Failed writing binary file"sv);
                return true;
            }
        }
        auto written_len = static_cast<u64>(body_bytes.size());
        m_remaining_request.clear();

        JsonObject result;
        result.set("path"sv, rel_utf8.release_value());
        result.set("bytesWritten"sv, written_len);
        send_ipc_ok(move(result));
        return true;
    }

    void handle_binary_fs_get(HTTP::HttpRequest const& request, StringView path_only, bool send_body)
    {
        auto header_token = find_header(request, "X-Ladybird-App-Token"sv);
        Optional<String> query_token;
        if (auto query = request.url().query(); query.has_value())
            query_token = extract_query_param(*query, "token"sv);
        bool authed = false;
        if (header_token.has_value() && constant_time_equals(header_token->trim_whitespace().bytes(), m_server->session_token().bytes()))
            authed = true;
        else if (query_token.has_value() && constant_time_equals(query_token->bytes(), m_server->session_token().bytes()))
            authed = true;

        if (!authed) {
            send_ipc_error(403, "Forbidden: Invalid app FS token"sv);
            return;
        }

        auto decoded_rel = URL::percent_decode(path_only.substring_view("/__ladybird_fs/"sv.length()));
        auto rel_utf8 = String::from_utf8(decoded_rel.bytes());
        if (rel_utf8.is_error()) {
            send_ipc_error(400, "Invalid UTF-8 path"sv);
            return;
        }

        auto resolved = resolve_permitted_data_path(m_server->data_directory(), m_server->manifest().permissions.fs_read, rel_utf8.value(), false);
        if (resolved.is_error()) {
            send_ipc_error(resolved.error().is_errno() && resolved.error().code() == ENOENT ? 404 : 403, "File read denied or not found"sv);
            return;
        }
        auto resolved_path = resolved.release_value();
        if (!FileSystem::is_regular_file(resolved_path)) {
            send_ipc_error(404, "Target is not a regular file"sv);
            return;
        }

        auto file_or_error = Core::File::open(resolved_path, Core::File::OpenMode::Read);
        if (file_or_error.is_error()) {
            send_ipc_error(500, "Unable to open file for reading"sv);
            return;
        }
        auto& file = *file_or_error.value();
        auto st = file.stat();
        if (st.is_error()) {
            send_ipc_error(500, "Unable to stat file"sv);
            return;
        }
        u64 total_size = static_cast<u64>(st.value().st_size);
        if (total_size > MAX_APP_BINARY_FILE_BYTES) {
            send_ipc_error(413, "File exceeds 128 MiB limit"sv);
            return;
        }

        auto content_type = mime_type_for_path(resolved_path);
        auto range_hdr = find_header(request, "Range"sv);
        if (range_hdr.has_value()) {
            auto range = parse_byte_range(*range_hdr, total_size);
            if (!range.has_value()) {
                auto extra = ByteString::formatted("Accept-Ranges: bytes\r\nContent-Range: bytes */{}\r\n", total_size);
                send_raw_http_response(416, "text/plain; charset=utf-8"sv, "Range Not Satisfiable"sv.bytes(), send_body, extra);
                return;
            }
            size_t slice_len = static_cast<size_t>(range->end - range->start + 1);
            ByteBuffer slice;
            if (send_body) {
                auto buf = ByteBuffer::create_uninitialized(slice_len);
                if (buf.is_error()) {
                    send_ipc_error(500, "Out of memory reading range"sv);
                    return;
                }
                slice = buf.release_value();
                if (auto seek_res = file.seek(static_cast<i64>(range->start), SeekMode::SetPosition); seek_res.is_error()) {
                    send_ipc_error(500, "Failed seeking file"sv);
                    return;
                }
                if (auto read_res = file.read_until_filled(slice.bytes()); read_res.is_error()) {
                    send_ipc_error(500, "Failed reading file range"sv);
                    return;
                }
            }
            auto extra = ByteString::formatted("Accept-Ranges: bytes\r\nContent-Range: bytes {}-{}/{}\r\n", range->start, range->end, total_size);
            send_raw_http_response(206, content_type, slice.bytes(), send_body, extra, slice_len);
            return;
        }

        ByteBuffer body_buf;
        if (send_body && total_size > 0) {
            auto data_or_error = file.read_until_eof();
            if (data_or_error.is_error()) {
                send_ipc_error(500, "Unable to read file"sv);
                return;
            }
            body_buf = data_or_error.release_value();
        }
        send_raw_http_response(200, content_type, body_buf.bytes(), send_body, "Accept-Ranges: bytes\r\n"sv, static_cast<size_t>(total_size));
    }

    void handle_http_request(HTTP::HttpRequest const& request)
    {
        if (!m_server) {
            close_later();
            return;
        }

        if (count_headers(request, "Host"sv) != 1) {
            send_ipc_error(400, "Bad Request: Exactly one Host header is required"sv);
            return;
        }

        auto host = find_header(request, "Host"sv);
        if (!host.has_value() || !is_allowed_loopback_host(*host)) {
            send_ipc_error(403, "Forbidden: Host header must be loopback"sv);
            return;
        }

        auto raw_resource = request.resource();
        auto path_only = raw_resource;
        if (auto q = path_only.find('?'); q.has_value())
            path_only = path_only.substring_view(0, *q);
        if (auto h = path_only.find('#'); h.has_value())
            path_only = path_only.substring_view(0, *h);

        if (path_only == "/__ladybird_ipc"sv) {
            handle_ipc_request(request);
            return;
        }
        if (path_only == "/__ladybird_events"sv) {
            handle_events_sse_get(request);
            return;
        }
        if (path_only.starts_with("/__ladybird_webhook/"sv)) {
            if (m_server->manifest().permissions.network == AppNetworkPolicy::Offline) {
                send_ipc_error(403, "Forbidden: Inbound webhooks are disabled in offline App Mode"sv);
                return;
            }
            handle_webhook_request(request, path_only);
            return;
        }

        if (request.method() != HTTP::HttpRequest::Method::GET && request.method() != HTTP::HttpRequest::Method::HEAD) {
            send_ipc_error(405, "Method Not Allowed"sv);
            return;
        }

        bool send_body = (request.method() == HTTP::HttpRequest::Method::GET);
        if (path_only.starts_with("/__ladybird_fs/"sv)) {
            handle_binary_fs_get(request, path_only, send_body);
            return;
        }
        if (path_only == "/__ladybird_bridge.js"sv) {
            if (auto sec_fetch_site = find_header(request, "Sec-Fetch-Site"sv); sec_fetch_site.has_value()) {
                auto trimmed = sec_fetch_site->trim_whitespace();
                if (!trimmed.equals_ignoring_ascii_case("same-origin"sv) && !trimmed.equals_ignoring_ascii_case("none"sv)) {
                    send_ipc_error(403, "Forbidden: Cross-site access to bridge script denied"sv);
                    return;
                }
            }
            auto expected_origin = m_server->origin();
            if (auto origin = find_header(request, "Origin"sv); origin.has_value() && !constant_time_equals(origin->trim_whitespace().bytes(), expected_origin.bytes())) {
                send_ipc_error(403, "Forbidden: Origin mismatch on bridge script"sv);
                return;
            }
            if (auto referer = find_header(request, "Referer"sv); referer.has_value()) {
                if (auto parsed_ref = URL::Parser::basic_parse(*referer); !parsed_ref.has_value() || parsed_ref->origin().is_opaque() || parsed_ref->origin().serialize() != expected_origin) {
                    send_ipc_error(403, "Forbidden: Cross-origin Referer on bridge script"sv);
                    return;
                }
            }
            auto script = m_server->build_bridge_script();
            send_raw_http_response(200, "application/javascript; charset=utf-8"sv, script.bytes(), send_body);
            return;
        }

        auto decoded = URL::percent_decode(path_only);
        auto rel = decoded.view();
        if (rel.starts_with('/'))
            rel = rel.substring_view(1);
        if (rel.is_empty())
            rel = m_server->manifest().entry;

        auto resolved = AppManifest::resolve_confined_subpath(m_server->manifest().root_directory, rel, false);
        if (resolved.is_error()) {
            if (resolved.error().is_errno() && resolved.error().code() == ENOENT)
                send_ipc_error(404, "Asset not found"sv);
            else
                send_ipc_error(403, "Forbidden asset path"sv);
            return;
        }

        auto resolved_path = resolved.release_value();
        if (!FileSystem::is_regular_file(resolved_path)) {
            send_ipc_error(404, "Asset is not a regular file"sv);
            return;
        }

        auto file_or_error = Core::File::open(resolved_path, Core::File::OpenMode::Read);
        if (file_or_error.is_error()) {
            send_ipc_error(404, "Unable to open asset"sv);
            return;
        }
        auto data_or_error = file_or_error.value()->read_until_eof();
        if (data_or_error.is_error()) {
            send_ipc_error(500, "Unable to read asset"sv);
            return;
        }

        send_raw_http_response(200, mime_type_for_path(resolved_path), data_or_error.value().bytes(), send_body);
    }

    void handle_webhook_request(HTTP::HttpRequest const& request, StringView path_only)
    {
        if (request.method() != HTTP::HttpRequest::Method::POST) {
            send_ipc_error(405, "Method Not Allowed: Webhook expects POST"sv);
            return;
        }
        if (request.body().size() > MAX_WEBHOOK_BODY_BYTES) {
            send_ipc_error(413, "Payload Too Large: Webhook body exceeds 64 KiB"sv);
            return;
        }
        auto trigger_id = path_only.substring_view("/__ladybird_webhook/"sv.length()).trim("/"sv, TrimMode::Both);
        if (trigger_id.is_empty()) {
            send_ipc_error(400, "Bad Request: Missing trigger id in webhook URL"sv);
            return;
        }

        StringView token;
        if (auto hdr = find_header(request, "X-Webhook-Token"sv); hdr.has_value()) {
            token = hdr->trim_whitespace();
        } else if (auto auth = find_header(request, "Authorization"sv); auth.has_value()) {
            auto trimmed = auth->trim_whitespace();
            if (trimmed.length() > 7 && trimmed.substring_view(0, 7).equals_ignoring_ascii_case("Bearer "sv))
                token = trimmed.substring_view(7).trim_whitespace();
        }

        auto dispatch = m_server->receive_webhook(trigger_id, token, StringView { request.body() });
        if (dispatch.status_code != 202) {
            send_ipc_error(dispatch.status_code, dispatch.message);
            return;
        }

        JsonObject ok;
        ok.set("ok"sv, true);
        ok.set("accepted"sv, true);
        ok.set("triggerId"sv, dispatch.trigger_id);
        ok.set("seq"sv, dispatch.event_seq);
        auto serialized = ok.serialized();
        send_raw_http_response(202, "application/json; charset=utf-8"sv, serialized.bytes());
    }

    void handle_events_sse_get(HTTP::HttpRequest const& request)
    {
        if (request.method() != HTTP::HttpRequest::Method::GET) {
            send_ipc_error(405, "Method Not Allowed: Event stream expects GET"sv);
            return;
        }
        auto header_token = find_header(request, "X-Ladybird-App-Token"sv);
        Optional<String> query_token;
        if (auto query = request.url().query(); query.has_value())
            query_token = extract_query_param(*query, "token"sv);
        bool authed = false;
        if (header_token.has_value() && constant_time_equals(header_token->trim_whitespace().bytes(), m_server->session_token().bytes()))
            authed = true;
        else if (query_token.has_value() && constant_time_equals(query_token->bytes(), m_server->session_token().bytes()))
            authed = true;
        if (!authed) {
            send_ipc_error(403, "Forbidden: Invalid app event stream token"sv);
            return;
        }
        if (m_server->sse_client_count() >= MAX_APP_SSE_CLIENTS) {
            send_ipc_error(503, "Too many active AppMode SSE streams"sv);
            return;
        }
        open_sse_stream();
    }

    void open_sse_stream()
    {
        if (m_closed || !m_server)
            return;
        m_is_sse_stream = true;
        StringBuilder builder;
        builder.append("HTTP/1.1 200 OK\r\n"sv);
        builder.append("Content-Type: text/event-stream; charset=utf-8\r\n"sv);
        builder.append("Cache-Control: no-cache\r\n"sv);
        builder.append("Connection: keep-alive\r\n"sv);
        builder.append("X-Content-Type-Options: nosniff\r\n"sv);
        builder.append("Cross-Origin-Resource-Policy: same-origin\r\n"sv);
        builder.append("\r\n"sv);
        builder.append(": connected\n\n"sv);

        if (auto result = m_socket->write_until_depleted(builder.string_view().bytes()); result.is_error()) {
            warnln("Failed to write AppMode SSE response: {}", result.error());
            close_later();
            return;
        }

        m_sse_keepalive_timer = Core::Timer::create_repeating(AK::Duration::from_seconds(25), [weak_self = make_weak_ptr()] {
            if (!weak_self || weak_self->m_closed || !weak_self->m_is_sse_stream)
                return;
            static constexpr auto keepalive = ": keepalive\n\n"sv;
            if (auto written = weak_self->m_socket->write_until_depleted(keepalive.bytes()); written.is_error())
                weak_self->close_later();
        });
        m_sse_keepalive_timer->start();
    }

    void handle_ipc_request(HTTP::HttpRequest const& request)
    {
        if (request.method() != HTTP::HttpRequest::Method::POST) {
            send_ipc_error(405, "Method Not Allowed"sv);
            return;
        }

        auto origin = find_header(request, "Origin"sv);
        if (!origin.has_value() || origin->trim_whitespace() != m_server->origin()) {
            send_ipc_error(403, "Forbidden: Missing or mismatched Origin header"sv);
            return;
        }

        auto token = find_header(request, "X-Ladybird-App-Token"sv);
        if (!token.has_value() || !constant_time_equals(token->trim_whitespace().bytes(), m_server->session_token().bytes())) {
            send_ipc_error(403, "Forbidden: Invalid app IPC session token"sv);
            return;
        }

        auto content_type = find_header(request, "Content-Type"sv);
        if (!content_type.has_value()
            || !content_type->find_first_split_view(';').trim_whitespace().equals_ignoring_ascii_case("application/json"sv)) {
            send_ipc_error(415, "Unsupported Media Type: Expected application/json"sv);
            return;
        }

        auto maybe_json = JsonValue::from_string(StringView { request.body() });
        if (maybe_json.is_error() || !maybe_json.value().is_object()) {
            send_ipc_error(400, "Bad Request: Expected JSON object"sv);
            return;
        }

        auto const& payload = maybe_json.value().as_object();
        auto command = payload.get_string("command"sv);
        if (!command.has_value() || command->is_empty()) {
            send_ipc_error(400, "Bad Request: Missing 'command' string"sv);
            return;
        }

        JsonObject args;
        if (payload.has("args"sv) && !payload.get("args"sv)->is_null()) {
            auto maybe_args = payload.get_object("args"sv);
            if (!maybe_args.has_value()) {
                send_ipc_error(400, "Bad Request: 'args' must be an object"sv);
                return;
            }
            args = *maybe_args;
        }

        auto const& manifest = m_server->manifest();

        if (*command == "system.info"sv) {
            if (!manifest.permissions.system_info) {
                send_ipc_error(403, "Permission denied: systemInfo is disabled in manifest"sv);
                return;
            }
            JsonObject account_obj;
            account_obj.set("id"sv, m_server->account_id());
            account_obj.set("label"sv, m_server->account_label());
            account_obj.set("profileName"sv, m_server->profile_name());

            JsonObject result;
            result.set("id"sv, manifest.id);
            result.set("name"sv, manifest.name);
            result.set("version"sv, manifest.version);
            result.set("platform"sv, platform_name());
            result.set("ladybirdVersion"sv, Core::Version::read_long_version_string());
            result.set("port"sv, static_cast<u64>(m_server->local_port()));
            result.set("stablePort"sv, m_server->is_using_stable_port());
            result.set("mcpAccess"sv, mcp_access_mode_name(manifest.mcp.access));
            result.set("networkPolicy"sv, network_policy_name(manifest.permissions.network));
            result.set("crossOriginIsolated"sv, manifest.permissions.cross_origin_isolated);
            result.set("account"sv, move(account_obj));
            if (auto mcp_port = m_server->current_mcp_port(); mcp_port.has_value())
                result.set("mcpPort"sv, static_cast<u64>(*mcp_port));
            else
                result.set("mcpPort"sv, JsonValue {});
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "app.quit"sv) {
            JsonObject result;
            result.set("quitting"sv, true);
            send_ipc_ok(move(result));
            m_server->request_quit();
            return;
        }

        if (command->bytes_as_string_view().starts_with("kv."sv)) {
            if (!manifest.permissions.storage) {
                send_ipc_error(403, "Permission denied: storage is disabled in manifest"sv);
                return;
            }
            auto* kv = m_server->kv_store();
            if (!kv) {
                send_ipc_error(503, "Persistent SQLite KV storage is unavailable (SQL database disabled)"sv);
                return;
            }

            if (*command == "kv.get"sv) {
                auto key = args.get_string("key"sv);
                if (!key.has_value() || !AppKVStore::is_valid_key(*key)) {
                    send_ipc_error(400, "Invalid or missing 'key' argument"sv);
                    return;
                }
                auto entry = kv->get(*key);
                if (entry.is_error()) {
                    send_ipc_error(500, "Failed reading KV entry"sv);
                    return;
                }
                JsonObject result;
                result.set("key"sv, *key);
                if (entry.value().has_value()) {
                    result.set("found"sv, true);
                    result.set("value"sv, move(entry.value()->value));
                    result.set("updatedAt"sv, entry.value()->updated_at_ms);
                } else {
                    result.set("found"sv, false);
                    result.set("value"sv, JsonValue {});
                    result.set("updatedAt"sv, JsonValue {});
                }
                send_ipc_ok(move(result));
                return;
            }

            if (*command == "kv.set"sv) {
                auto key = args.get_string("key"sv);
                if (!key.has_value() || !AppKVStore::is_valid_key(*key)) {
                    send_ipc_error(400, "Invalid or missing 'key' argument"sv);
                    return;
                }
                if (!args.has("value"sv)) {
                    send_ipc_error(400, "Missing 'value' argument"sv);
                    return;
                }
                auto updated_at = kv->set(*key, *args.get("value"sv));
                if (updated_at.is_error()) {
                    send_ipc_error(413, "Failed storing KV entry (check 4 MiB limit)"sv);
                    return;
                }
                JsonObject result;
                result.set("key"sv, *key);
                result.set("updatedAt"sv, updated_at.value());
                send_ipc_ok(move(result));
                return;
            }

            if (*command == "kv.delete"sv) {
                auto key = args.get_string("key"sv);
                if (!key.has_value() || !AppKVStore::is_valid_key(*key)) {
                    send_ipc_error(400, "Invalid or missing 'key' argument"sv);
                    return;
                }
                auto deleted = kv->remove(*key);
                if (deleted.is_error()) {
                    send_ipc_error(500, "Failed deleting KV entry"sv);
                    return;
                }
                JsonObject result;
                result.set("key"sv, *key);
                result.set("deleted"sv, deleted.value());
                send_ipc_ok(move(result));
                return;
            }

            if (*command == "kv.list"sv) {
                Optional<String> prefix_storage;
                Optional<StringView> prefix;
                if (args.has("prefix"sv) && !args.get("prefix"sv)->is_null()) {
                    prefix_storage = args.get_string("prefix"sv).copy();
                    if (!prefix_storage.has_value()) {
                        send_ipc_error(400, "'prefix' must be a string"sv);
                        return;
                    }
                    prefix = prefix_storage->bytes_as_string_view();
                }
                auto list_res = kv->list(prefix);
                if (list_res.is_error()) {
                    send_ipc_error(500, "Failed listing KV entries"sv);
                    return;
                }
                JsonArray keys_json;
                for (auto const& item : list_res.value()) {
                    JsonObject entry_obj;
                    entry_obj.set("key"sv, item.key);
                    entry_obj.set("updatedAt"sv, item.updated_at_ms);
                    entry_obj.set("bytes"sv, item.bytes);
                    keys_json.must_append(move(entry_obj));
                }
                JsonObject result;
                result.set("prefix"sv, prefix.has_value() ? JsonValue { *prefix_storage } : JsonValue {});
                result.set("keys"sv, move(keys_json));
                send_ipc_ok(move(result));
                return;
            }

            if (*command == "kv.clear"sv) {
                Optional<String> prefix_storage;
                Optional<StringView> prefix;
                if (args.has("prefix"sv) && !args.get("prefix"sv)->is_null()) {
                    prefix_storage = args.get_string("prefix"sv).copy();
                    if (!prefix_storage.has_value()) {
                        send_ipc_error(400, "'prefix' must be a string"sv);
                        return;
                    }
                    prefix = prefix_storage->bytes_as_string_view();
                }
                auto cleared = kv->clear(prefix);
                if (cleared.is_error()) {
                    send_ipc_error(500, "Failed clearing KV entries"sv);
                    return;
                }
                JsonObject result;
                result.set("cleared"sv, cleared.value());
                send_ipc_ok(move(result));
                return;
            }
        }

        if (*command == "accounts.current"sv) {
            JsonObject result;
            result.set("id"sv, m_server->account_id());
            result.set("label"sv, m_server->account_label());
            result.set("profileName"sv, m_server->profile_name());
            result.set("dataDirectory"sv, String::from_utf8_with_replacement_character(m_server->data_directory()));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "accounts.list"sv) {
            Vector<AppAccountRecord> records;
            if (m_server->accounts_registry_path().has_value()) {
                auto list_res = AppAccountRegistry::list(*m_server->accounts_registry_path());
                if (list_res.is_error()) {
                    send_ipc_error(500, "Failed reading accounts registry"sv);
                    return;
                }
                records = list_res.release_value();
            }

            bool found_active = false;
            JsonArray accounts_json;
            for (auto const& rec : records) {
                bool is_active = (rec.id == m_server->account_id());
                if (is_active)
                    found_active = true;
                JsonObject item;
                item.set("id"sv, rec.id);
                item.set("label"sv, rec.label);
                item.set("profileName"sv, rec.profile_name);
                item.set("active"sv, is_active);
                item.set("createdAtMs"sv, rec.created_at_ms);
                item.set("lastUsedAtMs"sv, rec.last_used_at_ms);
                accounts_json.must_append(move(item));
            }
            if (!found_active) {
                JsonObject active_item;
                active_item.set("id"sv, m_server->account_id());
                active_item.set("label"sv, m_server->account_label());
                active_item.set("profileName"sv, m_server->profile_name());
                active_item.set("active"sv, true);
                active_item.set("createdAtMs"sv, static_cast<i64>(0));
                active_item.set("lastUsedAtMs"sv, static_cast<i64>(0));
                accounts_json.must_append(move(active_item));
            }

            JsonObject result;
            result.set("activeAccountId"sv, m_server->account_id());
            result.set("accounts"sv, move(accounts_json));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "accounts.create"sv) {
            if (!m_server->accounts_registry_path().has_value()) {
                send_ipc_error(400, "Account registry is disabled in temporary profiles"sv);
                return;
            }
            auto account_id = args.get_string("id"sv);
            if (!account_id.has_value() || !AppManifest::is_valid_app_id(*account_id)) {
                send_ipc_error(400, "Account 'id' must start with [a-z0-9] and contain only [a-z0-9_-] (1..64 chars)"sv);
                return;
            }
            Optional<String> label_storage = args.get_string("label"sv).copy();
            Optional<StringView> label_view;
            if (label_storage.has_value())
                label_view = label_storage->bytes_as_string_view();

            auto created = AppAccountRegistry::upsert(*m_server->accounts_registry_path(), manifest.id, *account_id, label_view, false);
            if (created.is_error()) {
                send_ipc_error(500, "Failed persisting new account record"sv);
                return;
            }
            auto const& rec = created.value();
            JsonObject acc_obj;
            acc_obj.set("id"sv, rec.id);
            acc_obj.set("label"sv, rec.label);
            acc_obj.set("profileName"sv, rec.profile_name);
            acc_obj.set("active"sv, rec.id == m_server->account_id());
            acc_obj.set("createdAtMs"sv, rec.created_at_ms);
            acc_obj.set("lastUsedAtMs"sv, rec.last_used_at_ms);

            JsonObject result;
            result.set("account"sv, move(acc_obj));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "accounts.launch"sv) {
            auto account_id = args.get_string("id"sv);
            if (!account_id.has_value() || !AppManifest::is_valid_app_id(*account_id)) {
                send_ipc_error(400, "Account 'id' must start with [a-z0-9] and contain only [a-z0-9_-] (1..64 chars)"sv);
                return;
            }
            bool create_if_missing = args.get_bool("createIfMissing"sv).value_or(false);
            if (m_server->accounts_registry_path().has_value()) {
                auto found = AppAccountRegistry::find(*m_server->accounts_registry_path(), *account_id);
                if (found.is_error()) {
                    send_ipc_error(500, "Failed reading accounts registry"sv);
                    return;
                }
                if (!found.value().has_value()) {
                    if (create_if_missing || *account_id == "default"sv) {
                        Optional<String> label_storage = args.get_string("label"sv).copy();
                        Optional<StringView> label_view;
                        if (label_storage.has_value())
                            label_view = label_storage->bytes_as_string_view();
                        if (auto up = AppAccountRegistry::upsert(*m_server->accounts_registry_path(), manifest.id, *account_id, label_view, false); up.is_error()) {
                            send_ipc_error(500, "Failed registering account before launch"sv);
                            return;
                        }
                    } else {
                        send_ipc_error(404, "Account not found; pass createIfMissing: true or create it first"sv);
                        return;
                    }
                }
            }

            if (auto launch_res = m_server->launch_account(*account_id); launch_res.is_error()) {
                send_ipc_error(501, "Failed launching account window"sv);
                return;
            }
            JsonObject result;
            result.set("launched"sv, true);
            result.set("id"sv, *account_id);
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "fs.listDir"sv) {
            auto req_path = args.get_string("path"sv);
            if (!req_path.has_value()) {
                send_ipc_error(400, "Missing string argument 'path'"sv);
                return;
            }
            auto resolved = resolve_permitted_data_path(m_server->data_directory(), manifest.permissions.fs_read, *req_path, false);
            if (resolved.is_error()) {
                send_ipc_error(resolved.error().is_errno() && resolved.error().code() == ENOENT ? 404 : 403, "Directory access denied or missing"sv);
                return;
            }
            auto resolved_path = resolved.release_value();
            if (!FileSystem::is_directory(resolved_path)) {
                send_ipc_error(400, "Target path is not a directory"sv);
                return;
            }
            JsonArray entries;
            auto iter_res = Core::Directory::for_each_entry(resolved_path, Core::DirIterator::SkipParentAndBaseDir, [&](Core::DirectoryEntry const& entry, Core::Directory const&) -> ErrorOr<IterationDecision> {
                if (entry.type == Core::DirectoryEntry::Type::SymbolicLink)
                    return IterationDecision::Continue;
                auto full_child = LexicalPath::join(resolved_path, entry.name).string();
                JsonObject item;
                item.set("name"sv, String::from_utf8_without_validation(entry.name.bytes()));
                bool is_dir = (entry.type == Core::DirectoryEntry::Type::Directory);
                item.set("isDirectory"sv, is_dir);
                if (!is_dir) {
                    auto sz = FileSystem::size_from_stat(full_child);
                    item.set("size"sv, sz.is_error() ? 0 : static_cast<u64>(sz.value()));
                } else {
                    item.set("size"sv, static_cast<u64>(0));
                }
                entries.must_append(move(item));
                return IterationDecision::Continue;
            });
            if (iter_res.is_error()) {
                send_ipc_error(500, "Failed to enumerate directory"sv);
                return;
            }
            JsonObject result;
            result.set("path"sv, *req_path);
            result.set("entries"sv, move(entries));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "fs.readText"sv) {
            auto req_path = args.get_string("path"sv);
            if (!req_path.has_value()) {
                send_ipc_error(400, "Missing string argument 'path'"sv);
                return;
            }
            auto resolved = resolve_permitted_data_path(m_server->data_directory(), manifest.permissions.fs_read, *req_path, false);
            if (resolved.is_error()) {
                send_ipc_error(resolved.error().is_errno() && resolved.error().code() == ENOENT ? 404 : 403, "File read denied or not found"sv);
                return;
            }
            auto resolved_path = resolved.release_value();
            if (!FileSystem::is_regular_file(resolved_path)) {
                send_ipc_error(404, "Target is not a regular file"sv);
                return;
            }
            auto size = FileSystem::size_from_stat(resolved_path);
            if (!size.is_error() && static_cast<size_t>(size.value()) > MAX_APP_IPC_FILE_BYTES) {
                send_ipc_error(413, "File exceeds 4 MiB read limit"sv);
                return;
            }
            auto file = Core::File::open(resolved_path, Core::File::OpenMode::Read);
            if (file.is_error()) {
                send_ipc_error(500, "Unable to open file for reading"sv);
                return;
            }
            auto bytes = file.value()->read_until_eof();
            if (bytes.is_error()) {
                send_ipc_error(500, "Unable to read file"sv);
                return;
            }
            auto utf8 = String::from_utf8(bytes.value());
            if (utf8.is_error()) {
                send_ipc_error(400, "File is not valid UTF-8 text"sv);
                return;
            }
            JsonObject result;
            result.set("path"sv, *req_path);
            result.set("content"sv, utf8.release_value());
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "fs.writeText"sv) {
            auto req_path = args.get_string("path"sv);
            auto content = args.get_string("content"sv);
            if (!req_path.has_value() || !content.has_value()) {
                send_ipc_error(400, "Missing string arguments 'path' and 'content'"sv);
                return;
            }
            if (content->bytes().size() > MAX_APP_IPC_FILE_BYTES) {
                send_ipc_error(413, "Content exceeds 4 MiB write limit"sv);
                return;
            }
            auto resolved = resolve_permitted_data_path(m_server->data_directory(), manifest.permissions.fs_write, *req_path, true);
            if (resolved.is_error()) {
                send_ipc_error(403, "File write path denied"sv);
                return;
            }
            auto resolved_path = resolved.release_value();
            if (FileSystem::is_directory(resolved_path)) {
                send_ipc_error(400, "Target path is a directory"sv);
                return;
            }
            auto parent = LexicalPath { resolved_path }.parent();
            if (auto dir_res = Core::Directory::create(parent, Core::Directory::CreateDirectories::Yes); dir_res.is_error()) {
                send_ipc_error(500, "Unable to create parent directory"sv);
                return;
            }
            auto file = Core::File::open(resolved_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
            if (file.is_error()) {
                send_ipc_error(500, "Unable to open file for writing"sv);
                return;
            }
            if (auto write_res = file.value()->write_until_depleted(content->bytes()); write_res.is_error()) {
                send_ipc_error(500, "Failed writing file"sv);
                return;
            }
            JsonObject result;
            result.set("path"sv, *req_path);
            result.set("bytesWritten"sv, static_cast<u64>(content->bytes().size()));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "fs.deleteFile"sv) {
            auto req_path = args.get_string("path"sv);
            if (!req_path.has_value()) {
                send_ipc_error(400, "Missing string argument 'path'"sv);
                return;
            }
            auto resolved = resolve_permitted_data_path(m_server->data_directory(), manifest.permissions.fs_write, *req_path, false);
            if (resolved.is_error()) {
                send_ipc_error(resolved.error().is_errno() && resolved.error().code() == ENOENT ? 404 : 403, "File delete denied or not found"sv);
                return;
            }
            auto resolved_path = resolved.release_value();
            if (!FileSystem::is_regular_file(resolved_path)) {
                send_ipc_error(400, "Target is not a regular file"sv);
                return;
            }
            if (auto rm_res = FileSystem::remove(resolved_path, FileSystem::RecursionMode::Disallowed); rm_res.is_error()) {
                send_ipc_error(500, "Failed deleting file"sv);
                return;
            }
            JsonObject result;
            result.set("deleted"sv, true);
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "fs.stat"sv) {
            auto req_path = args.get_string("path"sv);
            if (!req_path.has_value()) {
                send_ipc_error(400, "Missing string argument 'path'"sv);
                return;
            }
            auto resolved = resolve_permitted_data_path(m_server->data_directory(), manifest.permissions.fs_read, *req_path, false);
            if (resolved.is_error()) {
                if (resolved.error().is_errno() && resolved.error().code() == ENOENT) {
                    auto check_allowed = resolve_permitted_data_path(m_server->data_directory(), manifest.permissions.fs_read, *req_path, true);
                    if (check_allowed.is_error()) {
                        send_ipc_error(403, "Path access denied"sv);
                        return;
                    }
                    JsonObject result;
                    result.set("path"sv, *req_path);
                    result.set("exists"sv, false);
                    result.set("isDirectory"sv, false);
                    result.set("isFile"sv, false);
                    result.set("size"sv, static_cast<u64>(0));
                    send_ipc_ok(move(result));
                    return;
                }
                send_ipc_error(403, "Path access denied"sv);
                return;
            }
            auto resolved_path = resolved.release_value();
            auto st = Core::File::stat(resolved_path);
            if (st.is_error()) {
                send_ipc_error(500, "Unable to stat target path"sv);
                return;
            }
            bool is_dir = S_ISDIR(st.value().st_mode);
            bool is_file = S_ISREG(st.value().st_mode);
            JsonObject result;
            result.set("path"sv, *req_path);
            result.set("exists"sv, true);
            result.set("isDirectory"sv, is_dir);
            result.set("isFile"sv, is_file);
            result.set("size"sv, is_file ? static_cast<u64>(st.value().st_size) : static_cast<u64>(0));
            result.set("modifiedAtMs"sv, static_cast<i64>(st.value().st_mtime) * 1000);
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "fs.mkdir"sv) {
            auto req_path = args.get_string("path"sv);
            if (!req_path.has_value()) {
                send_ipc_error(400, "Missing string argument 'path'"sv);
                return;
            }
            auto resolved = resolve_permitted_data_path(m_server->data_directory(), manifest.permissions.fs_write, *req_path, true);
            if (resolved.is_error()) {
                send_ipc_error(403, "Directory create path denied"sv);
                return;
            }
            auto resolved_path = resolved.release_value();
            if (FileSystem::is_regular_file(resolved_path)) {
                send_ipc_error(400, "Target path already exists as a regular file"sv);
                return;
            }
            if (auto dir_res = Core::Directory::create(resolved_path, Core::Directory::CreateDirectories::Yes); dir_res.is_error()) {
                send_ipc_error(500, "Unable to create directory"sv);
                return;
            }
            JsonObject result;
            result.set("path"sv, *req_path);
            result.set("created"sv, true);
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "tabs.list"sv) {
            auto live_states = m_server->list_service_tabs();
            JsonArray arr;
            for (auto const& svc : m_server->effective_services()) {
                Optional<AppServiceTabState const&> matched;
                for (auto const& st : live_states) {
                    if (st.service_id == svc.id) {
                        matched = st;
                        break;
                    }
                }
                arr.must_append(service_tab_state_to_json(svc, matched, m_server->is_dynamic_service(svc.id)));
            }
            JsonObject result;
            result.set("services"sv, move(arr));
            result.set("lastSeq"sv, m_server->last_notification_seq());
            send_ipc_ok(move(result));
            return;
        }

        if (manifest.permissions.network == AppNetworkPolicy::Offline
            && command->is_one_of("tabs.registerService"sv, "tabs.open"sv, "tabs.focus"sv, "tabs.navigate"sv, "tabs.eval"sv, "tabs.readMarkdown"sv, "tabs.listTools"sv, "tabs.callTool"sv, "tabs.runFlow"sv, "triggers.save"sv)) {
            send_ipc_error(403, "Forbidden: Service tab operations and triggers are disabled in offline App Mode"sv);
            return;
        }

        if (*command == "tabs.registerService"sv) {
            if (!manifest.permissions.allow_dynamic_services) {
                send_ipc_error(403, "Forbidden: Dynamic services are disabled in manifest"sv);
                return;
            }
            JsonObject const* cfg_ptr = &args;
            if (auto nested = args.get_object("service"sv); nested.has_value())
                cfg_ptr = &(*nested);
            auto raw_id = cfg_ptr->get_string("id"sv);
            if (!raw_id.has_value() || raw_id->is_empty())
                raw_id = args.get_string("serviceId"sv);
            if (!raw_id.has_value() || raw_id->is_empty()) {
                send_ipc_error(400, "Missing string argument 'id'"sv);
                return;
            }
            auto parsed = AppManifest::parse_service_config(*raw_id, *cfg_ptr);
            if (parsed.is_error()) {
                send_ipc_error(400, parsed.error().string_literal());
                return;
            }
            auto new_cfg = parsed.release_value();
            if (manifest.find_service(new_cfg.id).has_value()) {
                send_ipc_error(400, "Cannot override a manifest-declared service"sv);
                return;
            }

            bool needs_consent = true;
            if (auto existing = m_server->find_effective_service(new_cfg.id); existing.has_value()) {
                if (existing->origins == new_cfg.origins
                    && existing->container == new_cfg.container
                    && existing->allow_navigate == new_cfg.allow_navigate
                    && existing->allow_notifications == new_cfg.allow_notifications
                    && existing->allow_read == new_cfg.allow_read
                    && existing->allow_webmcp == new_cfg.allow_webmcp
                    && existing->allow_eval == new_cfg.allow_eval
                    && existing->allow_flows == new_cfg.allow_flows
                    && existing->allow_network == new_cfg.allow_network) {
                    needs_consent = false;
                }
            }

            auto commit_service = [self_ref = NonnullRefPtr<Connection> { *this }, weak_server = m_server->make_weak_ptr(), cfg = new_cfg]() mutable {
                if (self_ref->m_closed || !weak_server)
                    return;
                auto saved = weak_server->register_dynamic_service(move(cfg));
                if (saved.is_error()) {
                    self_ref->send_ipc_error(400, saved.error().string_literal());
                    return;
                }
                JsonObject result;
                result.set("service"sv, service_tab_state_to_json(saved.value(), {}, true));
                self_ref->send_ipc_ok(move(result));
            };

            if (!needs_consent || !m_server->has_service_register_consent_handler()) {
                commit_service();
                return;
            }

            NonnullRefPtr<Connection> self_ref { *this };
            m_server->request_service_register_consent(new_cfg, [self_ref, commit_service = move(commit_service)](bool approved) mutable {
                if (self_ref->m_closed)
                    return;
                if (!approved) {
                    self_ref->send_ipc_error(403, "Forbidden: User denied dynamic service registration"sv);
                    return;
                }
                commit_service();
            });
            return;
        }

        if (*command == "tabs.removeService"sv) {
            if (!manifest.permissions.allow_dynamic_services) {
                send_ipc_error(403, "Forbidden: Dynamic services are disabled in manifest"sv);
                return;
            }
            auto service_id = args.get_string("serviceId"sv);
            if (!service_id.has_value() || service_id->is_empty()) {
                send_ipc_error(400, "Missing string argument 'serviceId'"sv);
                return;
            }
            if (manifest.find_service(*service_id).has_value()) {
                send_ipc_error(403, "Forbidden: Cannot remove a manifest-declared service"sv);
                return;
            }
            (void)m_server->close_service_tab(*service_id);
            bool removed = m_server->remove_dynamic_service(*service_id);
            if (!removed) {
                send_ipc_error(404, "Dynamic service not found"sv);
                return;
            }
            JsonObject result;
            result.set("removed"sv, true);
            result.set("serviceId"sv, *service_id);
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "tabs.open"sv) {
            auto service_id = args.get_string("serviceId"sv);
            if (!service_id.has_value() || service_id->is_empty()) {
                send_ipc_error(400, "Missing string argument 'serviceId'"sv);
                return;
            }
            auto svc = m_server->find_effective_service(*service_id);
            if (!svc.has_value()) {
                send_ipc_error(403, "Forbidden: Service is not declared in manifest"sv);
                return;
            }
            Optional<URL::URL> parsed_url;
            if (args.has("url"sv) && !args.get("url"sv)->is_null()) {
                auto raw_url = args.get_string("url"sv);
                if (!raw_url.has_value() || raw_url->is_empty()) {
                    send_ipc_error(400, "'url' must be a non-empty string"sv);
                    return;
                }
                parsed_url = URL::Parser::basic_parse(*raw_url);
                if (!parsed_url.has_value() || !AppManifest::is_service_origin_allowed(*svc, *parsed_url)) {
                    send_ipc_error(403, "Forbidden: URL origin is not allowed for this service"sv);
                    return;
                }
            }
            Optional<String> container_override;
            if (args.has("container"sv) && !args.get("container"sv)->is_null()) {
                auto raw_container = args.get_string("container"sv);
                if (!raw_container.has_value()) {
                    send_ipc_error(400, "'container' must be a string"sv);
                    return;
                }
                if (!raw_container->is_empty() && !AppManifest::is_valid_app_id(*raw_container)) {
                    send_ipc_error(400, "Invalid 'container' argument"sv);
                    return;
                }
                container_override = *raw_container;
            }
            bool activate = !args.get_bool("background"sv).value_or(false);
            if (args.has("activate"sv) && !args.get("activate"sv)->is_null())
                activate = args.get_bool("activate"sv).value_or(activate);
            bool reuse = args.get_bool("reuse"sv).value_or(true);

            auto opened = m_server->open_service_tab(*service_id, move(parsed_url), activate, reuse, move(container_override));
            if (opened.is_error()) {
                send_ipc_error(501, opened.error().string_literal());
                return;
            }
            JsonObject result;
            result.set("service"sv, service_tab_state_to_json(*svc, opened.value(), m_server->is_dynamic_service(*service_id)));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "tabs.focus"sv) {
            auto service_id = args.get_string("serviceId"sv);
            if (!service_id.has_value() || service_id->is_empty()) {
                send_ipc_error(400, "Missing string argument 'serviceId'"sv);
                return;
            }
            auto svc = m_server->find_effective_service(*service_id);
            if (!svc.has_value()) {
                send_ipc_error(403, "Forbidden: Service is not declared in manifest"sv);
                return;
            }
            auto focused = m_server->focus_service_tab(*service_id);
            if (focused.is_error()) {
                send_ipc_error(404, focused.error().string_literal());
                return;
            }
            JsonObject result;
            result.set("service"sv, service_tab_state_to_json(*svc, focused.value(), m_server->is_dynamic_service(*service_id)));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "tabs.navigate"sv) {
            auto service_id = args.get_string("serviceId"sv);
            auto raw_url = args.get_string("url"sv);
            if (!service_id.has_value() || service_id->is_empty() || !raw_url.has_value() || raw_url->is_empty()) {
                send_ipc_error(400, "Missing 'serviceId' or 'url' string argument"sv);
                return;
            }
            auto svc = m_server->find_effective_service(*service_id);
            if (!svc.has_value()) {
                send_ipc_error(403, "Forbidden: Service is not declared in manifest"sv);
                return;
            }
            if (!svc->allow_navigate) {
                send_ipc_error(403, "Forbidden: 'navigate' permission is disabled for this service"sv);
                return;
            }
            auto parsed_url = URL::Parser::basic_parse(*raw_url);
            if (!parsed_url.has_value() || !AppManifest::is_service_origin_allowed(*svc, *parsed_url)) {
                send_ipc_error(403, "Forbidden: URL origin is not allowed for this service"sv);
                return;
            }
            auto nav = m_server->navigate_service_tab(*service_id, parsed_url.release_value());
            if (nav.is_error()) {
                send_ipc_error(404, nav.error().string_literal());
                return;
            }
            JsonObject result;
            result.set("service"sv, service_tab_state_to_json(*svc, nav.value(), m_server->is_dynamic_service(*service_id)));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "tabs.close"sv) {
            auto service_id = args.get_string("serviceId"sv);
            if (!service_id.has_value() || service_id->is_empty()) {
                send_ipc_error(400, "Missing string argument 'serviceId'"sv);
                return;
            }
            auto svc = m_server->find_effective_service(*service_id);
            if (!svc.has_value()) {
                send_ipc_error(403, "Forbidden: Service is not declared in manifest"sv);
                return;
            }
            auto closed = m_server->close_service_tab(*service_id);
            if (closed.is_error()) {
                send_ipc_error(501, closed.error().string_literal());
                return;
            }
            JsonObject result;
            result.set("closed"sv, closed.value());
            result.set("serviceId"sv, *service_id);
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "tabs.notifications"sv) {
            Optional<String> filter_service = args.get_string("serviceId"sv).copy();
            if (filter_service.has_value() && !filter_service->is_empty()) {
                auto svc = m_server->find_effective_service(*filter_service);
                if (!svc.has_value() || !svc->allow_notifications) {
                    send_ipc_error(403, "Forbidden: Notifications disabled or service undeclared"sv);
                    return;
                }
            }
            u64 since_seq = args.get_u64("sinceSeq"sv).value_or(0);
            size_t limit = static_cast<size_t>(min<u64>(args.get_u64("limit"sv).value_or(50), MAX_SERVICE_NOTIFICATIONS));

            JsonArray items;
            for (auto const& n : m_server->service_notifications()) {
                if (n.seq <= since_seq)
                    continue;
                if (filter_service.has_value() && !filter_service->is_empty() && n.service_id != *filter_service)
                    continue;
                JsonObject obj;
                obj.set("seq"sv, n.seq);
                obj.set("timestampMs"sv, n.timestamp_ms);
                obj.set("serviceId"sv, n.service_id);
                if (n.tab_id.has_value())
                    obj.set("tabId"sv, *n.tab_id);
                else
                    obj.set("tabId"sv, JsonValue {});
                obj.set("origin"sv, n.origin);
                obj.set("eventType"sv, n.event_type);
                obj.set("title"sv, n.title);
                obj.set("body"sv, n.body);
                obj.set("tag"sv, n.tag);
                items.must_append(move(obj));
                if (items.size() >= limit)
                    break;
            }
            JsonObject result;
            result.set("notifications"sv, move(items));
            result.set("lastSeq"sv, m_server->last_notification_seq());
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "tabs.eval"sv) {
            auto service_id = args.get_string("serviceId"sv);
            auto script = args.get_string("script"sv);
            if (!service_id.has_value() || service_id->is_empty() || !script.has_value() || script->is_empty()) {
                send_ipc_error(400, "Missing 'serviceId' or 'script' string argument"sv);
                return;
            }
            if (script->bytes().size() > MAX_SERVICE_EVAL_BYTES) {
                send_ipc_error(413, "Service eval script exceeds 64 KiB limit"sv);
                return;
            }
            auto svc = m_server->find_effective_service(*service_id);
            if (!svc.has_value() || !svc->allow_eval) {
                send_ipc_error(403, "Forbidden: 'eval' permission is disabled or service undeclared"sv);
                return;
            }
            NonnullRefPtr<Connection> self_ref { *this };
            m_server->eval_service_tab(*service_id, *script, [self_ref, svc_id = *service_id](Optional<JsonValue> value, Optional<String> error) mutable {
                if (self_ref->m_closed)
                    return;
                if (error.has_value()) {
                    self_ref->send_ipc_error(400, *error);
                    return;
                }
                JsonObject result;
                result.set("serviceId"sv, svc_id);
                result.set("value"sv, value.value_or(JsonValue {}));
                self_ref->send_ipc_ok(move(result));
            });
            return;
        }

        if (*command == "tabs.readMarkdown"sv) {
            auto service_id = args.get_string("serviceId"sv);
            if (!service_id.has_value() || service_id->is_empty()) {
                send_ipc_error(400, "Missing string argument 'serviceId'"sv);
                return;
            }
            Optional<String> root_selector;
            if (args.has("rootSelector"sv) && !args.get("rootSelector"sv)->is_null()) {
                auto raw_sel = args.get_string("rootSelector"sv);
                if (!raw_sel.has_value()) {
                    send_ipc_error(400, "'rootSelector' must be a string"sv);
                    return;
                }
                if (!raw_sel->is_empty())
                    root_selector = *raw_sel;
            }
            bool main_content_only = args.get_bool("mainContentOnly"sv).value_or(true);
            bool include_links = args.get_bool("includeLinks"sv).value_or(true);
            bool include_images = args.get_bool("includeImages"sv).value_or(false);
            size_t max_chars = static_cast<size_t>(clamp<u64>(args.get_u64("maxChars"sv).value_or(40000), 500, 200000));

            auto svc = m_server->find_effective_service(*service_id);
            if (!svc.has_value() || !svc->allow_read) {
                send_ipc_error(403, "Forbidden: 'read' permission is disabled or service undeclared"sv);
                return;
            }

            NonnullRefPtr<Connection> self_ref { *this };
            m_server->read_service_tab_markdown(*service_id, move(root_selector), main_content_only, include_links, include_images, max_chars, [self_ref, svc_id = *service_id](Optional<JsonValue> value, Optional<String> error) mutable {
                if (self_ref->m_closed)
                    return;
                if (error.has_value()) {
                    self_ref->send_ipc_error(400, *error);
                    return;
                }
                JsonObject result;
                if (value.has_value() && value->is_object())
                    result = value->as_object();
                else if (value.has_value())
                    result.set("result"sv, move(*value));
                result.set("serviceId"sv, svc_id);
                self_ref->send_ipc_ok(move(result));
            });
            return;
        }

        if (*command == "tabs.listTools"sv) {
            auto service_id = args.get_string("serviceId"sv);
            if (!service_id.has_value() || service_id->is_empty()) {
                send_ipc_error(400, "Missing string argument 'serviceId'"sv);
                return;
            }
            bool include_declarative = args.get_bool("includeDeclarative"sv).value_or(true);
            auto svc = m_server->find_effective_service(*service_id);
            if (!svc.has_value() || !svc->allow_webmcp) {
                send_ipc_error(403, "Forbidden: 'webmcp' permission is disabled or service undeclared"sv);
                return;
            }

            NonnullRefPtr<Connection> self_ref { *this };
            m_server->list_service_tab_tools(*service_id, include_declarative, [self_ref, svc_id = *service_id](Optional<JsonValue> value, Optional<String> error) mutable {
                if (self_ref->m_closed)
                    return;
                if (error.has_value()) {
                    self_ref->send_ipc_error(400, *error);
                    return;
                }
                JsonObject result;
                if (value.has_value() && value->is_object())
                    result = value->as_object();
                else if (value.has_value())
                    result.set("tools"sv, move(*value));
                result.set("serviceId"sv, svc_id);
                self_ref->send_ipc_ok(move(result));
            });
            return;
        }

        if (*command == "tabs.callTool"sv) {
            auto service_id = args.get_string("serviceId"sv);
            auto tool_name = args.get_string("toolName"sv);
            if (!service_id.has_value() || service_id->is_empty() || !tool_name.has_value() || tool_name->is_empty()) {
                send_ipc_error(400, "Missing string argument 'serviceId' or 'toolName'"sv);
                return;
            }
            JsonObject tool_args;
            if (args.has("arguments"sv) && !args.get("arguments"sv)->is_null()) {
                auto maybe_args = args.get_object("arguments"sv);
                if (!maybe_args.has_value()) {
                    send_ipc_error(400, "'arguments' must be a JSON object"sv);
                    return;
                }
                tool_args = *maybe_args;
            }

            auto svc = m_server->find_effective_service(*service_id);
            if (!svc.has_value() || !svc->allow_webmcp) {
                send_ipc_error(403, "Forbidden: 'webmcp' permission is disabled or service undeclared"sv);
                return;
            }

            NonnullRefPtr<Connection> self_ref { *this };
            m_server->call_service_tab_tool(*service_id, *tool_name, move(tool_args), [self_ref, svc_id = *service_id, t_name = *tool_name](Optional<JsonValue> value, Optional<String> error) mutable {
                if (self_ref->m_closed)
                    return;
                if (error.has_value()) {
                    self_ref->send_ipc_error(400, *error);
                    return;
                }
                JsonObject result;
                if (value.has_value() && value->is_object())
                    result = value->as_object();
                else if (value.has_value())
                    result.set("result"sv, move(*value));
                result.set("serviceId"sv, svc_id);
                result.set("toolName"sv, t_name);
                self_ref->send_ipc_ok(move(result));
            });
            return;
        }

        if (*command == "tabs.network"sv) {
            Optional<String> filter_service = args.get_string("serviceId"sv).copy();
            if (filter_service.has_value() && !filter_service->is_empty()) {
                auto svc = m_server->find_effective_service(*filter_service);
                if (!svc.has_value() || !svc->allow_network) {
                    send_ipc_error(403, "Forbidden: Network events disabled or service undeclared"sv);
                    return;
                }
            }
            u64 since_seq = args.get_u64("sinceSeq"sv).value_or(0);
            size_t limit = static_cast<size_t>(min<u64>(args.get_u64("limit"sv).value_or(50), MAX_SERVICE_NETWORK_EVENTS));
            auto url_contains = args.get_string("urlContains"sv).copy();
            auto method_filter = args.get_string("method"sv).copy();
            auto initiator_filter = args.get_string("initiatorType"sv).copy();

            JsonArray items;
            for (auto const& ev : m_server->service_network_events()) {
                if (ev.seq <= since_seq)
                    continue;
                if (filter_service.has_value() && !filter_service->is_empty() && ev.service_id != *filter_service)
                    continue;
                if (url_contains.has_value() && !url_contains->is_empty() && !ev.url.contains(*url_contains))
                    continue;
                if (method_filter.has_value() && !method_filter->is_empty() && !ev.method.equals_ignoring_ascii_case(*method_filter))
                    continue;
                if (initiator_filter.has_value() && !initiator_filter->is_empty() && (!ev.initiator_type.has_value() || !ev.initiator_type->equals_ignoring_ascii_case(*initiator_filter)))
                    continue;
                JsonObject obj;
                obj.set("seq"sv, ev.seq);
                obj.set("timestampMs"sv, ev.timestamp_ms);
                obj.set("serviceId"sv, ev.service_id);
                obj.set("tabId"sv, ev.tab_id);
                obj.set("requestId"sv, ev.request_id);
                obj.set("url"sv, ev.url);
                obj.set("method"sv, ev.method);
                if (ev.initiator_type.has_value())
                    obj.set("initiatorType"sv, *ev.initiator_type);
                else
                    obj.set("initiatorType"sv, JsonValue {});
                if (ev.status_code.has_value())
                    obj.set("statusCode"sv, *ev.status_code);
                else
                    obj.set("statusCode"sv, JsonValue {});
                obj.set("bodySize"sv, ev.body_size);
                if (ev.error.has_value())
                    obj.set("error"sv, *ev.error);
                else
                    obj.set("error"sv, JsonValue {});
                items.must_append(move(obj));
                if (items.size() >= limit)
                    break;
            }
            JsonObject result;
            result.set("requests"sv, move(items));
            result.set("lastSeq"sv, m_server->last_network_seq());
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "tabs.events"sv) {
            Optional<String> filter_service = args.get_string("serviceId"sv).copy();
            if (filter_service.has_value() && !filter_service->is_empty()) {
                if (!m_server->find_effective_service(*filter_service).has_value()) {
                    send_ipc_error(403, "Forbidden: Service is not declared in manifest"sv);
                    return;
                }
            }
            u64 since_seq = args.get_u64("sinceSeq"sv).value_or(0);
            size_t limit = static_cast<size_t>(min<u64>(args.get_u64("limit"sv).value_or(50), MAX_SERVICE_EVENTS));
            Vector<String> type_filters;
            if (auto types_arr = args.get_array("types"sv); types_arr.has_value()) {
                for (auto const& tv : types_arr->values()) {
                    if (tv.is_string() && !tv.as_string().is_empty())
                        type_filters.append(tv.as_string());
                }
            }

            JsonArray items;
            for (auto const& ev : m_server->service_events()) {
                if (ev.seq <= since_seq)
                    continue;
                if (filter_service.has_value() && !filter_service->is_empty() && ev.service_id != *filter_service)
                    continue;
                if (!type_filters.is_empty() && !type_filters.contains_slow(ev.type))
                    continue;
                auto ev_svc = m_server->find_effective_service(ev.service_id);
                if (ev.type == "notification"sv && (!ev_svc.has_value() || !ev_svc->allow_notifications))
                    continue;
                if (ev.type == "network"sv && (!ev_svc.has_value() || !ev_svc->allow_network))
                    continue;
                JsonObject obj;
                obj.set("seq"sv, ev.seq);
                obj.set("timestampMs"sv, ev.timestamp_ms);
                obj.set("type"sv, ev.type);
                obj.set("serviceId"sv, ev.service_id);
                if (ev.tab_id.has_value())
                    obj.set("tabId"sv, *ev.tab_id);
                else
                    obj.set("tabId"sv, JsonValue {});
                obj.set("data"sv, ev.data);
                items.must_append(move(obj));
                if (items.size() >= limit)
                    break;
            }
            JsonObject result;
            result.set("events"sv, move(items));
            result.set("lastSeq"sv, m_server->last_event_seq());
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "tabs.runFlow"sv) {
            auto service_id = args.get_string("serviceId"sv);
            if (!service_id.has_value() || service_id->is_empty()) {
                send_ipc_error(400, "Missing string argument 'serviceId'"sv);
                return;
            }
            auto svc = m_server->find_effective_service(*service_id);
            if (!svc.has_value() || !svc->allow_flows) {
                send_ipc_error(403, "Forbidden: 'flows' permission is disabled or service undeclared"sv);
                return;
            }
            Optional<String> flow_id = args.get_string("flowId"sv).copy();
            Optional<FlowDefinition> inline_flow;
            if (args.has("flow"sv) && !args.get("flow"sv)->is_null()) {
                auto parsed = parse_flow_definition(*args.get("flow"sv));
                if (parsed.is_error()) {
                    send_ipc_error(400, "Invalid inline 'flow' definition"sv);
                    return;
                }
                inline_flow = parsed.release_value();
            }
            if (!inline_flow.has_value() && (!flow_id.has_value() || flow_id->is_empty())) {
                send_ipc_error(400, "Either 'flowId' or 'flow' must be provided"sv);
                return;
            }
            HashMap<String, String> variables;
            if (args.has("variables"sv) && !args.get("variables"sv)->is_null()) {
                auto vars_obj = args.get_object("variables"sv);
                if (!vars_obj.has_value()) {
                    send_ipc_error(400, "'variables' must be an object"sv);
                    return;
                }
                vars_obj->for_each_member([&](String const& key, JsonValue const& val) {
                    if (val.is_string())
                        variables.set(key, val.as_string());
                    else if (val.is_number() || val.is_bool())
                        variables.set(key, val.serialized());
                });
            }
            HashTable<String> tainted_vars;
            if (auto tainted_arr = args.get_array("taintedVariables"sv); tainted_arr.has_value()) {
                for (auto const& tv : tainted_arr->values()) {
                    if (tv.is_string() && !tv.as_string().is_empty())
                        tainted_vars.set(tv.as_string());
                }
            }
            bool open_if_closed = args.get_bool("openIfClosed"sv).value_or(true);
            bool activate = args.get_bool("activate"sv).value_or(false);
            bool close_on_finish = args.get_bool("closeOnFinish"sv).value_or(false);
            bool wait_for_completion = args.get_bool("wait"sv).value_or(false);

            if (!m_server->m_on_service_run_flow) {
                send_ipc_error(501, "Service flow execution is unavailable in this runtime"sv);
                return;
            }
            NonnullRefPtr<Connection> self_ref { *this };
            m_server->m_on_service_run_flow(*service_id, move(flow_id), move(inline_flow), move(variables), move(tainted_vars), open_if_closed, activate, close_on_finish, wait_for_completion, [self_ref](Optional<FlowRunRecord> run, Optional<String> error) mutable {
                if (self_ref->m_closed)
                    return;
                if (error.has_value()) {
                    self_ref->send_ipc_error(400, *error);
                    return;
                }
                JsonObject result;
                result.set("run"sv, run.has_value() ? JsonValue { serialize_flow_run(*run) } : JsonValue {});
                self_ref->send_ipc_ok(move(result));
            });
            return;
        }

        if (*command == "flows.list"sv) {
            if (!m_server->m_on_flows_list) {
                send_ipc_error(501, "Flow store is unavailable in this runtime"sv);
                return;
            }
            JsonArray arr;
            for (auto const& flow : m_server->m_on_flows_list())
                arr.must_append(serialize_flow_summary(flow));
            JsonObject result;
            result.set("flows"sv, move(arr));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "flows.get"sv) {
            auto flow_id = args.get_string("flowId"sv);
            if (!flow_id.has_value() || !is_valid_flow_id(*flow_id)) {
                send_ipc_error(400, "Invalid or missing 'flowId'"sv);
                return;
            }
            if (!m_server->m_on_flows_get) {
                send_ipc_error(501, "Flow store is unavailable in this runtime"sv);
                return;
            }
            auto flow = m_server->m_on_flows_get(*flow_id);
            if (!flow.has_value()) {
                send_ipc_error(404, "Flow not found"sv);
                return;
            }
            JsonObject result;
            result.set("flow"sv, serialize_flow_definition(*flow));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "flows.save"sv) {
            auto flow_val = args.get("flow"sv);
            if (!flow_val.has_value() || !flow_val->is_object()) {
                send_ipc_error(400, "Missing object argument 'flow'"sv);
                return;
            }
            auto parsed = parse_flow_definition(*flow_val);
            if (parsed.is_error()) {
                send_ipc_error(400, "Invalid flow definition"sv);
                return;
            }
            if (!m_server->m_on_flows_save) {
                send_ipc_error(501, "Flow store is unavailable in this runtime"sv);
                return;
            }
            auto saved = m_server->m_on_flows_save(parsed.release_value());
            if (saved.is_error()) {
                send_ipc_error(500, "Failed saving flow"sv);
                return;
            }
            JsonObject result;
            result.set("flow"sv, serialize_flow_definition(saved.value()));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "flows.delete"sv) {
            auto flow_id = args.get_string("flowId"sv);
            if (!flow_id.has_value() || !is_valid_flow_id(*flow_id)) {
                send_ipc_error(400, "Invalid or missing 'flowId'"sv);
                return;
            }
            if (!m_server->m_on_flows_delete) {
                send_ipc_error(501, "Flow store is unavailable in this runtime"sv);
                return;
            }
            if (auto res = m_server->m_on_flows_delete(*flow_id); res.is_error()) {
                send_ipc_error(404, "Flow not found"sv);
                return;
            }
            JsonObject result;
            result.set("deleted"sv, true);
            result.set("flowId"sv, *flow_id);
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "flows.getRun"sv) {
            auto run_id = args.get_string("runId"sv);
            if (!run_id.has_value() || run_id->is_empty()) {
                send_ipc_error(400, "Missing string argument 'runId'"sv);
                return;
            }
            if (!m_server->m_on_flows_get_run) {
                send_ipc_error(501, "Flow runner is unavailable in this runtime"sv);
                return;
            }
            auto run = m_server->m_on_flows_get_run(*run_id);
            if (!run.has_value()) {
                send_ipc_error(404, "Flow run not found"sv);
                return;
            }
            JsonObject result;
            result.set("run"sv, serialize_flow_run(*run));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "flows.listRuns"sv) {
            if (!m_server->m_on_flows_list_runs) {
                send_ipc_error(501, "Flow runner is unavailable in this runtime"sv);
                return;
            }
            Optional<String> flow_id_storage = args.get_string("flowId"sv).copy();
            Optional<StringView> flow_id_filter;
            if (flow_id_storage.has_value() && !flow_id_storage->is_empty())
                flow_id_filter = flow_id_storage->bytes_as_string_view();
            size_t limit = static_cast<size_t>(min<u64>(args.get_u64("limit"sv).value_or(25), 100));
            JsonArray arr;
            for (auto const& run : m_server->m_on_flows_list_runs(flow_id_filter, limit))
                arr.must_append(serialize_flow_run(run));
            JsonObject result;
            result.set("runs"sv, move(arr));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "flows.cancelRun"sv) {
            auto run_id = args.get_string("runId"sv);
            if (!run_id.has_value() || run_id->is_empty()) {
                send_ipc_error(400, "Missing string argument 'runId'"sv);
                return;
            }
            if (!m_server->m_on_flows_cancel_run) {
                send_ipc_error(501, "Flow runner is unavailable in this runtime"sv);
                return;
            }
            bool cancelled = m_server->m_on_flows_cancel_run(*run_id);
            JsonObject result;
            result.set("cancelled"sv, cancelled);
            result.set("runId"sv, *run_id);
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "triggers.list"sv) {
            JsonArray arr;
            for (auto const& trig : m_server->triggers())
                arr.must_append(trigger_to_json(trig));
            JsonObject result;
            result.set("triggers"sv, move(arr));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "triggers.save"sv) {
            auto trig_val = args.get("trigger"sv);
            if (!trig_val.has_value() || !trig_val->is_object()) {
                send_ipc_error(400, "Missing object argument 'trigger'"sv);
                return;
            }
            auto parsed = parse_trigger_config(trig_val->as_object());
            if (parsed.is_error()) {
                send_ipc_error(400, parsed.error().string_literal());
                return;
            }
            auto cfg = parsed.release_value();
            auto svc = m_server->find_effective_service(cfg.service_id);
            if (!svc.has_value()) {
                send_ipc_error(403, "Forbidden: Trigger target service undeclared"sv);
                return;
            }
            if (!(cfg.type == "webhook"sv && cfg.client_handled) && !svc->allow_flows) {
                send_ipc_error(403, "Forbidden: Trigger target service requires 'flows' permission"sv);
                return;
            }
            if (cfg.type == "notification"sv && !svc->allow_notifications) {
                send_ipc_error(403, "Forbidden: Notification trigger requires 'notifications' permission"sv);
                return;
            }
            if (cfg.type == "network"sv && !svc->allow_network) {
                send_ipc_error(403, "Forbidden: Network trigger requires 'network' permission"sv);
                return;
            }
            auto saved = m_server->save_trigger(move(cfg));
            if (saved.is_error()) {
                send_ipc_error(400, saved.error().string_literal());
                return;
            }
            JsonObject result;
            result.set("trigger"sv, trigger_to_json(saved.value()));
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "triggers.delete"sv) {
            auto trig_id = args.get_string("id"sv);
            if (!trig_id.has_value() || trig_id->is_empty()) {
                send_ipc_error(400, "Missing string argument 'id'"sv);
                return;
            }
            bool deleted = m_server->delete_trigger(*trig_id);
            JsonObject result;
            result.set("deleted"sv, deleted);
            result.set("id"sv, *trig_id);
            send_ipc_ok(move(result));
            return;
        }

        if (*command == "triggers.setEnabled"sv) {
            auto trig_id = args.get_string("id"sv);
            auto enabled = args.get_bool("enabled"sv);
            if (!trig_id.has_value() || trig_id->is_empty() || !enabled.has_value()) {
                send_ipc_error(400, "Missing 'id' or 'enabled' argument"sv);
                return;
            }
            auto updated = m_server->set_trigger_enabled(*trig_id, *enabled);
            if (updated.is_error()) {
                send_ipc_error(404, "Trigger not found"sv);
                return;
            }
            JsonObject result;
            result.set("trigger"sv, trigger_to_json(updated.value()));
            send_ipc_ok(move(result));
            return;
        }

        send_ipc_error(400, "Unknown AppMode IPC command"sv);
    }

    void send_ipc_ok(JsonValue result)
    {
        JsonObject response;
        response.set("ok"sv, true);
        response.set("result"sv, move(result));
        auto serialized = response.serialized();
        send_raw_http_response(200, "application/json; charset=utf-8"sv, serialized.bytes());
    }

    void send_ipc_error(u32 status_code, StringView message)
    {
        JsonObject response;
        response.set("ok"sv, false);
        response.set("error"sv, message);
        auto serialized = response.serialized();
        send_raw_http_response(status_code, "application/json; charset=utf-8"sv, serialized.bytes());
    }

    void send_raw_http_response(u32 status_code, StringView content_type, ReadonlyBytes body, bool send_body = true, StringView extra_headers = {}, Optional<size_t> content_length_override = {})
    {
        if (m_closed)
            return;

        StringBuilder header_builder;
        header_builder.appendff("HTTP/1.1 {} {}\r\n", status_code, HTTP::reason_phrase_for_code(status_code));
        header_builder.append("Connection: close\r\n"sv);
        header_builder.append("Cache-Control: no-store\r\n"sv);
        header_builder.append("X-Content-Type-Options: nosniff\r\n"sv);
        header_builder.append("Cross-Origin-Resource-Policy: same-origin\r\n"sv);
        if (m_server) {
            auto const& perms = m_server->manifest().permissions;
            if (perms.network == AppNetworkPolicy::Offline) {
                header_builder.append("Content-Security-Policy: default-src 'self' data: blob:; script-src 'self' 'unsafe-inline' 'wasm-unsafe-eval' blob:; style-src 'self' 'unsafe-inline'; connect-src 'self'; img-src 'self' data: blob:; media-src 'self' data: blob:; font-src 'self' data:; worker-src 'self' blob:; frame-src 'self' blob:; form-action 'self'; object-src 'none'; base-uri 'self'\r\n"sv);
            }
            if (perms.cross_origin_isolated) {
                header_builder.append("Cross-Origin-Opener-Policy: same-origin\r\n"sv);
                header_builder.append("Cross-Origin-Embedder-Policy: require-corp\r\n"sv);
            }
        }
        header_builder.appendff("Content-Type: {}\r\n", content_type);
        header_builder.appendff("Content-Length: {}\r\n", content_length_override.value_or(body.size()));
        if (!extra_headers.is_empty())
            header_builder.append(extra_headers);
        header_builder.append("\r\n"sv);

        if (auto result = m_socket->write_until_depleted(header_builder.string_view().bytes()); result.is_error()) {
            warnln("Failed to write AppMode HTTP header: {}", result.error());
            close_later();
            return;
        }
        if (send_body && !body.is_empty()) {
            if (auto result = m_socket->write_until_depleted(body); result.is_error()) {
                warnln("Failed to write AppMode HTTP body: {}", result.error());
            }
        }
        close_later();
    }

    WeakPtr<AppModeServer> m_server;
    NonnullOwnPtr<Core::BufferedTCPSocket> m_socket;
    RefPtr<Core::Timer> m_read_timeout_timer;
    RefPtr<Core::Timer> m_sse_keepalive_timer;
    StringBuilder m_remaining_request;
    bool m_request_handled { false };
    bool m_is_sse_stream { false };
    bool m_closed { false };
    bool m_close_scheduled { false };
};

u16 AppModeServer::compute_stable_port(StringView app_id, StringView account_id)
{
    static constexpr u16 BASE_PORT = 42000;
    static constexpr u16 SPAN_PORTS = 17000;
    auto resolved_account = account_id.is_empty() ? "default"sv : account_id;
    auto input = ByteString::formatted("ladybird-app:{}:{}", app_id, resolved_account);
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, input.bytes());
    u16 raw = (static_cast<u16>(digest.bytes()[0]) << 8) | static_cast<u16>(digest.bytes()[1]);
    return static_cast<u16>(BASE_PORT + (raw % SPAN_PORTS));
}

ErrorOr<NonnullOwnPtr<AppModeServer>> AppModeServer::create(AppModeServerConfig config)
{
    TRY(Core::Directory::create(config.data_directory, Core::Directory::CreateDirectories::Yes));
    auto real_data_directory = TRY(FileSystem::real_path(config.data_directory));
    config.data_directory = real_data_directory;

    if (config.account_id.is_empty())
        config.account_id = "default"_string;
    if (config.account_label.is_empty())
        config.account_label = AppAccountRegistry::default_label_for_account(config.account_id);
    if (config.profile_name.is_empty())
        config.profile_name = AppManifest::profile_name_for_account(config.manifest.id, config.account_id);

    for (auto const& dir : config.manifest.permissions.fs_read) {
        auto full = TRY(AppManifest::resolve_confined_subpath(real_data_directory, dir, true));
        TRY(Core::Directory::create(full, Core::Directory::CreateDirectories::Yes));
    }
    for (auto const& dir : config.manifest.permissions.fs_write) {
        auto full = TRY(AppManifest::resolve_confined_subpath(real_data_directory, dir, true));
        TRY(Core::Directory::create(full, Core::Directory::CreateDirectories::Yes));
    }

    auto origin_port_file = LexicalPath::join(real_data_directory, "origin-port.txt"sv).string();
    Optional<u16> saved_port;
    bool has_saved_port_file = FileSystem::is_regular_file(origin_port_file);
    if (has_saved_port_file) {
        if (auto file = Core::File::open(origin_port_file, Core::File::OpenMode::Read); !file.is_error()) {
            if (auto bytes = file.value()->read_until_eof(); !bytes.is_error()) {
                auto trimmed = StringView { bytes.value() }.trim_whitespace();
                if (auto parsed = trimmed.to_number<u32>(); parsed.has_value() && *parsed >= 1024 && *parsed <= 65535)
                    saved_port = static_cast<u16>(*parsed);
            }
        }
    }

    u16 preferred_port = 0;
    if (saved_port.has_value()) {
        preferred_port = *saved_port;
    } else if (config.account_id == "default"sv && config.manifest.preferred_port.has_value()) {
        preferred_port = *config.manifest.preferred_port;
    } else {
        preferred_port = compute_stable_port(config.manifest.id, config.account_id);
    }

    auto server = TRY(Core::TCPServer::try_create());
    bool using_stable_port = false;
    if (preferred_port != 0) {
        if (auto bind_res = server->listen(IPv4Address { 127, 0, 0, 1 }, preferred_port, Core::TCPServer::AllowAddressReuse::Yes); !bind_res.is_error()) {
            using_stable_port = true;
        } else {
            warnln("AppModeServer could not bind preferred stable port {} ({}); falling back to ephemeral port", preferred_port, bind_res.error());
        }
    }

    if (!using_stable_port) {
        TRY(server->listen(IPv4Address { 127, 0, 0, 1 }, 0, Core::TCPServer::AllowAddressReuse::Yes));
    } else if (!has_saved_port_file) {
        if (auto bound = server->local_port(); bound.has_value() && *bound > 0) {
            if (auto port_out = Core::File::open(origin_port_file, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate); !port_out.is_error()) {
                auto serialized_port = ByteString::number(*bound);
                (void)port_out.value()->write_until_depleted(serialized_port.bytes());
            }
        }
    }

    OwnPtr<AppKVStore> kv_store;
    if (config.manifest.permissions.storage && !config.disable_sql_database) {
        kv_store = TRY(AppKVStore::create(real_data_directory));
    }

    Array<u8, 32> random_bytes;
    fill_with_random(random_bytes);
    auto session_token = TRY(String::from_byte_string(encode_hex(random_bytes)));

    return adopt_own(*new AppModeServer(
        move(config),
        move(server),
        using_stable_port,
        move(kv_store),
        move(session_token)));
}

AppModeServer::AppModeServer(
    AppModeServerConfig config,
    NonnullRefPtr<Core::TCPServer> server,
    bool using_stable_port,
    OwnPtr<AppKVStore> kv_store,
    String session_token)
    : m_manifest(move(config.manifest))
    , m_data_directory(move(config.data_directory))
    , m_account_id(move(config.account_id))
    , m_account_label(move(config.account_label))
    , m_profile_name(move(config.profile_name))
    , m_accounts_registry_path(move(config.accounts_registry_path))
    , m_kv_store(move(kv_store))
    , m_server(move(server))
    , m_using_stable_port(using_stable_port)
    , m_enable_webmcp_shim(config.enable_webmcp_shim)
    , m_session_token(move(session_token))
    , m_mcp_port_provider(move(config.mcp_port_provider))
    , m_on_quit_requested(move(config.on_quit_requested))
    , m_on_launch_account(move(config.on_launch_account))
    , m_on_service_open(move(config.on_service_open))
    , m_on_service_focus(move(config.on_service_focus))
    , m_on_service_navigate(move(config.on_service_navigate))
    , m_on_service_close(move(config.on_service_close))
    , m_on_service_list(move(config.on_service_list))
    , m_on_service_eval(move(config.on_service_eval))
    , m_on_service_read_markdown(move(config.on_service_read_markdown))
    , m_on_service_list_tools(move(config.on_service_list_tools))
    , m_on_service_call_tool(move(config.on_service_call_tool))
    , m_on_service_register_consent(move(config.on_service_register_consent))
    , m_on_flows_list(move(config.on_flows_list))
    , m_on_flows_get(move(config.on_flows_get))
    , m_on_flows_save(move(config.on_flows_save))
    , m_on_flows_delete(move(config.on_flows_delete))
    , m_on_flows_get_run(move(config.on_flows_get_run))
    , m_on_flows_list_runs(move(config.on_flows_list_runs))
    , m_on_flows_cancel_run(move(config.on_flows_cancel_run))
    , m_on_service_run_flow(move(config.on_service_run_flow))
{
    load_dynamic_services_from_disk();
    load_triggers_from_disk();
    sync_interval_timers();
    m_server->on_ready_to_accept = [weak_self = make_weak_ptr()] {
        if (!weak_self)
            return;
        if (auto result = weak_self->on_new_client(); result.is_error())
            warnln("Failed to accept AppMode client: {}", result.error());
    };
}

AppModeServer::~AppModeServer()
{
    m_is_shutting_down = true;
    for (auto const& [_, timer] : m_interval_timers) {
        if (timer)
            timer->stop();
    }
    m_interval_timers.clear();
    if (m_cron_timer) {
        m_cron_timer->stop();
        m_cron_timer.clear();
    }
    m_server->on_ready_to_accept = {};
    for (auto const& connection : m_connections)
        connection->close();
    m_connections.clear();
}

u16 AppModeServer::local_port() const
{
    return m_server->local_port().value_or(0);
}

String AppModeServer::origin() const
{
    return MUST(String::formatted("http://127.0.0.1:{}", local_port()));
}

URL::URL AppModeServer::entry_url() const
{
    auto parsed = URL::Parser::basic_parse(MUST(String::formatted("http://127.0.0.1:{}/", local_port())));
    VERIFY(parsed.has_value());
    return parsed.release_value();
}

Optional<u16> AppModeServer::current_mcp_port() const
{
    if (!m_mcp_port_provider)
        return {};
    return m_mcp_port_provider();
}

void AppModeServer::request_quit()
{
    if (!m_on_quit_requested)
        return;
    Core::deferred_invoke([weak_self = make_weak_ptr()] {
        if (weak_self && weak_self->m_on_quit_requested)
            weak_self->m_on_quit_requested();
    });
}

ErrorOr<void> AppModeServer::launch_account(StringView account_id)
{
    if (!m_on_launch_account)
        return Error::from_string_literal("Account launcher is unavailable");
    return m_on_launch_account(account_id);
}

void AppModeServer::append_service_event(StringView type, StringView service_id, Optional<u64> tab_id, JsonObject data, i64 timestamp_ms)
{
    while (m_service_events.size() >= MAX_SERVICE_EVENTS)
        m_service_events.take_first();
    m_service_events.append(AppServiceEvent {
        .seq = m_next_event_seq++,
        .timestamp_ms = timestamp_ms,
        .type = String::from_utf8_without_validation(type.bytes()),
        .service_id = String::from_utf8_without_validation(service_id.bytes()),
        .tab_id = tab_id,
        .data = move(data),
    });
    broadcast_sse_event("event"sv, service_event_to_json(m_service_events.last()));
}

void AppModeServer::push_service_notification(StringView origin, Optional<u64> tab_id, StringView event_type, StringView title, StringView body, StringView tag, i64 timestamp_ms)
{
    for (auto const& svc : effective_services()) {
        if (!svc.allow_notifications)
            continue;
        if (!AppManifest::is_service_origin_allowed(svc, origin))
            continue;
        while (m_service_notifications.size() >= MAX_SERVICE_NOTIFICATIONS)
            m_service_notifications.take_first();
        m_service_notifications.append(AppServiceNotification {
            .seq = m_next_notification_seq++,
            .timestamp_ms = timestamp_ms,
            .service_id = svc.id,
            .tab_id = tab_id,
            .origin = String::from_utf8_without_validation(origin.bytes()),
            .event_type = String::from_utf8_without_validation(event_type.bytes()),
            .title = String::from_utf8_without_validation(title.bytes()),
            .body = String::from_utf8_without_validation(body.bytes()),
            .tag = String::from_utf8_without_validation(tag.bytes()),
        });
        broadcast_sse_event("notification"sv, service_notification_to_json(m_service_notifications.last()));

        JsonObject ev_data;
        ev_data.set("origin"sv, origin);
        ev_data.set("eventType"sv, event_type);
        ev_data.set("title"sv, title);
        ev_data.set("body"sv, body);
        ev_data.set("tag"sv, tag);
        append_service_event("notification"sv, svc.id, tab_id, move(ev_data), timestamp_ms);

        if (event_type == "shown"sv) {
            HashMap<String, String> vars;
            vars.set("trigger_event"_string, "notification"_string);
            vars.set("trigger_service"_string, svc.id);
            vars.set("trigger_origin"_string, String::from_utf8_without_validation(origin.bytes()));
            vars.set("notification_title"_string, String::from_utf8_without_validation(title.bytes()));
            vars.set("notification_body"_string, String::from_utf8_without_validation(body.bytes()));
            vars.set("notification_tag"_string, String::from_utf8_without_validation(tag.bytes()));
            HashTable<String> tainted;
            tainted.set("trigger_origin"_string);
            tainted.set("notification_title"_string);
            tainted.set("notification_body"_string);
            tainted.set("notification_tag"_string);
            evaluate_event_triggers("notification"sv, svc.id, tab_id, origin, {}, {}, title, body, move(vars), move(tainted));
        }
    }
}

void AppModeServer::push_service_network_event(u64 tab_id, URL::URL const& tab_url, u64 request_id, StringView request_url, StringView method, Optional<StringView> initiator_type, Optional<u32> status_code, u64 body_size, Optional<StringView> error, i64 timestamp_ms)
{
    for (auto const& svc : effective_services()) {
        if (!svc.allow_network)
            continue;
        if (!AppManifest::is_service_origin_allowed(svc, tab_url))
            continue;
        while (m_service_network_events.size() >= MAX_SERVICE_NETWORK_EVENTS)
            m_service_network_events.take_first();
        AppServiceNetworkEvent net_ev;
        net_ev.seq = m_next_network_seq++;
        net_ev.timestamp_ms = timestamp_ms;
        net_ev.service_id = svc.id;
        net_ev.tab_id = tab_id;
        net_ev.request_id = request_id;
        net_ev.url = String::from_utf8_without_validation(request_url.bytes());
        net_ev.method = String::from_utf8_without_validation(method.bytes());
        if (initiator_type.has_value())
            net_ev.initiator_type = String::from_utf8_without_validation(initiator_type->bytes());
        net_ev.status_code = status_code;
        net_ev.body_size = body_size;
        if (error.has_value())
            net_ev.error = String::from_utf8_without_validation(error->bytes());
        m_service_network_events.append(move(net_ev));
        broadcast_sse_event("network"sv, service_network_event_to_json(m_service_network_events.last()));

        JsonObject ev_data;
        ev_data.set("requestId"sv, request_id);
        ev_data.set("url"sv, request_url);
        ev_data.set("method"sv, method);
        if (initiator_type.has_value())
            ev_data.set("initiatorType"sv, *initiator_type);
        else
            ev_data.set("initiatorType"sv, JsonValue {});
        if (status_code.has_value())
            ev_data.set("statusCode"sv, *status_code);
        else
            ev_data.set("statusCode"sv, JsonValue {});
        ev_data.set("bodySize"sv, body_size);
        if (error.has_value())
            ev_data.set("error"sv, *error);
        else
            ev_data.set("error"sv, JsonValue {});
        append_service_event("network"sv, svc.id, tab_id, move(ev_data), timestamp_ms);

        HashMap<String, String> vars;
        vars.set("trigger_event"_string, "network"_string);
        vars.set("trigger_service"_string, svc.id);
        vars.set("network_url"_string, String::from_utf8_without_validation(request_url.bytes()));
        vars.set("network_method"_string, String::from_utf8_without_validation(method.bytes()));
        vars.set("network_status"_string, status_code.has_value() ? String::number(*status_code) : String {});
        vars.set("network_body_size"_string, String::number(body_size));
        HashTable<String> tainted;
        tainted.set("network_url"_string);
        tainted.set("network_method"_string);
        evaluate_event_triggers("network"sv, svc.id, tab_id, request_url, method, status_code, {}, {}, move(vars), move(tainted));
    }
}

void AppModeServer::push_service_navigation_event(u64 tab_id, URL::URL const& url, StringView title, i64 timestamp_ms)
{
    auto url_str = url.to_string();
    for (auto const& svc : effective_services()) {
        if (!AppManifest::is_service_origin_allowed(svc, url))
            continue;
        JsonObject ev_data;
        ev_data.set("url"sv, url_str);
        ev_data.set("title"sv, title);
        append_service_event("navigation"sv, svc.id, tab_id, move(ev_data), timestamp_ms);

        HashMap<String, String> vars;
        vars.set("trigger_event"_string, "navigation"_string);
        vars.set("trigger_service"_string, svc.id);
        vars.set("navigation_url"_string, url_str);
        vars.set("navigation_title"_string, String::from_utf8_without_validation(title.bytes()));
        HashTable<String> tainted;
        tainted.set("navigation_url"_string);
        tainted.set("navigation_title"_string);
        evaluate_event_triggers("navigation"sv, svc.id, tab_id, url_str, {}, {}, title, {}, move(vars), move(tainted));
    }
}

Optional<AppTriggerConfig const&> AppModeServer::find_trigger(StringView id) const
{
    for (auto const& t : m_triggers) {
        if (t.id == id)
            return t;
    }
    return {};
}

ErrorOr<AppTriggerConfig> AppModeServer::save_trigger(AppTriggerConfig cfg)
{
    for (size_t i = 0; i < m_triggers.size(); ++i) {
        if (m_triggers[i].id == cfg.id) {
            cfg.last_fired_at_ms = m_triggers[i].last_fired_at_ms;
            cfg.last_run_id = m_triggers[i].last_run_id;
            cfg.fire_count = m_triggers[i].fire_count;
            if (cfg.type == "webhook"sv && (!cfg.webhook_secret.has_value() || cfg.webhook_secret->is_empty()))
                cfg.webhook_secret = m_triggers[i].webhook_secret;
            if (cfg.type == "webhook"sv && (!cfg.webhook_secret.has_value() || cfg.webhook_secret->is_empty())) {
                Array<u8, 16> raw;
                fill_with_random(raw);
                cfg.webhook_secret = MUST(String::from_byte_string(encode_hex(raw)));
            }
            m_triggers[i] = cfg;
            save_triggers_to_disk();
            sync_interval_timers();
            return cfg;
        }
    }
    if (m_triggers.size() >= MAX_SERVICE_TRIGGERS)
        return Error::from_string_literal("Maximum number of triggers reached");
    if (cfg.type == "webhook"sv && (!cfg.webhook_secret.has_value() || cfg.webhook_secret->is_empty())) {
        Array<u8, 16> raw;
        fill_with_random(raw);
        cfg.webhook_secret = MUST(String::from_byte_string(encode_hex(raw)));
    }
    m_triggers.append(cfg);
    save_triggers_to_disk();
    sync_interval_timers();
    return cfg;
}

AppModeServer::WebhookDispatchResult AppModeServer::receive_webhook(StringView trigger_id, StringView provided_token, StringView raw_body)
{
    AppTriggerConfig* target = nullptr;
    for (auto& t : m_triggers) {
        if (t.id == trigger_id && t.type == "webhook"sv && t.enabled) {
            target = &t;
            break;
        }
    }
    if (!target)
        return { 404, "Webhook trigger not found or disabled"_string, 0, {} };

    if (!target->webhook_secret.has_value() || target->webhook_secret->is_empty() || provided_token.is_empty()
        || !constant_time_equals(provided_token.bytes(), target->webhook_secret->bytes())) {
        return { 401, "Unauthorized: Invalid webhook token"_string, 0, {} };
    }

    auto now_ms = UnixDateTime::now().milliseconds_since_epoch();
    if (target->last_fired_at_ms > 0 && now_ms - target->last_fired_at_ms < static_cast<i64>(target->cooldown_ms))
        return { 429, "Too Many Requests: Trigger cooldown active"_string, 0, {} };

    JsonObject ev_data;
    ev_data.set("triggerId"sv, target->id);
    if (!raw_body.trim_whitespace().is_empty()) {
        if (auto parsed = JsonValue::from_string(raw_body); !parsed.is_error())
            ev_data.set("payload"sv, parsed.release_value());
        else
            ev_data.set("raw"sv, raw_body);
    } else {
        ev_data.set("payload"sv, JsonObject {});
    }

    append_service_event("webhook"sv, target->service_id, {}, move(ev_data), now_ms);
    u64 seq = last_event_seq();
    String id_copy = target->id;

    if (target->client_handled) {
        target->last_fired_at_ms = now_ms;
        target->fire_count++;
        save_triggers_to_disk();
        return { 202, "Accepted"_string, seq, move(id_copy) };
    }

    HashMap<String, String> vars;
    vars.set("trigger_event"_string, "webhook"_string);
    vars.set("trigger_service"_string, target->service_id);
    vars.set("webhook_body"_string, String::from_utf8_without_validation(raw_body.bytes()));
    HashTable<String> tainted;
    tainted.set("webhook_body"_string);
    fire_trigger(id_copy, move(vars), move(tainted));
    return { 202, "Accepted"_string, seq, move(id_copy) };
}

bool AppModeServer::delete_trigger(StringView id)
{
    bool removed = false;
    m_triggers.remove_all_matching([&](AppTriggerConfig const& t) {
        if (t.id == id) {
            removed = true;
            return true;
        }
        return false;
    });
    if (removed) {
        save_triggers_to_disk();
        sync_interval_timers();
    }
    return removed;
}

ErrorOr<AppTriggerConfig> AppModeServer::set_trigger_enabled(StringView id, bool enabled)
{
    for (auto& t : m_triggers) {
        if (t.id == id) {
            t.enabled = enabled;
            auto copy = t;
            save_triggers_to_disk();
            sync_interval_timers();
            return copy;
        }
    }
    return Error::from_string_literal("Trigger not found");
}

void AppModeServer::evaluate_event_triggers(StringView event_type, StringView service_id, Optional<u64>, StringView match_url, StringView match_method, Optional<u32> status_code, StringView match_title, StringView match_body, HashMap<String, String> trigger_vars, HashTable<String> tainted_keys)
{
    Vector<String> matching_ids;
    for (auto const& t : m_triggers) {
        if (!t.enabled || t.type != event_type || t.service_id != service_id)
            continue;
        if (t.url_contains.has_value() && !t.url_contains->is_empty() && !match_url.contains(*t.url_contains))
            continue;
        if (t.method.has_value() && !t.method->is_empty() && !match_method.equals_ignoring_ascii_case(*t.method))
            continue;
        if (t.status_code.has_value() && status_code != t.status_code)
            continue;
        if (t.title_contains.has_value() && !t.title_contains->is_empty() && !match_title.contains(*t.title_contains))
            continue;
        if (t.body_contains.has_value() && !t.body_contains->is_empty() && !match_body.contains(*t.body_contains))
            continue;
        matching_ids.append(t.id);
    }
    for (auto const& trig_id : matching_ids)
        fire_trigger(trig_id, trigger_vars, tainted_keys);
}

void AppModeServer::fire_trigger(String const& trigger_id, HashMap<String, String> trigger_vars, HashTable<String> tainted_keys)
{
    if (m_is_shutting_down || !m_on_service_run_flow)
        return;
    AppTriggerConfig* target = nullptr;
    for (auto& t : m_triggers) {
        if (t.id == trigger_id) {
            target = &t;
            break;
        }
    }
    if (!target || !target->enabled)
        return;

    auto svc = find_effective_service(target->service_id);
    if (!svc.has_value() || !svc->allow_flows)
        return;

    auto now_ms = UnixDateTime::now().milliseconds_since_epoch();
    if (target->last_fired_at_ms > 0 && now_ms - target->last_fired_at_ms < static_cast<i64>(target->cooldown_ms))
        return;

    target->last_fired_at_ms = now_ms;
    target->fire_count++;

    HashMap<String, String> merged_vars;
    for (auto const& [k, v] : target->variables)
        merged_vars.set(k, v);
    for (auto const& [k, v] : trigger_vars)
        merged_vars.set(k, v);
    merged_vars.set("trigger_id"_string, target->id);

    auto service_id = target->service_id;
    auto flow_id = target->flow_id;
    auto open_if_closed = target->open_if_closed;
    auto activate = target->activate;
    auto close_on_finish = target->close_on_finish;
    save_triggers_to_disk();

    m_on_service_run_flow(service_id, flow_id, {}, move(merged_vars), move(tainted_keys), open_if_closed, activate, close_on_finish, false, [weak_self = make_weak_ptr(), trigger_id](Optional<FlowRunRecord> run, Optional<String>) {
        if (!weak_self || !run.has_value())
            return;
        for (auto& trig : weak_self->m_triggers) {
            if (trig.id == trigger_id) {
                trig.last_run_id = run->run_id;
                weak_self->save_triggers_to_disk();
                break;
            }
        }
    });
}

bool AppModeServer::is_valid_cron_expression(StringView expr)
{
    Vector<StringView, 5> fields;
    if (!split_cron_expression(expr, fields))
        return false;
    return match_cron_field(fields[0], 0, 59, {})
        && match_cron_field(fields[1], 0, 23, {})
        && match_cron_field(fields[2], 1, 31, {})
        && match_cron_field(fields[3], 1, 12, {})
        && match_cron_field(fields[4], 0, 7, {}, true);
}

bool AppModeServer::cron_matches_local_time(StringView expr, int minute, int hour, int day_of_month, int month, int day_of_week)
{
    Vector<StringView, 5> fields;
    if (!split_cron_expression(expr, fields))
        return false;
    int normalized_dow = (day_of_week == 7) ? 0 : day_of_week;
    return match_cron_field(fields[0], 0, 59, minute)
        && match_cron_field(fields[1], 0, 23, hour)
        && match_cron_field(fields[2], 1, 31, day_of_month)
        && match_cron_field(fields[3], 1, 12, month)
        && match_cron_field(fields[4], 0, 7, normalized_dow, true);
}

void AppModeServer::evaluate_cron_at(int year, int month, int day, int hour, int minute, int dow, i64 now_ms)
{
    if (m_is_shutting_down)
        return;

    auto slot = MUST(String::formatted("{:04d}-{:02d}-{:02d} {:02d}:{:02d}", year, month, day, hour, minute));
    for (size_t i = 0; i < m_triggers.size(); ++i) {
        auto const& trig = m_triggers[i];
        if (!trig.enabled || trig.type != "cron"sv || !trig.cron_expr.has_value())
            continue;
        if (auto prev = m_cron_last_slot.get(trig.id); prev.has_value() && *prev == slot)
            continue;
        if (!cron_matches_local_time(*trig.cron_expr, minute, hour, day, month, dow))
            continue;

        m_cron_last_slot.set(trig.id, slot);
        auto trig_id = trig.id;
        auto service_id = trig.service_id;
        auto cron_expr = *trig.cron_expr;
        bool client_handled = trig.client_handled;

        JsonObject ev_data;
        ev_data.set("triggerId"sv, trig_id);
        ev_data.set("cronExpr"sv, cron_expr);
        ev_data.set("slot"sv, slot);
        append_service_event("cron"sv, service_id, {}, move(ev_data), now_ms);

        if (client_handled) {
            m_triggers[i].last_fired_at_ms = now_ms;
            m_triggers[i].fire_count++;
            save_triggers_to_disk();
        } else {
            HashMap<String, String> vars;
            vars.set("trigger_event"_string, "cron"_string);
            vars.set("cron_slot"_string, slot);
            vars.set("trigger_service"_string, service_id);
            fire_trigger(trig_id, move(vars), {});
        }
    }
}

void AppModeServer::tick_cron_triggers()
{
    if (m_is_shutting_down)
        return;
    auto now_ts = UnixDateTime::now();
    auto now_ms = now_ts.milliseconds_since_epoch();
    time_t sec = static_cast<time_t>(now_ms / 1000);
    struct tm local_tm {};
#if defined(AK_OS_WINDOWS)
    if (localtime_s(&local_tm, &sec) != 0)
        return;
#else
    if (!localtime_r(&sec, &local_tm))
        return;
#endif
    evaluate_cron_at(
        local_tm.tm_year + 1900,
        local_tm.tm_mon + 1,
        local_tm.tm_mday,
        local_tm.tm_hour,
        local_tm.tm_min,
        local_tm.tm_wday,
        now_ms);
}

void AppModeServer::sync_interval_timers()
{
    for (auto const& [_, timer] : m_interval_timers) {
        if (timer)
            timer->stop();
    }
    m_interval_timers.clear();
    if (m_cron_timer) {
        m_cron_timer->stop();
        m_cron_timer.clear();
    }

    if (m_is_shutting_down)
        return;

    bool has_cron = false;
    for (auto const& t : m_triggers) {
        if (!t.enabled)
            continue;
        if (t.type == "cron"sv && t.cron_expr.has_value()) {
            has_cron = true;
            continue;
        }
        if (t.type != "interval"sv)
            continue;
        auto interval_ms = max<i64>(5000, static_cast<i64>(t.interval_ms));
        auto timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(interval_ms), [weak_self = make_weak_ptr(), trigger_id = t.id, service_id = t.service_id] {
            if (!weak_self || weak_self->m_is_shutting_down)
                return;
            HashMap<String, String> vars;
            vars.set("trigger_event"_string, "interval"_string);
            vars.set("trigger_service"_string, service_id);
            weak_self->fire_trigger(trigger_id, move(vars), {});
        });
        timer->start();
        m_interval_timers.set(t.id, move(timer));
    }

    if (has_cron) {
        m_cron_timer = Core::Timer::create_repeating(AK::Duration::from_seconds(15), [weak_self = make_weak_ptr()] {
            if (!weak_self || weak_self->m_is_shutting_down)
                return;
            weak_self->tick_cron_triggers();
        });
        m_cron_timer->start();
    }
}

void AppModeServer::request_service_register_consent(AppServiceConfig const& service, Function<void(bool approved)> on_decision)
{
    if (!m_on_service_register_consent) {
        on_decision(true);
        return;
    }
    m_on_service_register_consent(service, move(on_decision));
}

void AppModeServer::load_triggers_from_disk()
{
    m_triggers.clear();
    if (m_data_directory.is_empty())
        return;
    auto triggers_path = LexicalPath::join(m_data_directory, "triggers.json"sv).string();
    if (!FileSystem::is_regular_file(triggers_path))
        return;
    auto file = Core::File::open(triggers_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return;
    auto bytes = file.value()->read_until_eof();
    if (bytes.is_error())
        return;
    auto json = JsonValue::from_string(bytes.value());
    if (json.is_error() || !json.value().is_array())
        return;
    for (auto const& item : json.value().as_array().values()) {
        if (!item.is_object())
            continue;
        auto parsed = parse_trigger_config(item.as_object());
        if (parsed.is_error())
            continue;
        if (m_triggers.size() < MAX_SERVICE_TRIGGERS)
            m_triggers.append(parsed.release_value());
    }
}

void AppModeServer::save_triggers_to_disk() const
{
    if (m_data_directory.is_empty())
        return;
    JsonArray arr;
    for (auto const& trig : m_triggers)
        arr.must_append(trigger_to_json(trig));
    auto serialized = arr.serialized();
    auto triggers_path = LexicalPath::join(m_data_directory, "triggers.json"sv).string();
    if (auto file = Core::File::open(triggers_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate); !file.is_error())
        (void)file.value()->write_until_depleted(serialized.bytes());
}

Vector<AppServiceConfig> AppModeServer::effective_services() const
{
    Vector<AppServiceConfig> all;
    all.ensure_capacity(m_manifest.services.size() + m_dynamic_services.size());
    for (auto const& svc : m_manifest.services)
        all.append(svc);
    for (auto const& svc : m_dynamic_services)
        all.append(svc);
    return all;
}

Optional<AppServiceConfig> AppModeServer::find_effective_service(StringView service_id) const
{
    if (auto builtin = m_manifest.find_service(service_id); builtin.has_value())
        return *builtin;
    for (auto const& svc : m_dynamic_services) {
        if (svc.id == service_id)
            return svc;
    }
    return {};
}

bool AppModeServer::is_dynamic_service(StringView service_id) const
{
    for (auto const& svc : m_dynamic_services) {
        if (svc.id == service_id)
            return true;
    }
    return false;
}

ErrorOr<AppServiceConfig> AppModeServer::register_dynamic_service(AppServiceConfig cfg)
{
    if (!m_manifest.permissions.allow_dynamic_services)
        return Error::from_string_literal("Dynamic services are disabled in manifest");
    if (m_manifest.find_service(cfg.id).has_value())
        return Error::from_string_literal("Cannot override a manifest-declared service");

    for (size_t i = 0; i < m_dynamic_services.size(); ++i) {
        if (m_dynamic_services[i].id == cfg.id) {
            m_dynamic_services[i] = cfg;
            save_dynamic_services_to_disk();
            return cfg;
        }
    }
    if (m_dynamic_services.size() >= MAX_DYNAMIC_SERVICES)
        return Error::from_string_literal("Maximum number of dynamic services reached");
    m_dynamic_services.append(cfg);
    save_dynamic_services_to_disk();
    return cfg;
}

bool AppModeServer::remove_dynamic_service(StringView service_id)
{
    if (m_manifest.find_service(service_id).has_value())
        return false;
    bool removed = false;
    m_dynamic_services.remove_all_matching([&](AppServiceConfig const& svc) {
        if (svc.id == service_id) {
            removed = true;
            return true;
        }
        return false;
    });
    if (removed)
        save_dynamic_services_to_disk();
    return removed;
}

void AppModeServer::load_dynamic_services_from_disk()
{
    m_dynamic_services.clear();
    if (!m_manifest.permissions.allow_dynamic_services || m_data_directory.is_empty())
        return;
    auto services_path = LexicalPath::join(m_data_directory, "services.json"sv).string();
    if (!FileSystem::is_regular_file(services_path))
        return;
    auto file = Core::File::open(services_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return;
    auto bytes = file.value()->read_until_eof();
    if (bytes.is_error())
        return;
    auto json = JsonValue::from_string(bytes.value());
    if (json.is_error() || !json.value().is_array())
        return;
    for (auto const& item : json.value().as_array().values()) {
        if (!item.is_object())
            continue;
        auto const& obj = item.as_object();
        auto id = obj.get_string("id"sv);
        if (!id.has_value() || m_manifest.find_service(*id).has_value())
            continue;
        auto parsed = AppManifest::parse_service_config(*id, obj);
        if (parsed.is_error())
            continue;
        if (m_dynamic_services.size() < MAX_DYNAMIC_SERVICES)
            m_dynamic_services.append(parsed.release_value());
    }
}

void AppModeServer::save_dynamic_services_to_disk() const
{
    if (m_data_directory.is_empty())
        return;
    JsonArray arr;
    for (auto const& svc : m_dynamic_services)
        arr.must_append(AppManifest::serialize_service_config(svc));
    auto serialized = arr.serialized();
    auto services_path = LexicalPath::join(m_data_directory, "services.json"sv).string();
    if (auto file = Core::File::open(services_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate); !file.is_error())
        (void)file.value()->write_until_depleted(serialized.bytes());
}

ErrorOr<AppServiceTabState> AppModeServer::open_service_tab(StringView service_id, Optional<URL::URL> url, bool activate, bool reuse, Optional<String> container_override)
{
    if (!m_on_service_open)
        return Error::from_string_literal("Service tabs are unavailable in this runtime");
    return m_on_service_open(service_id, move(url), activate, reuse, move(container_override));
}

ErrorOr<AppServiceTabState> AppModeServer::focus_service_tab(StringView service_id)
{
    if (!m_on_service_focus)
        return Error::from_string_literal("Service tabs are unavailable in this runtime");
    return m_on_service_focus(service_id);
}

ErrorOr<AppServiceTabState> AppModeServer::navigate_service_tab(StringView service_id, URL::URL url)
{
    if (!m_on_service_navigate)
        return Error::from_string_literal("Service tabs are unavailable in this runtime");
    return m_on_service_navigate(service_id, move(url));
}

ErrorOr<bool> AppModeServer::close_service_tab(StringView service_id)
{
    if (!m_on_service_close)
        return Error::from_string_literal("Service tabs are unavailable in this runtime");
    return m_on_service_close(service_id);
}

Vector<AppServiceTabState> AppModeServer::list_service_tabs() const
{
    if (!m_on_service_list)
        return {};
    return m_on_service_list();
}

void AppModeServer::eval_service_tab(StringView service_id, String script, Function<void(Optional<JsonValue> value, Optional<String> error)> on_complete)
{
    if (!m_on_service_eval) {
        on_complete({}, "Service tab evaluation is unavailable in this runtime"_string);
        return;
    }
    m_on_service_eval(service_id, move(script), move(on_complete));
}

void AppModeServer::read_service_tab_markdown(StringView service_id, Optional<String> root_selector, bool main_content_only, bool include_links, bool include_images, size_t max_chars, Function<void(Optional<JsonValue> value, Optional<String> error)> on_complete)
{
    if (!m_on_service_read_markdown) {
        on_complete({}, "Service tab markdown reader is unavailable in this runtime"_string);
        return;
    }
    m_on_service_read_markdown(service_id, move(root_selector), main_content_only, include_links, include_images, max_chars, move(on_complete));
}

void AppModeServer::list_service_tab_tools(StringView service_id, bool include_declarative, Function<void(Optional<JsonValue> value, Optional<String> error)> on_complete)
{
    if (!m_on_service_list_tools) {
        on_complete({}, "Service tab WebMCP discovery is unavailable in this runtime"_string);
        return;
    }
    m_on_service_list_tools(service_id, include_declarative, move(on_complete));
}

void AppModeServer::call_service_tab_tool(StringView service_id, String tool_name, JsonObject arguments, Function<void(Optional<JsonValue> value, Optional<String> error)> on_complete)
{
    if (!m_on_service_call_tool) {
        on_complete({}, "Service tab WebMCP invocation is unavailable in this runtime"_string);
        return;
    }
    m_on_service_call_tool(service_id, move(tool_name), move(arguments), move(on_complete));
}

ErrorOr<void> AppModeServer::on_new_client()
{
    auto client = TRY(m_server->accept());
    if (m_connections.size() >= MAX_ACTIVE_APP_CONNECTIONS) {
        (void)client->write_until_depleted("HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\nContent-Length: 24\r\n\r\nToo many app connections"sv.bytes());
        client->close();
        return {};
    }
    auto connection = TRY(Connection::create(*this, move(client)));
    m_connections.set(move(connection));
    return {};
}

void AppModeServer::remove_connection(Connection& connection)
{
    if (m_is_shutting_down)
        return;
    m_connections.remove_all_matching([&](auto const& entry) {
        return entry.ptr() == &connection;
    });
}

size_t AppModeServer::sse_client_count() const
{
    size_t count = 0;
    for (auto const& conn : m_connections) {
        if (conn->is_sse_stream())
            ++count;
    }
    return count;
}

void AppModeServer::broadcast_sse_event(StringView event_name, JsonObject const& payload)
{
    if (m_is_shutting_down)
        return;

    Vector<NonnullRefPtr<Connection>> sse_clients;
    for (auto const& conn : m_connections) {
        if (conn->is_sse_stream())
            sse_clients.append(conn);
    }
    if (sse_clients.is_empty())
        return;

    auto serialized = payload.serialized();
    for (auto const& conn : sse_clients)
        conn->send_sse_event(event_name, serialized.bytes_as_string_view());
}

String AppModeServer::build_bridge_script() const
{
    JsonObject perms;
    JsonArray fs_read;
    for (auto const& d : m_manifest.permissions.fs_read)
        fs_read.must_append(d);
    JsonArray fs_write;
    for (auto const& d : m_manifest.permissions.fs_write)
        fs_write.must_append(d);
    perms.set("fsRead"sv, move(fs_read));
    perms.set("fsWrite"sv, move(fs_write));
    perms.set("storage"sv, m_manifest.permissions.storage);
    perms.set("clipboardWrite"sv, m_manifest.permissions.clipboard_write);
    perms.set("systemInfo"sv, m_manifest.permissions.system_info);
    perms.set("dynamicServices"sv, m_manifest.permissions.allow_dynamic_services);
    perms.set("network"sv, network_policy_name(m_manifest.permissions.network));
    perms.set("crossOriginIsolated"sv, m_manifest.permissions.cross_origin_isolated);

    JsonObject account_obj;
    account_obj.set("id"sv, m_account_id);
    account_obj.set("label"sv, m_account_label);
    account_obj.set("profileName"sv, m_profile_name);

    JsonObject info;
    info.set("id"sv, m_manifest.id);
    info.set("name"sv, m_manifest.name);
    info.set("version"sv, m_manifest.version);
    info.set("origin"sv, origin());
    info.set("port"sv, static_cast<u64>(local_port()));
    info.set("stablePort"sv, m_using_stable_port);
    info.set("mcpAccess"sv, mcp_access_mode_name(m_manifest.mcp.access));
    info.set("account"sv, move(account_obj));
    info.set("permissions"sv, move(perms));
    JsonArray services_meta;
    for (auto const& svc : effective_services())
        services_meta.must_append(service_tab_state_to_json(svc, {}, is_dynamic_service(svc.id)));
    info.set("services"sv, move(services_meta));

    StringBuilder sb;
    sb.append("(() => {\n"sv);
    sb.append("  'use strict';\n"sv);
    sb.appendff("  const TOKEN = {};\n", JsonValue(m_session_token).serialized());
    sb.appendff("  const APP_INFO = Object.freeze({});\n", info.serialized());
    sb.append(R"JS(
  async function invoke(command, args) {
    const resp = await fetch('/__ladybird_ipc', {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        'X-Ladybird-App-Token': TOKEN
      },
      body: JSON.stringify({ command, args: args || {} })
    });
    const payload = await resp.json();
    if (!resp.ok || !payload || !payload.ok) {
      throw new Error((payload && payload.error) || ('App IPC failed with HTTP ' + resp.status));
    }
    return payload.result;
  }

  function encodeFsPath(relPath) {
    return String(relPath || '')
      .split('/')
      .filter((s) => s.length > 0 && s !== '.')
      .map((s) => encodeURIComponent(s))
      .join('/');
  }

  async function readBytes(relPath, options) {
    const encoded = encodeFsPath(relPath);
    const headers = { 'X-Ladybird-App-Token': TOKEN };
    if (options && typeof options.offset === 'number' && typeof options.count === 'number' && options.count > 0) {
      const start = Math.max(0, Math.floor(options.offset));
      const end = start + Math.floor(options.count) - 1;
      headers['Range'] = 'bytes=' + start + '-' + end;
    }
    const resp = await fetch('/__ladybird_fs/' + encoded, { method: 'GET', headers });
    if (!resp.ok) {
      let msg = 'App FS readBytes failed with HTTP ' + resp.status;
      try {
        const errJson = await resp.json();
        if (errJson && errJson.error) msg = errJson.error;
      } catch (e) {}
      throw new Error(msg);
    }
    return resp.arrayBuffer();
  }

  async function writeBytes(relPath, data) {
    const encoded = encodeFsPath(relPath);
    const body = data instanceof Blob || data instanceof ArrayBuffer || ArrayBuffer.isView(data) ? data : String(data ?? '');
    const resp = await fetch('/__ladybird_fs/' + encoded, {
      method: 'PUT',
      headers: { 'X-Ladybird-App-Token': TOKEN },
      body
    });
    const payload = await resp.json();
    if (!resp.ok || !payload || !payload.ok) {
      throw new Error((payload && payload.error) || ('App FS writeBytes failed with HTTP ' + resp.status));
    }
    return payload.result;
  }

  function fileUrl(relPath) {
    return '/__ladybird_fs/' + encodeFsPath(relPath) + '?token=' + encodeURIComponent(TOKEN);
  }

  const sseChannels = {
    notification: new Set(),
    network: new Set(),
    event: new Set()
  };
  let sseSource = null;
  let sseConnected = false;

  function totalSseListeners() {
    return sseChannels.notification.size + sseChannels.network.size + sseChannels.event.size;
  }

  function syncSseConnection() {
    if (totalSseListeners() === 0) {
      if (sseSource) {
        try { sseSource.close(); } catch (e) {}
        sseSource = null;
      }
      sseConnected = false;
      return;
    }
    if (sseSource || typeof EventSource !== 'function')
      return;
    sseSource = new EventSource('/__ladybird_events?token=' + encodeURIComponent(TOKEN));
    sseSource.onopen = () => { sseConnected = true; };
    sseSource.onerror = () => { sseConnected = false; };
    const dispatch = (kind) => (msg) => {
      sseConnected = true;
      let payload;
      try { payload = JSON.parse(msg.data); } catch (e) { return; }
      for (const listener of sseChannels[kind]) {
        try { listener(payload); } catch (e) {}
      }
    };
    sseSource.addEventListener('notification', dispatch('notification'));
    sseSource.addEventListener('network', dispatch('network'));
    sseSource.addEventListener('event', dispatch('event'));
  }

  function subscribeSseStream(kind, ipcCommand, itemsKey, buildArgs, matchesItem, serviceIdOrCb, maybeCb, maybeOpts, errName) {
    let serviceId = serviceIdOrCb;
    let cb = maybeCb;
    let opts = maybeOpts || {};
    if (typeof serviceIdOrCb === 'function') {
      cb = serviceIdOrCb;
      serviceId = null;
      opts = maybeCb || {};
    }
    if (typeof cb !== 'function') throw new Error(errName + ' requires a callback');

    let stopped = false;
    let lastSeq = opts.replay ? 0 : null;
    const intervalMs = Math.max(400, Number(opts.intervalMs) || 1500);

    const deliver = (item) => {
      if (stopped || !item) return;
      if (typeof item.seq === 'number') {
        if (lastSeq !== null && item.seq <= lastSeq) return;
        lastSeq = Math.max(lastSeq || 0, item.seq);
      }
      if (serviceId && item.serviceId !== serviceId) return;
      if (matchesItem && !matchesItem(item, opts)) return;
      try { cb(item); } catch (e) {}
    };

    sseChannels[kind].add(deliver);
    syncSseConnection();

    const poll = async (forceInit) => {
      if (stopped) return;
      if (forceInit || !sseConnected || lastSeq === null) {
        try {
          if (lastSeq === null) {
            const initRes = await invoke(ipcCommand, serviceId ? { serviceId, limit: 1 } : { limit: 1 });
            if (lastSeq === null)
              lastSeq = (initRes && initRes.lastSeq) || 0;
          } else {
            const args = buildArgs(serviceId, lastSeq, opts);
            const res = await invoke(ipcCommand, args);
            if (res && typeof res.lastSeq === 'number' && res.lastSeq > (lastSeq || 0))
              lastSeq = res.lastSeq;
            for (const item of (res && res[itemsKey]) || []) {
              if (stopped) break;
              deliver(item);
            }
          }
        } catch (e) {}
      }
      if (!stopped) setTimeout(() => poll(false), intervalMs);
    };

    setTimeout(() => poll(true), 25);
    return () => {
      stopped = true;
      sseChannels[kind].delete(deliver);
      syncSseConnection();
    };
  }

  function matchesNetworkOpts(r, opts) {
    const f = opts && opts.filter;
    if (!f) return true;
    if (f.urlContains && !String(r.url || '').includes(f.urlContains)) return false;
    if (f.method && String(r.method || '').toUpperCase() !== String(f.method).toUpperCase()) return false;
    if (f.initiatorType && String(r.initiatorType || '').toLowerCase() !== String(f.initiatorType).toLowerCase()) return false;
    return true;
  }

  function matchesEventOpts(ev, opts) {
    if (Array.isArray(opts && opts.types) && opts.types.length > 0)
      return opts.types.includes(ev.type);
    return true;
  }

  const bridge = Object.freeze({
    info: APP_INFO,
    invoke,
    system: Object.freeze({
      info: () => invoke('system.info', {})
    }),
    fs: Object.freeze({
      listDir: (path) => invoke('fs.listDir', { path }),
      readText: (path) => invoke('fs.readText', { path }),
      writeText: (path, content) => invoke('fs.writeText', { path, content }),
      deleteFile: (path) => invoke('fs.deleteFile', { path }),
      stat: (path) => invoke('fs.stat', { path }),
      mkdir: (path) => invoke('fs.mkdir', { path }),
      readBytes,
      writeBytes,
      fileUrl
    }),
    kv: Object.freeze({
      get: (key) => invoke('kv.get', { key }),
      set: (key, value) => invoke('kv.set', { key, value }),
      delete: (key) => invoke('kv.delete', { key }),
      list: (prefix) => invoke('kv.list', prefix === undefined ? {} : { prefix }),
      clear: (prefix) => invoke('kv.clear', prefix === undefined ? {} : { prefix })
    }),
    accounts: Object.freeze({
      current: () => invoke('accounts.current', {}),
      list: () => invoke('accounts.list', {}),
      create: (id, label) => invoke('accounts.create', { id, label }),
      launch: (id, options) => invoke('accounts.launch', Object.assign({ id }, options || {}))
    }),
    tabs: Object.freeze({
      list: () => invoke('tabs.list', {}),
      open: (serviceId, options) => invoke('tabs.open', Object.assign({ serviceId }, options || {})),
      focus: (serviceId) => invoke('tabs.focus', { serviceId }),
      navigate: (serviceId, url) => invoke('tabs.navigate', { serviceId, url }),
      close: (serviceId) => invoke('tabs.close', { serviceId }),
      registerService: (service) => invoke('tabs.registerService', { service }),
      removeService: (serviceId) => invoke('tabs.removeService', { serviceId }),
      notifications: (options) => invoke('tabs.notifications', options || {}),
      network: (options) => invoke('tabs.network', options || {}),
      events: (options) => invoke('tabs.events', options || {}),
      readMarkdown: (serviceId, options) => invoke('tabs.readMarkdown', Object.assign({ serviceId }, options || {})),
      listTools: (serviceId, options) => invoke('tabs.listTools', Object.assign({ serviceId }, options || {})),
      callTool: (serviceId, toolName, args) => invoke('tabs.callTool', { serviceId, toolName, arguments: args || {} }),
      eval: (serviceId, script) => invoke('tabs.eval', { serviceId, script }),
      runFlow: (serviceId, options) => invoke('tabs.runFlow', Object.assign({ serviceId }, options || {})),
      onNotification: (serviceIdOrCb, maybeCb, maybeOpts) => subscribeSseStream(
        'notification',
        'tabs.notifications',
        'notifications',
        (serviceId, lastSeq) => serviceId ? { serviceId, sinceSeq: lastSeq, limit: 50 } : { sinceSeq: lastSeq, limit: 50 },
        null,
        serviceIdOrCb,
        maybeCb,
        maybeOpts,
        'tabs.onNotification'
      ),
      onNetwork: (serviceIdOrCb, maybeCb, maybeOpts) => subscribeSseStream(
        'network',
        'tabs.network',
        'requests',
        (serviceId, lastSeq, opts) => {
          const args = Object.assign({}, (opts && opts.filter) || {}, { sinceSeq: lastSeq, limit: 50 });
          if (serviceId) args.serviceId = serviceId;
          return args;
        },
        matchesNetworkOpts,
        serviceIdOrCb,
        maybeCb,
        maybeOpts,
        'tabs.onNetwork'
      ),
      onEvent: (serviceIdOrCb, maybeCb, maybeOpts) => subscribeSseStream(
        'event',
        'tabs.events',
        'events',
        (serviceId, lastSeq, opts) => {
          const args = { sinceSeq: lastSeq, limit: 50 };
          if (serviceId) args.serviceId = serviceId;
          if (Array.isArray(opts && opts.types)) args.types = opts.types;
          return args;
        },
        matchesEventOpts,
        serviceIdOrCb,
        maybeCb,
        maybeOpts,
        'tabs.onEvent'
      )
    }),
    flows: Object.freeze({
      list: () => invoke('flows.list', {}),
      get: (flowId) => invoke('flows.get', { flowId }),
      save: (flow) => invoke('flows.save', { flow }),
      delete: (flowId) => invoke('flows.delete', { flowId }),
      getRun: (runId) => invoke('flows.getRun', { runId }),
      listRuns: (options) => invoke('flows.listRuns', options || {}),
      cancelRun: (runId) => invoke('flows.cancelRun', { runId })
    }),
    triggers: Object.freeze({
      list: () => invoke('triggers.list', {}),
      save: (trigger) => invoke('triggers.save', { trigger }),
      delete: (id) => invoke('triggers.delete', { id }),
      setEnabled: (id, enabled) => invoke('triggers.setEnabled', { id, enabled })
    }),
    app: Object.freeze({
      quit: () => invoke('app.quit', {})
    })
  });

  Object.defineProperty(window, 'ladybirdApp', {
    value: bridge,
    writable: false,
    configurable: false,
    enumerable: true
  });
})();
)JS"sv);
    if (m_enable_webmcp_shim && m_manifest.mcp.enabled && m_manifest.mcp.allow_webmcp) {
        sb.append('\n');
        sb.append(webmcp_shim_javascript());
    }
    return sb.to_string_without_validation();
}

}
