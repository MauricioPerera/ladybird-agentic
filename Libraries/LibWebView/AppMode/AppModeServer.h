/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibCore/Forward.h>
#include <LibCore/Timer.h>
#include <LibURL/URL.h>
#include <LibWebView/AppMode/AppKVStore.h>
#include <LibWebView/AppMode/AppManifest.h>
#include <LibWebView/Automation/FlowTypes.h>

namespace WebView {

struct WEBVIEW_API AppServiceTabState {
    String service_id;
    u64 tab_id { 0 };
    bool open { false };
    String url;
    String title;
    String container {};
    bool origin_allowed { false };
};

struct WEBVIEW_API AppServiceNotification {
    u64 seq { 0 };
    i64 timestamp_ms { 0 };
    String service_id;
    Optional<u64> tab_id {};
    String origin;
    String event_type;
    String title;
    String body;
    String tag;
};

struct WEBVIEW_API AppServiceNetworkEvent {
    u64 seq { 0 };
    i64 timestamp_ms { 0 };
    String service_id;
    u64 tab_id { 0 };
    u64 request_id { 0 };
    String url;
    String method;
    Optional<String> initiator_type {};
    Optional<u32> status_code {};
    u64 body_size { 0 };
    Optional<String> error {};
};

struct WEBVIEW_API AppServiceEvent {
    u64 seq { 0 };
    i64 timestamp_ms { 0 };
    String type;
    String service_id;
    Optional<u64> tab_id {};
    JsonObject data;
};

struct WEBVIEW_API AppTriggerConfig {
    String id;
    String name;
    bool enabled { true };
    String type;
    String service_id;
    String flow_id;
    u32 interval_ms { 60000 };
    u32 cooldown_ms { 3000 };
    Optional<String> url_contains {};
    Optional<String> method {};
    Optional<u32> status_code {};
    Optional<String> title_contains {};
    Optional<String> body_contains {};
    bool open_if_closed { false };
    bool activate { false };
    bool close_on_finish { false };
    bool client_handled { false };
    Optional<String> webhook_secret {};
    Optional<String> cron_expr {};
    HashMap<String, String> variables {};
    i64 last_fired_at_ms { 0 };
    Optional<String> last_run_id {};
    u64 fire_count { 0 };
};

struct WEBVIEW_API AppModeServerConfig {
    AppManifest manifest;
    ByteString data_directory;
    String account_id { "default"_string };
    String account_label { "Default"_string };
    String profile_name {};
    Optional<ByteString> accounts_registry_path {};
    bool disable_sql_database { false };
    bool enable_webmcp_shim { true };
    Function<Optional<u16>()> mcp_port_provider {};
    Function<void()> on_quit_requested {};
    Function<ErrorOr<void>(StringView account_id)> on_launch_account {};
    Function<ErrorOr<AppServiceTabState>(StringView service_id, Optional<URL::URL> url, bool activate, bool reuse, Optional<String> container_override)> on_service_open {};
    Function<ErrorOr<AppServiceTabState>(StringView service_id)> on_service_focus {};
    Function<ErrorOr<AppServiceTabState>(StringView service_id, URL::URL url)> on_service_navigate {};
    Function<ErrorOr<bool>(StringView service_id)> on_service_close {};
    Function<Vector<AppServiceTabState>()> on_service_list {};
    Function<void(StringView service_id, String script, Function<void(Optional<JsonValue> value, Optional<String> error)> on_complete)> on_service_eval {};
    Function<void(StringView service_id, Optional<String> root_selector, bool main_content_only, bool include_links, bool include_images, size_t max_chars, Function<void(Optional<JsonValue> value, Optional<String> error)> on_complete)> on_service_read_markdown {};
    Function<void(StringView service_id, bool include_declarative, Function<void(Optional<JsonValue> value, Optional<String> error)> on_complete)> on_service_list_tools {};
    Function<void(StringView service_id, String tool_name, JsonObject arguments, Function<void(Optional<JsonValue> value, Optional<String> error)> on_complete)> on_service_call_tool {};
    Function<void(AppServiceConfig const& service, Function<void(bool approved)> on_decision)> on_service_register_consent {};
    Function<Vector<FlowDefinition>()> on_flows_list {};
    Function<Optional<FlowDefinition>(StringView flow_id)> on_flows_get {};
    Function<ErrorOr<FlowDefinition>(FlowDefinition)> on_flows_save {};
    Function<ErrorOr<void>(StringView flow_id)> on_flows_delete {};
    Function<Optional<FlowRunRecord>(StringView run_id)> on_flows_get_run {};
    Function<Vector<FlowRunRecord>(Optional<StringView> flow_id, size_t limit)> on_flows_list_runs {};
    Function<bool(StringView run_id)> on_flows_cancel_run {};
    Function<void(StringView service_id, Optional<String> flow_id, Optional<FlowDefinition> inline_flow, HashMap<String, String> variables, HashTable<String> tainted_variables, bool open_if_closed, bool activate, bool close_on_finish, bool wait_for_completion, Function<void(Optional<FlowRunRecord> run, Optional<String> error)> on_done)> on_service_run_flow {};
};

class WEBVIEW_API AppModeServer final : public Weakable<AppModeServer> {
public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<AppModeServer>> create(AppModeServerConfig config);
    ~AppModeServer();

    static u16 compute_stable_port(StringView app_id, StringView account_id);

    u16 local_port() const;
    bool is_using_stable_port() const { return m_using_stable_port; }
    String origin() const;
    URL::URL entry_url() const;
    String const& session_token() const { return m_session_token; }
    ByteString const& data_directory() const { return m_data_directory; }
    AppManifest const& manifest() const { return m_manifest; }
    String const& account_id() const { return m_account_id; }
    String const& account_label() const { return m_account_label; }
    String const& profile_name() const { return m_profile_name; }
    Optional<ByteString> const& accounts_registry_path() const { return m_accounts_registry_path; }
    AppKVStore* kv_store() const { return m_kv_store.ptr(); }

    void push_service_notification(StringView origin, Optional<u64> tab_id, StringView event_type, StringView title, StringView body, StringView tag, i64 timestamp_ms);
    void push_service_network_event(u64 tab_id, URL::URL const& tab_url, u64 request_id, StringView request_url, StringView method, Optional<StringView> initiator_type, Optional<u32> status_code, u64 body_size, Optional<StringView> error, i64 timestamp_ms);
    void push_service_navigation_event(u64 tab_id, URL::URL const& url, StringView title, i64 timestamp_ms);
    ErrorOr<AppServiceTabState> open_service_tab(StringView service_id, Optional<URL::URL> url, bool activate, bool reuse, Optional<String> container_override = {});
    ErrorOr<AppServiceTabState> focus_service_tab(StringView service_id);
    ErrorOr<AppServiceTabState> navigate_service_tab(StringView service_id, URL::URL url);
    ErrorOr<bool> close_service_tab(StringView service_id);
    Vector<AppServiceTabState> list_service_tabs() const;
    void eval_service_tab(StringView service_id, String script, Function<void(Optional<JsonValue> value, Optional<String> error)> on_complete);
    void read_service_tab_markdown(StringView service_id, Optional<String> root_selector, bool main_content_only, bool include_links, bool include_images, size_t max_chars, Function<void(Optional<JsonValue> value, Optional<String> error)> on_complete);
    void list_service_tab_tools(StringView service_id, bool include_declarative, Function<void(Optional<JsonValue> value, Optional<String> error)> on_complete);
    void call_service_tab_tool(StringView service_id, String tool_name, JsonObject arguments, Function<void(Optional<JsonValue> value, Optional<String> error)> on_complete);
    Vector<AppServiceNotification> const& service_notifications() const { return m_service_notifications; }
    u64 last_notification_seq() const { return m_next_notification_seq > 1 ? m_next_notification_seq - 1 : 0; }
    Vector<AppServiceNetworkEvent> const& service_network_events() const { return m_service_network_events; }
    u64 last_network_seq() const { return m_next_network_seq > 1 ? m_next_network_seq - 1 : 0; }
    Vector<AppServiceEvent> const& service_events() const { return m_service_events; }
    u64 last_event_seq() const { return m_next_event_seq > 1 ? m_next_event_seq - 1 : 0; }
    size_t sse_client_count() const;
    void broadcast_sse_event(StringView event_name, JsonObject const& payload);

    static bool is_valid_cron_expression(StringView expr);
    static bool cron_matches_local_time(StringView expr, int minute, int hour, int day_of_month, int month, int day_of_week);
    void evaluate_cron_at(int year, int month, int day, int hour, int minute, int dow, i64 now_ms);

    bool has_service_register_consent_handler() const { return static_cast<bool>(m_on_service_register_consent); }
    void request_service_register_consent(AppServiceConfig const& service, Function<void(bool approved)> on_decision);

    Vector<AppTriggerConfig> const& triggers() const { return m_triggers; }
    ErrorOr<AppTriggerConfig> save_trigger(AppTriggerConfig);
    bool delete_trigger(StringView id);
    ErrorOr<AppTriggerConfig> set_trigger_enabled(StringView id, bool enabled);

    struct WebhookDispatchResult {
        u32 status_code { 202 };
        String message;
        u64 event_seq { 0 };
        String trigger_id;
    };
    WebhookDispatchResult receive_webhook(StringView trigger_id, StringView provided_token, StringView raw_body);

    Optional<AppTriggerConfig const&> find_trigger(StringView id) const;

    Vector<AppServiceConfig> effective_services() const;
    Optional<AppServiceConfig> find_effective_service(StringView service_id) const;
    bool is_dynamic_service(StringView service_id) const;
    ErrorOr<AppServiceConfig> register_dynamic_service(AppServiceConfig);
    bool remove_dynamic_service(StringView service_id);

    class Connection;

private:
    AppModeServer(
        AppModeServerConfig config,
        NonnullRefPtr<Core::TCPServer> server,
        bool using_stable_port,
        OwnPtr<AppKVStore> kv_store,
        String session_token);

    ErrorOr<void> on_new_client();
    void remove_connection(Connection&);
    String build_bridge_script() const;
    Optional<u16> current_mcp_port() const;
    void request_quit();
    ErrorOr<void> launch_account(StringView account_id);
    void append_service_event(StringView type, StringView service_id, Optional<u64> tab_id, JsonObject data, i64 timestamp_ms);
    void evaluate_event_triggers(StringView event_type, StringView service_id, Optional<u64> tab_id, StringView match_url, StringView match_method, Optional<u32> status_code, StringView match_title, StringView match_body, HashMap<String, String> trigger_vars, HashTable<String> tainted_keys);
    void fire_trigger(String const& trigger_id, HashMap<String, String> trigger_vars, HashTable<String> tainted_keys);
    void sync_interval_timers();
    void tick_cron_triggers();
    void load_triggers_from_disk();
    void save_triggers_to_disk() const;
    void load_dynamic_services_from_disk();
    void save_dynamic_services_to_disk() const;

    AppManifest m_manifest;
    ByteString m_data_directory;
    String m_account_id;
    String m_account_label;
    String m_profile_name;
    Optional<ByteString> m_accounts_registry_path;
    OwnPtr<AppKVStore> m_kv_store;
    NonnullRefPtr<Core::TCPServer> m_server;
    bool m_using_stable_port { false };
    bool m_enable_webmcp_shim { true };
    String m_session_token;
    Function<Optional<u16>()> m_mcp_port_provider;
    Function<void()> m_on_quit_requested;
    Function<ErrorOr<void>(StringView)> m_on_launch_account;
    Function<ErrorOr<AppServiceTabState>(StringView, Optional<URL::URL>, bool, bool, Optional<String>)> m_on_service_open;
    Function<ErrorOr<AppServiceTabState>(StringView)> m_on_service_focus;
    Function<ErrorOr<AppServiceTabState>(StringView, URL::URL)> m_on_service_navigate;
    Function<ErrorOr<bool>(StringView)> m_on_service_close;
    Function<Vector<AppServiceTabState>()> m_on_service_list;
    Function<void(StringView, String, Function<void(Optional<JsonValue>, Optional<String>)>)> m_on_service_eval;
    Function<void(StringView, Optional<String>, bool, bool, bool, size_t, Function<void(Optional<JsonValue>, Optional<String>)>)> m_on_service_read_markdown;
    Function<void(StringView, bool, Function<void(Optional<JsonValue>, Optional<String>)>)> m_on_service_list_tools;
    Function<void(StringView, String, JsonObject, Function<void(Optional<JsonValue>, Optional<String>)>)> m_on_service_call_tool;
    Function<void(AppServiceConfig const&, Function<void(bool)>)> m_on_service_register_consent;
    Function<Vector<FlowDefinition>()> m_on_flows_list;
    Function<Optional<FlowDefinition>(StringView)> m_on_flows_get;
    Function<ErrorOr<FlowDefinition>(FlowDefinition)> m_on_flows_save;
    Function<ErrorOr<void>(StringView)> m_on_flows_delete;
    Function<Optional<FlowRunRecord>(StringView)> m_on_flows_get_run;
    Function<Vector<FlowRunRecord>(Optional<StringView>, size_t)> m_on_flows_list_runs;
    Function<bool(StringView)> m_on_flows_cancel_run;
    Function<void(StringView, Optional<String>, Optional<FlowDefinition>, HashMap<String, String>, HashTable<String>, bool, bool, bool, bool, Function<void(Optional<FlowRunRecord>, Optional<String>)>)> m_on_service_run_flow;
    Vector<AppServiceNotification> m_service_notifications;
    u64 m_next_notification_seq { 1 };
    Vector<AppServiceNetworkEvent> m_service_network_events;
    u64 m_next_network_seq { 1 };
    Vector<AppServiceEvent> m_service_events;
    u64 m_next_event_seq { 1 };
    Vector<AppTriggerConfig> m_triggers;
    Vector<AppServiceConfig> m_dynamic_services;
    HashMap<String, RefPtr<Core::Timer>> m_interval_timers;
    RefPtr<Core::Timer> m_cron_timer;
    HashMap<String, String> m_cron_last_slot;
    HashTable<NonnullRefPtr<Connection>> m_connections;
    bool m_is_shutting_down { false };
};

}
