/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/LexicalPath.h>
#include <AK/Platform.h>
#include <AK/Utf16String.h>
#include <LibCore/BrowserVersion.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/GeolocationProvider.h>
#include <LibCore/Promise.h>
#include <LibCore/StandardPaths.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/HTML/AutoplayPolicy.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/ContainerStore.h>
#include <LibWebView/CrashReport.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/DefaultBrowser.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/UserAgent.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebUI/SettingsUI.h>

namespace WebView {

static StringView config_variable_type_to_string(JsonValue::Type type)
{
    switch (type) {
    case JsonValue::Type::Null:
        return "null"sv;
    case JsonValue::Type::Bool:
        return "boolean"sv;
    case JsonValue::Type::Number:
        return "number"sv;
    case JsonValue::Type::String:
        return "string"sv;
    case JsonValue::Type::Array:
        return "array"sv;
    case JsonValue::Type::Object:
        return "object"sv;
    }

    VERIFY_NOT_REACHED();
}

static bool should_show_config_variable(ConfigVariableID id)
{
    // NB: The Debug menu has the toggle for this one.
    if (id == ConfigVariableID::ShowTabPerformanceMonitor)
        return false;
    if (id == ConfigVariableID::ShowFeedbackButton)
        return !BROWSER_IS_FINAL_RELEASE;
    if (id == ConfigVariableID::UseClientSideWindowDecorations)
        return Application::the().supports_client_side_window_decorations();
    if (id == ConfigVariableID::VerticalTabsHoverExpandDelay || id == ConfigVariableID::VerticalTabsHoverExpandAnimationDuration)
        return Application::the().supports_vertical_tabs();
    return true;
}

SettingsUISitePermissionsObserver::SettingsUISitePermissionsObserver(SettingsUI& settings_ui)
    : m_settings_ui(settings_ui)
    , m_manager(*Application::default_session().permission_manager)
{
    m_manager->register_observer(*this);
}

// The settings page can outlive the browsing session at shutdown.
SettingsUISitePermissionsObserver::~SettingsUISitePermissionsObserver()
{
    if (auto* manager = m_manager.ptr())
        manager->unregister_observer(*this);
}

void SettingsUISitePermissionsObserver::permissions_changed(PermissionManager const&)
{
    m_settings_ui.broadcast_settings_change();
}

void SettingsUI::register_interfaces()
{
    m_site_permissions_observer = make<SettingsUISitePermissionsObserver>(*this);

    register_interface("loadFeatures"sv, [this](auto const&) {
        load_features();
    });
    register_interface("loadCurrentSettings"sv, [this](auto const&) {
        load_current_settings();
    });
    register_interface("refreshSettings"sv, [this](auto const&) {
        broadcast_settings_change();
    });
    register_interface("loadAvailableEngines"sv, [this](auto const&) {
        load_available_engines();
    });

    register_interface("setTabSettings"sv, [this](auto const& data) {
        set_tab_settings(data);
    });
    register_interface("setSearchEngineSettings"sv, [this](auto const& data) {
        set_search_engine_settings(data);
    });
    register_interface("setContentBlockerEnabled"sv, [this](auto const& data) {
        set_content_blocker_enabled(data);
    });
    register_interface("setContentBlockerEnabledForSite"sv, [this](auto const& data) {
        set_content_blocker_enabled_for_site(data);
    });
    register_interface("setContentBlockerListEnabled"sv, [this](auto const& data) {
        set_content_blocker_list_enabled(data);
    });
    register_interface("setFilterListUpdatesEnabled"sv, [this](auto const& data) {
        set_filter_list_updates_enabled(data);
    });
#if defined(LADYBIRD_HAS_UPDATE_CHECK)
    register_interface("setUpdateChecksEnabled"sv, [this](auto const& data) {
        set_update_checks_enabled(data);
    });
#endif
    register_interface("setAppearance"sv, [this](auto const& data) {
        set_appearance(data);
    });
    register_interface("makeDefaultBrowser"sv, [this](auto const& data) {
        make_default_browser(data);
    });
    register_interface("setUserAgentPreset"sv, [this](auto const& data) {
        set_user_agent_preset(data);
    });
    register_interface("setUploadDirectory"sv, [this](auto const& data) {
        set_upload_directory(data);
    });
    register_interface("openUploadDirectory"sv, [this](auto const& data) {
        open_upload_directory(data);
    });
    register_interface("setMcpAutoStart"sv, [this](auto const& data) {
        set_mcp_auto_start(data);
    });
    register_interface("setMcpEnabled"sv, [this](auto const& data) {
        set_mcp_enabled(data);
    });
    register_interface("createContainer"sv, [this](auto const& data) {
        create_container(data);
    });
    register_interface("deleteContainer"sv, [this](auto const& data) {
        delete_container(data);
    });
    register_interface("setTabRecorderSettings"sv, [this](auto const& data) {
        set_tab_recorder_settings(data);
    });

    if (host() == "welcome"sv) {
        register_interface("completeFirstRun"sv, [this](auto const& data) {
            complete_first_run(data);
        });
        return;
    }

    register_interface("loadCrashReportsWaiting"sv, [this](auto const&) {
        crash_reports_changed();
    });
    register_interface("showCrashReports"sv, [this](auto const&) {
        if (!CrashReport::is_supported()) {
            async_send_message("crashReportsStatus"sv, "Crash reporting is not available on this platform yet."_string);
            return;
        }
        auto result = CrashReportStore::the().show_directory();
        async_send_message("crashReportsStatus"sv, result.is_error() ? "Could not open the crash reports folder."_string : String {});
    });

    register_interface("setNewTabPageURL"sv, [this](auto const& data) {
        set_new_tab_page_url(data);
    });
    register_interface("setRestorePreviousSession"sv, [this](auto const& data) {
        set_restore_previous_session(data);
    });
    register_interface("setLanguages"sv, [this](auto const& data) {
        set_languages(data);
    });
    register_interface("resetHostSetting"sv, [this](auto const& data) {
        reset_host_setting(data);
    });
    register_interface("setContentSettings"sv, [this](auto const& data) {
        set_content_settings(data);
    });
    register_interface("setBrowsingBehavior"sv, [this](auto const& data) {
        set_browsing_behavior(data);
    });
    register_interface("setConfigVariable"sv, [this](auto const& data) {
        set_config_variable(data);
    });

    register_interface("addCustomSearchEngine"sv, [this](auto const& data) {
        add_custom_search_engine(data);
    });
    register_interface("removeCustomSearchEngine"sv, [this](auto const& data) {
        remove_custom_search_engine(data);
    });

    register_interface("loadForciblyEnabledSiteSettings"sv, [this](auto const&) {
        load_forcibly_enabled_site_settings();
    });
    register_interface("setSiteSettingPolicy"sv, [this](auto const& data) {
        set_site_setting_policy(data);
    });
    register_interface("addSiteSettingFilter"sv, [this](auto const& data) {
        add_site_setting_filter(data);
    });
    register_interface("removeSiteSettingFilter"sv, [this](auto const& data) {
        remove_site_setting_filter(data);
    });
    register_interface("removeAllSiteSettingFilters"sv, [this](auto const& data) {
        remove_all_site_setting_filters(data);
    });

    register_interface("estimateBrowsingDataSizes"sv, [this](auto const& data) {
        estimate_browsing_data_sizes(data);
    });
    register_interface("clearBrowsingData"sv, [this](auto const& data) {
        clear_browsing_data(data);
    });
    register_interface("setGlobalPrivacyControl"sv, [this](auto const& data) {
        set_global_privacy_control(data);
    });
    register_interface("setCrashReportSending"sv, [this](auto const& data) {
        set_crash_report_sending(data);
    });

    register_interface("setServicesNetworkAccessEnabled"sv, [this](auto const& data) {
        set_services_network_access_enabled(data);
    });
    register_interface("updateContentBlockerLists"sv, [this](auto const& data) {
        update_content_blocker_lists(data);
    });
    register_interface("addCustomContentBlockerSubscription"sv, [this](auto const& data) {
        add_custom_content_blocker_subscription(data);
    });
    register_interface("removeContentBlockerList"sv, [this](auto const& data) {
        remove_content_blocker_list(data);
    });
    register_interface("importLocalContentBlockerList"sv, [this](auto const& data) {
        import_local_content_blocker_list(data);
    });
    register_interface("setCustomContentBlockerFilters"sv, [this](auto const& data) {
        set_custom_content_blocker_filters(data);
    });

    register_interface("setDNSSettings"sv, [this](auto const& data) {
        set_dns_settings(data);
    });
}

void SettingsUI::load_features()
{
    auto& application = Application::the();

    JsonObject features;
    features.set("menuBar"_string, application.supports_system_menu_bar());
    features.set("primaryPaste"_string, application.supports_clipboard_type(Application::ClipboardType::Selection));
    features.set("verticalTabs"_string, application.supports_vertical_tabs());
    features.set("geolocation"_string, Core::GeolocationProvider::is_available());

    async_send_message("loadFeatures"sv, move(features));
}

static ByteString tab_recorder_settings_directory()
{
#if defined(AK_OS_WINDOWS)
    auto const& instance_dir = Application::the().instance_data_directory();
    if (!instance_dir.is_empty())
        return instance_dir;
    return Core::StandardPaths::user_data_directory();
#else
    return LexicalPath::join(Core::StandardPaths::config_directory(), "Ladybird"sv).string();
#endif
}

static ByteString tab_recorder_settings_path()
{
    return LexicalPath::join(tab_recorder_settings_directory(), "tab_recorder_settings.json"sv).string();
}

static JsonObject read_tab_recorder_settings_raw()
{
    auto parsed = read_json_file(tab_recorder_settings_path());
    if (!parsed.is_error())
        return parsed.release_value();
    return {};
}

static JsonObject serialize_tab_recorder_settings(JsonObject const& raw)
{
    JsonObject out;
    out.set("recordMicrophone"sv, raw.get_bool("record_microphone"sv).value_or(true));
    out.set("recordSystemAudio"sv, raw.get_bool("record_system_audio"sv).value_or(true));
    out.set("showCursor"sv, raw.get_bool("show_cursor"sv).value_or(true));
    out.set("exportSpeakerTimeline"sv, raw.get_bool("export_speaker_timeline"sv).value_or(true));
    out.set("fps"sv, clamp<u32>(raw.get_u32("fps"sv).value_or(30), 10, 60));
    out.set("batchDurationSeconds"sv, clamp<u32>(raw.get_u32("batch_duration_seconds"sv).value_or(300), 30, 3600));
    return out;
}

JsonObject SettingsUI::serialize_settings() const
{
    auto settings_json = Application::settings().serialize_json();
    auto settings = move(settings_json.as_object());

    settings.set("defaultNewTabPageURL"sv, Settings::default_new_tab_page_url().serialize());
    settings.set("defaultUploadDirectory"sv, Settings::default_upload_directory());
    auto effective_upload_dir = Application::the().effective_upload_directory();
    settings.set("effectiveUploadDirectory"sv, String::from_utf8_without_validation(effective_upload_dir.bytes()));
    settings.set("uploadDirectoryOverriddenByCli"sv, Application::the().upload_directory_overridden_by_cli());

    JsonArray available_ua_presets;
    for (auto const& entry : WebView::user_agents)
        available_ua_presets.must_append(entry.key);
    settings.set("availableUserAgentPresets"sv, move(available_ua_presets));

    switch (Application::the().default_browser().status()) {
    case DefaultBrowserStatus::Unknown:
        break;
    case DefaultBrowserStatus::Unavailable:
        settings.set("defaultBrowser"sv, "unavailable"_string);
        break;
    case DefaultBrowserStatus::NotDefault:
        settings.set("defaultBrowser"sv, "not-default"_string);
        break;
    case DefaultBrowserStatus::Default:
        settings.set("defaultBrowser"sv, "default"_string);
        break;
    }

    // The sites the user allowed or blocked, for each permission.
    JsonObject site_settings;
    site_settings.set("autoplay"sv, settings.get("autoplay"sv).value());
    for (auto name : Web::Permissions::PERMISSION_NAMES) {
        JsonArray sites;
        for (auto const& site : Application::default_session().permission_manager->site_decisions(name)) {
            JsonObject entry;
            entry.set("filter"sv, site.origin.serialize());
            switch (site.decision) {
            case PermissionDecision::Allow:
                entry.set("policy"sv, "allow"_string);
                break;
            case PermissionDecision::AllowOnce:
                entry.set("policy"sv, "allow-once"_string);
                break;
            case PermissionDecision::Block:
                entry.set("policy"sv, "block"_string);
                break;
            }
            sites.must_append(move(entry));
        }

        JsonObject setting;
        setting.set("policy"sv, Application::settings().permission_policy(name) == PermissionPolicy::Block ? "block"sv : "ask"sv);
        setting.set("siteFilters"sv, move(sites));
        site_settings.set(Web::Permissions::to_string(name), move(setting));
    }

    settings.set("siteSettings"sv, move(site_settings));

    JsonArray config_variables;
    for (auto const& variable : config_variable_definitions()) {
        if (!should_show_config_variable(variable.id))
            continue;

        JsonObject variable_object;
        variable_object.set("name"sv, variable.name);
        variable_object.set("title"sv, variable.title);
        variable_object.set("description"sv, variable.description);
        variable_object.set("type"sv, config_variable_type_to_string(variable.default_value.type()));
        if (variable.array_element_type.has_value())
            variable_object.set("elementType"sv, config_variable_type_to_string(*variable.array_element_type));
        variable_object.set("defaultValue"sv, variable.default_value);
        variable_object.set("value"sv, Application::settings().config_variable(variable.id));

        config_variables.must_append(move(variable_object));
    }

    settings.set("configVariableDefinitions"sv, move(config_variables));

    JsonArray lists;
    for (auto const& list : Application::settings().content_blocker_lists()) {
        JsonObject object;
        object.set("identifier"sv, list.identifier);
        object.set("name"sv, list.name);
        object.set("description"sv, list.description);
        object.set("url"sv, list.url.has_value() ? JsonValue(list.url->serialize()) : JsonValue {});
        object.set("builtIn"sv, list.built_in);
        object.set("languageSpecific"sv, list.language_specific);
        object.set("enabled"sv, list.enabled);
        auto updated = Application::the().content_blocker_list_last_updated_at(list.identifier);
        object.set("lastUpdatedAt"sv, updated.has_value() ? JsonValue(updated->milliseconds_since_epoch()) : JsonValue {});
        lists.must_append(move(object));
    }
    settings.set("contentBlockerLists"sv, move(lists));
    settings.set("contentBlockerListUpdateInProgress"sv, Application::the().content_blocker_list_update_in_progress());

    auto& app = Application::the();
    JsonObject mcp;
    mcp.set("enabled"sv, app.mcp_enabled());
    mcp.set("port"sv, app.mcp_port().value_or(WebView::default_mcp_port));
    mcp.set("autoStart"sv, Application::settings().mcp_auto_start());
    mcp.set("ephemeralDefault"sv, static_cast<MCP::MCPDelegate const&>(app).ephemeral_default_enabled());
    if (app.mcp_enabled() && app.mcp_token().has_value() && !app.mcp_token()->is_empty()) {
        JsonObject headers;
        headers.set("Authorization"sv, MUST(String::formatted("Bearer {}", *app.mcp_token())));
        JsonObject cfg;
        cfg.set("url"sv, MUST(String::formatted("http://127.0.0.1:{}/mcp", app.mcp_port().value_or(WebView::default_mcp_port))));
        cfg.set("headers"sv, move(headers));
        mcp.set("configJson"sv, JsonValue { move(cfg) }.serialized());
    } else {
        mcp.set("configJson"sv, JsonValue {});
    }
    settings.set("mcp"sv, move(mcp));

    JsonArray containers;
    for (auto const& container : Application::container_store().list_containers()) {
        JsonObject obj;
        obj.set("id"sv, container.id);
        obj.set("name"sv, container.name);
        obj.set("color"sv, container.color);
        containers.must_append(move(obj));
    }
    settings.set("containers"sv, move(containers));
    settings.set("tabRecorder"sv, serialize_tab_recorder_settings(read_tab_recorder_settings_raw()));

    return settings;
}

void SettingsUI::load_current_settings()
{
    m_current_settings = serialize_settings();
    async_send_message("loadSettings"sv, *m_current_settings);
}

void SettingsUI::broadcast_settings_change()
{
    if (!m_current_settings.has_value() || m_settings_change_scheduled)
        return;
    m_settings_change_scheduled = true;

    // Defer to allow the current handler to finish, including any downloads started after changing a setting.
    Core::deferred_invoke([weak_this = make_weak_ptr<SettingsUI>()] {
        auto* self = weak_this.ptr();
        if (!self)
            return;

        auto settings = self->serialize_settings();
        JsonObject changes;

        settings.for_each_member([&](auto const& name, auto const& value) {
            auto previous = self->m_current_settings->get(name);
            if (!previous.has_value() || !previous->equals(value))
                changes.set(name, value);
        });

        self->m_current_settings->for_each_member([&](auto const& name, auto const&) {
            if (!settings.has(name))
                changes.set(name, JsonValue {});
        });
        self->m_current_settings = move(settings);
        self->m_settings_change_scheduled = false;

        if (!changes.is_empty())
            self->async_send_message("loadSettings"sv, changes);
    });
}

void SettingsUI::complete_first_run(JsonValue const&)
{
    auto& settings = Application::settings();

    if (auto result = settings.complete_first_run(); result.is_error()) {
        async_send_message("firstRunError"sv, "Could not save your settings. Please try again."_string);
        return;
    }

    async_send_message("firstRunCompleted"sv, settings.new_tab_page_url().serialize());
}

void SettingsUI::make_default_browser(JsonValue const&)
{
    Application::the().default_browser().make_default([weak_this = make_weak_ptr<SettingsUI>()](MakeDefaultBrowserResult const& result) {
        auto self = weak_this.strong_ref();
        if (!self)
            return;

        if (!result.failed) {
            self->async_send_message("defaultBrowserResult"sv, JsonValue {});
            return;
        }

        auto failure = result.system_message.has_value()
            ? MUST(String::formatted("Could not make Ladybird the default browser. The system reported: {}", *result.system_message))
            : "Could not make Ladybird the default browser. Choose it in your system settings instead."_string;
        self->async_send_message("defaultBrowserResult"sv, move(failure));
    });
}

void SettingsUI::set_new_tab_page_url(JsonValue const& new_tab_page_url)
{
    if (!new_tab_page_url.is_string())
        return;

    auto parsed_new_tab_page_url = URL::Parser::basic_parse(new_tab_page_url.as_string());
    if (!parsed_new_tab_page_url.has_value())
        return;

    Application::settings().set_new_tab_page_url(parsed_new_tab_page_url.release_value());
}

void SettingsUI::set_restore_previous_session(JsonValue const& restore_previous_session)
{
    if (!restore_previous_session.is_bool())
        return;

    Application::settings().set_restore_previous_session(restore_previous_session.as_bool());
}

void SettingsUI::set_languages(JsonValue const& languages)
{
    auto parsed_languages = Settings::parse_json_languages(languages);
    Application::settings().set_languages(move(parsed_languages));
}

void SettingsUI::set_appearance(JsonValue const& appearance)
{
    auto parsed_appearance = Settings::parse_appearance(appearance);
    Application::settings().set_appearance(parsed_appearance);
}

void SettingsUI::set_content_settings(JsonValue const& content_settings)
{
    auto& settings = Application::settings();
    auto parsed_content_settings = Settings::parse_content_settings(content_settings);
    auto const& current_content_settings = settings.content_settings();

    // Host choices change through their own controls, independently of edits to the default.
    parsed_content_settings.zoom_per_host = current_content_settings.zoom_per_host;
    parsed_content_settings.force_dark_per_host = current_content_settings.force_dark_per_host;

    Application::settings().set_content_settings(move(parsed_content_settings));
}

void SettingsUI::reset_host_setting(JsonValue const& data)
{
    if (!data.is_object())
        return;

    auto setting = data.as_object().get_string("setting"sv);
    auto host = data.as_object().get_string("host"sv);
    if (data.as_object().has("host"sv) && !host.has_value())
        return;

    auto host_view = host.map([](auto const& value) { return value.bytes_as_string_view(); });

    if (setting == "zoom"sv)
        Application::settings().reset_host_setting(HostSetting::Zoom, host_view);
    else if (setting == "force-dark"sv)
        Application::settings().reset_host_setting(HostSetting::ForceDark, host_view);
}

void SettingsUI::set_tab_settings(JsonValue const& tab_settings)
{
    auto& settings = Application::settings();
    auto parsed_tab_settings = Settings::parse_tab_settings(tab_settings);
    auto const& current_tab_settings = settings.tab_settings();

    // Collapsed/expanded vertical tabs and their width are not controlled by the settings UI. Don't overwrite them.
    parsed_tab_settings.vertical_tabs_expanded = current_tab_settings.vertical_tabs_expanded;
    parsed_tab_settings.vertical_tabs_expanded_width = current_tab_settings.vertical_tabs_expanded_width;

    // NB: A sidebar layout picked on the welcome page opens expanded, so the choice shows in the window at once.
    if (host() == "welcome"sv && parsed_tab_settings.vertical_tabs_enabled)
        parsed_tab_settings.vertical_tabs_expanded = true;

    settings.set_tab_settings(parsed_tab_settings);
}

void SettingsUI::set_browsing_behavior(JsonValue const& browsing_behavior)
{
    auto parsed_browsing_behavior = Settings::parse_browsing_behavior(browsing_behavior);
    Application::settings().set_browsing_behavior(parsed_browsing_behavior);
}

void SettingsUI::set_config_variable(JsonValue const& variable)
{
    if (!variable.is_object())
        return;

    auto name = variable.as_object().get_string("name"sv);
    auto value = variable.as_object().get("value"sv);
    if (!name.has_value() || !value.has_value())
        return;

    Application::settings().set_config_variable(*name, *value);
}

void SettingsUI::load_available_engines()
{
    JsonArray search_engines;
    for (auto const& engine : WebView::builtin_search_engines()) {
        JsonObject object;
        object.set("name"sv, engine.name);
        object.set("supportsSuggestions"sv, engine.suggestions_url.has_value());
        search_engines.must_append(move(object));
    }

    JsonObject engines;
    engines.set("search"sv, move(search_engines));

    async_send_message("loadEngines"sv, move(engines));
}

void SettingsUI::set_search_engine_settings(JsonValue const& data)
{
    if (!data.is_object())
        return;

    auto engine = data.as_object().get("engine"sv);
    auto suggestions = data.as_object().get_bool("suggestions"sv);
    if (!engine.has_value() || (!engine->is_null() && !engine->is_string()) || !suggestions.has_value())
        return;

    auto& settings = Application::settings();
    SearchEngineSettings search_engine_settings;
    if (engine->is_string())
        search_engine_settings.engine = settings.find_search_engine_by_name(engine->as_string());
    search_engine_settings.suggestions = *suggestions;
    settings.set_search_engine_settings(move(search_engine_settings));
}

void SettingsUI::add_custom_search_engine(JsonValue const& search_engine)
{
    if (auto custom_engine = Settings::parse_custom_search_engine(search_engine); custom_engine.has_value())
        Application::settings().add_custom_search_engine(custom_engine.release_value());
}

void SettingsUI::remove_custom_search_engine(JsonValue const& search_engine)
{
    if (auto custom_engine = Settings::parse_custom_search_engine(search_engine); custom_engine.has_value())
        Application::settings().remove_custom_search_engine(*custom_engine);
}

void SettingsUI::load_forcibly_enabled_site_settings()
{
    JsonArray site_settings;

    if (Application::web_content_options().enable_autoplay == EnableAutoplay::Yes)
        site_settings.must_append("autoplay"sv);

    async_send_message("forciblyEnableSiteSettings"sv, move(site_settings));
}

void SettingsUI::set_site_setting_policy(JsonValue const& site_setting)
{
    if (!site_setting.is_object())
        return;

    auto setting = site_setting.as_object().get_string("setting"sv);
    auto policy = site_setting.as_object().get_string("policy"sv);
    if (!setting.has_value() || !policy.has_value() || !policy->is_ascii())
        return;

    if (*setting == "autoplay"sv) {
        auto policy_utf16 = Utf16View::from_ascii(*policy);
        if (auto parsed = Web::HTML::autoplay_policy_from_string(policy_utf16); parsed.has_value())
            Application::settings().set_autoplay_policy(*parsed);
    } else if (auto name = Web::Permissions::permission_name_from_string(*setting); name.has_value()) {
        if (*policy == "ask"sv)
            Application::settings().set_permission_policy(*name, PermissionPolicy::Ask);
        else if (*policy == "block"sv)
            Application::settings().set_permission_policy(*name, PermissionPolicy::Block);
    }
}

static Optional<URL::Origin> site_setting_origin(String const& filter)
{
    auto url = URL::Parser::basic_parse(filter.bytes_as_string_view().trim_whitespace());
    if (!url.has_value() || url->origin().is_opaque())
        return {};
    return url->origin();
}

void SettingsUI::add_site_setting_filter(JsonValue const& site_setting)
{
    if (!site_setting.is_object())
        return;

    auto setting = site_setting.as_object().get_string("setting"sv);
    auto filter = site_setting.as_object().get_string("filter"sv);
    auto policy = site_setting.as_object().get_string("policy"sv);
    if (!setting.has_value() || !filter.has_value() || !policy.has_value() || !policy->is_ascii())
        return;

    auto origin = site_setting_origin(*filter);
    if (!origin.has_value())
        return;

    if (*setting == "autoplay"sv) {
        auto policy_utf16 = Utf16String::from_utf8_without_validation(*policy);
        if (auto parsed = Web::HTML::autoplay_policy_from_string(policy_utf16.utf16_view()); parsed.has_value())
            Application::settings().add_autoplay_site_filter(origin->serialize(), *parsed);
    } else if (auto name = Web::Permissions::permission_name_from_string(*setting); name.has_value()) {
        PermissionDecision decision;
        if (*policy == "allow"sv)
            decision = PermissionDecision::Allow;
        else if (*policy == "block"sv)
            decision = PermissionDecision::Block;
        else
            return;

        Application::default_session().permission_manager->set_decision(*name, *origin, decision);
    }
}

void SettingsUI::remove_site_setting_filter(JsonValue const& site_setting)
{
    if (!site_setting.is_object())
        return;

    auto setting = site_setting.as_object().get_string("setting"sv);
    auto filter = site_setting.as_object().get_string("filter"sv);
    if (!setting.has_value() || !filter.has_value())
        return;

    auto origin = site_setting_origin(*filter);
    if (!origin.has_value())
        return;

    if (*setting == "autoplay"sv) {
        Application::settings().remove_autoplay_site_filter(origin->serialize());
    } else if (auto name = Web::Permissions::permission_name_from_string(*setting); name.has_value()) {
        Application::default_session().permission_manager->set_decision(*name, *origin, {});
    }
}

void SettingsUI::remove_all_site_setting_filters(JsonValue const& site_setting)
{
    if (!site_setting.is_object())
        return;

    auto setting = site_setting.as_object().get_string("setting"sv);
    if (!setting.has_value())
        return;

    if (*setting == "autoplay"sv) {
        Application::settings().remove_all_autoplay_site_filters();
    } else if (auto name = Web::Permissions::permission_name_from_string(*setting); name.has_value()) {
        auto& manager = *Application::default_session().permission_manager;

        for (auto const& site : manager.site_decisions(*name))
            manager.set_decision(*name, site.origin, {});
    }
}

void SettingsUI::estimate_browsing_data_sizes(JsonValue const& options)
{
    if (!options.is_object())
        return;

    auto& application = Application::the();

    auto since = [&]() {
        if (auto since = options.as_object().get_integer<i64>("since"sv); since.has_value())
            return UnixDateTime::from_milliseconds_since_epoch(*since);
        return UnixDateTime::earliest();
    }();

    application.estimate_browsing_data_size_accessed_since(since)
        ->when_resolved([weak_this = make_weak_ptr<SettingsUI>()](Application::BrowsingDataSizes const& sizes) {
            JsonObject result;

            result.set("cacheSizeSinceRequestedTime"sv, sizes.cache_size_since_requested_time);
            result.set("totalCacheSize"sv, sizes.total_cache_size);

            result.set("siteDataSizeSinceRequestedTime"sv, sizes.site_data_size_since_requested_time);
            result.set("totalSiteDataSize"sv, sizes.total_site_data_size);

            if (auto self = weak_this.strong_ref())
                self->async_send_message("estimatedBrowsingDataSizes"sv, move(result));
        })
        .when_rejected([](Error const& error) {
            dbgln("Failed to estimate browsing data sizes: {}", error);
        });
}

void SettingsUI::clear_browsing_data(JsonValue const& options)
{
    if (!options.is_object())
        return;

    Application::ClearBrowsingDataOptions clear_browsing_data_options;

    if (auto since = options.as_object().get_integer<i64>("since"sv); since.has_value())
        clear_browsing_data_options.since = UnixDateTime::from_milliseconds_since_epoch(*since);

    clear_browsing_data_options.delete_cached_files = options.as_object().get_bool("cachedFiles"sv).value_or(false)
        ? Application::ClearBrowsingDataOptions::Delete::Yes
        : Application::ClearBrowsingDataOptions::Delete::No;

    clear_browsing_data_options.delete_history = options.as_object().get_bool("history"sv).value_or(false)
        ? Application::ClearBrowsingDataOptions::Delete::Yes
        : Application::ClearBrowsingDataOptions::Delete::No;

    clear_browsing_data_options.delete_download_history = options.as_object().get_bool("downloadHistory"sv).value_or(false)
        ? Application::ClearBrowsingDataOptions::Delete::Yes
        : Application::ClearBrowsingDataOptions::Delete::No;

    clear_browsing_data_options.delete_site_data = options.as_object().get_bool("siteData"sv).value_or(false)
        ? Application::ClearBrowsingDataOptions::Delete::Yes
        : Application::ClearBrowsingDataOptions::Delete::No;

    auto& application = Application::the();

    application.clear_browsing_data(clear_browsing_data_options)
        ->when_resolved([weak_this = make_weak_ptr<SettingsUI>()](Empty) {
            if (auto self = weak_this.strong_ref())
                self->async_send_message("clearedBrowsingData"sv, {});
        })
        .when_rejected([](Error const& error) {
            dbgln("Failed to clear browser data: {}", error);
        });
}

void SettingsUI::set_global_privacy_control(JsonValue const& global_privacy_control)
{
    if (!global_privacy_control.is_bool())
        return;

    Application::settings().set_global_privacy_control(global_privacy_control.as_bool() ? GlobalPrivacyControl::Yes : GlobalPrivacyControl::No);
}

void SettingsUI::set_user_agent_preset(JsonValue const& preset)
{
    if (!preset.is_string())
        return;

    auto value = preset.as_string();
    if (value.is_empty())
        Application::settings().set_user_agent_preset({});
    else
        Application::settings().set_user_agent_preset(move(value));
}

void SettingsUI::set_upload_directory(JsonValue const& directory)
{
    if (!directory.is_string())
        return;

    auto raw = directory.as_string();
    auto trimmed = raw.bytes_as_string_view().trim_whitespace();
    Optional<String> next_value;
    if (!trimmed.is_empty()) {
        auto validated = Settings::validate_upload_directory(trimmed);
        if (validated.is_error()) {
            async_send_message("uploadDirectoryStatus"sv, MUST(String::formatted("Invalid upload folder: {}", validated.error())));
            return;
        }
        if (auto dir_res = Core::Directory::create(validated.value().to_byte_string(), Core::Directory::CreateDirectories::Yes); dir_res.is_error()) {
            async_send_message("uploadDirectoryStatus"sv, MUST(String::formatted("Could not create upload folder: {}", dir_res.error())));
            return;
        }
        next_value = validated.release_value();
    }

    if (auto res = Application::settings().set_upload_directory(move(next_value)); res.is_error()) {
        async_send_message("uploadDirectoryStatus"sv, MUST(String::formatted("Could not save upload folder: {}", res.error())));
        return;
    }

    async_send_message("uploadDirectoryStatus"sv, String {});
    broadcast_settings_change();
}

void SettingsUI::open_upload_directory(JsonValue const&)
{
    if (auto res = Application::the().open_upload_directory(); res.is_error())
        async_send_message("uploadDirectoryStatus"sv, MUST(String::formatted("Could not open upload folder: {}", res.error())));
    else
        async_send_message("uploadDirectoryStatus"sv, String {});
}

void SettingsUI::set_mcp_auto_start(JsonValue const& value)
{
    if (!value.is_bool())
        return;

    Application::settings().set_mcp_auto_start(value.as_bool());
}

void SettingsUI::set_mcp_enabled(JsonValue const& value)
{
    if (!value.is_bool())
        return;

    bool desired = value.as_bool();
    auto& app = Application::the();
    JsonObject result;
    if (desired != app.mcp_enabled()) {
        if (auto res = app.toggle_mcp_enabled(); res.is_error()) {
            result.set("ok"sv, false);
            result.set("error"sv, MUST(String::formatted("Could not update MCP server: {}", res.error())));
            async_send_message("mcpActionResult"sv, move(result));
            broadcast_settings_change();
            return;
        }
    }

    result.set("ok"sv, true);
    async_send_message("mcpActionResult"sv, move(result));
    broadcast_settings_change();
}

void SettingsUI::create_container(JsonValue const& value)
{
    if (!value.is_object())
        return;

    auto name = value.as_object().get_string("name"sv).value_or(String {});
    auto color = value.as_object().get_string("color"sv).value_or("#3b82f6"_string);
    auto created = Application::container_store().create_container(move(name), move(color));
    JsonObject result;
    if (created.is_error()) {
        result.set("ok"sv, false);
        result.set("error"sv, MUST(String::formatted("Could not create container: {}", created.error())));
        async_send_message("containerActionResult"sv, move(result));
        return;
    }

    JsonObject container_obj;
    container_obj.set("id"sv, created.value().id);
    container_obj.set("name"sv, created.value().name);
    container_obj.set("color"sv, created.value().color);
    result.set("ok"sv, true);
    result.set("container"sv, move(container_obj));
    async_send_message("containerActionResult"sv, move(result));
    broadcast_settings_change();
}

void SettingsUI::delete_container(JsonValue const& value)
{
    if (!value.is_string())
        return;

    auto res = Application::the().delete_container(value.as_string());
    JsonObject result;
    if (res.is_error()) {
        result.set("ok"sv, false);
        result.set("error"sv, MUST(String::formatted("Could not delete container: {}", res.error())));
        async_send_message("containerActionResult"sv, move(result));
        return;
    }

    result.set("ok"sv, true);
    async_send_message("containerActionResult"sv, move(result));
    broadcast_settings_change();
}

void SettingsUI::set_tab_recorder_settings(JsonValue const& value)
{
    if (!value.is_object())
        return;

    auto const& input = value.as_object();
    auto raw = read_tab_recorder_settings_raw();
    auto merged = serialize_tab_recorder_settings(raw);

    if (auto v = input.get_bool("recordMicrophone"sv); v.has_value())
        raw.set("record_microphone"sv, *v);
    else if (!raw.has("record_microphone"sv))
        raw.set("record_microphone"sv, merged.get_bool("recordMicrophone"sv).value_or(true));

    if (auto v = input.get_bool("recordSystemAudio"sv); v.has_value())
        raw.set("record_system_audio"sv, *v);
    else if (!raw.has("record_system_audio"sv))
        raw.set("record_system_audio"sv, merged.get_bool("recordSystemAudio"sv).value_or(true));

    if (auto v = input.get_bool("showCursor"sv); v.has_value())
        raw.set("show_cursor"sv, *v);
    else if (!raw.has("show_cursor"sv))
        raw.set("show_cursor"sv, merged.get_bool("showCursor"sv).value_or(true));

    if (auto v = input.get_bool("exportSpeakerTimeline"sv); v.has_value())
        raw.set("export_speaker_timeline"sv, *v);
    else if (!raw.has("export_speaker_timeline"sv))
        raw.set("export_speaker_timeline"sv, merged.get_bool("exportSpeakerTimeline"sv).value_or(true));

    if (auto v = input.get_u32("fps"sv); v.has_value())
        raw.set("fps"sv, clamp<u32>(*v, 10, 60));
    else if (!raw.has("fps"sv))
        raw.set("fps"sv, merged.get_u32("fps"sv).value_or(30));

    if (auto v = input.get_u32("batchDurationSeconds"sv); v.has_value())
        raw.set("batch_duration_seconds"sv, clamp<u32>(*v, 30, 3600));
    else if (!raw.has("batch_duration_seconds"sv))
        raw.set("batch_duration_seconds"sv, merged.get_u32("batchDurationSeconds"sv).value_or(300));

    if (!raw.has("microphone_device_id"sv))
        raw.set("microphone_device_id"sv, String {});

    (void)Core::Directory::create(tab_recorder_settings_directory(), Core::Directory::CreateDirectories::Yes);
    (void)write_json_file(tab_recorder_settings_path(), JsonValue { move(raw) });
    broadcast_settings_change();
}

void SettingsUI::crash_reports_changed()
{
    if (has_client())
        async_send_message("crashReportsWaiting"sv, CrashReportOutbox::the().waiting_report_count());
}

void SettingsUI::set_crash_report_sending(JsonValue const& crash_report_sending)
{
    if (!crash_report_sending.is_string())
        return;

    auto sending = crash_report_sending_from_string(crash_report_sending.as_string());
    Application::settings().set_crash_report_sending(sending);

    if (sending == CrashReportSending::NeverSend)
        CrashReportOutbox::the().discard_all();
}

void SettingsUI::set_services_network_access_enabled(JsonValue const& enabled)
{
    if (!enabled.is_bool())
        return;

    Application::settings().set_background_networking_enabled(enabled.as_bool());
}

void SettingsUI::set_filter_list_updates_enabled(JsonValue const& enabled)
{
    if (!enabled.is_bool())
        return;

    Application::settings().set_filter_list_updates_enabled(enabled.as_bool());
}

#if defined(LADYBIRD_HAS_UPDATE_CHECK)
void SettingsUI::set_update_checks_enabled(JsonValue const& enabled)
{
    if (!enabled.is_bool())
        return;

    Application::settings().set_update_checks_enabled(enabled.as_bool());
}
#endif

void SettingsUI::update_content_blocker_lists(JsonValue const&)
{
    Application::the().update_content_blocker_lists({});
    broadcast_settings_change();
}

void SettingsUI::set_content_blocker_enabled(JsonValue const& enabled)
{
    if (!enabled.is_bool())
        return;

    Application::settings().set_content_blocker_enabled(enabled.as_bool());

    if (enabled.as_bool()) {
        for (auto const& list : Application::settings().content_blocker_lists()) {
            if (list.enabled)
                Application::the().download_content_blocker_list_if_needed({}, list.identifier);
        }
    }
}

void SettingsUI::set_content_blocker_enabled_for_site(JsonValue const& value)
{
    if (!value.is_object())
        return;

    auto site = value.as_object().get_string("site"sv);
    auto enabled = value.as_object().get_bool("enabled"sv);
    if (!site.has_value() || !enabled.has_value())
        return;

    if (auto result = Application::settings().set_content_blocker_enabled_for_site(*site, *enabled); result.is_error())
        dbgln("Unable to change content blocking for {}: {}", *site, result.error());
}

void SettingsUI::set_content_blocker_list_enabled(JsonValue const& value)
{
    if (!value.is_object())
        return;

    auto identifier = value.as_object().get_string("identifier"sv);
    auto enabled = value.as_object().get_bool("enabled"sv);
    if (!identifier.has_value() || !enabled.has_value())
        return;

    Application::settings().set_content_blocker_list_enabled(*identifier, *enabled);
    if (*enabled && Application::settings().content_blocker_enabled())
        Application::the().download_content_blocker_list_if_needed({}, *identifier);
}

void SettingsUI::add_custom_content_blocker_subscription(JsonValue const& value)
{
    if (!value.is_string())
        return;

    auto url = URL::Parser::basic_parse(value.as_string());
    if (!url.has_value() || !url->scheme().is_one_of("http"sv, "https"sv)) {
        report_content_blocker_result("subscription"_string, "Enter an HTTP or HTTPS URL."_string);
        return;
    }

    for (auto const& subscription : Application::settings().content_blocker_lists()) {
        if (!subscription.built_in && subscription.url == url) {
            report_content_blocker_result("subscription"_string, "Subscription already added."_string);
            return;
        }
    }

    auto identifier = Application::settings().add_content_blocker_list(url->serialize(), url);
    Application::the().download_content_blocker_list_if_needed({}, identifier);
    report_content_blocker_result("subscription"_string, "Subscription added."_string);
}

void SettingsUI::remove_content_blocker_list(JsonValue const& value)
{
    if (!value.is_string())
        return;

    Application::the().remove_content_blocker_list({}, value.as_string());
}

void SettingsUI::import_local_content_blocker_list(JsonValue const& value)
{
    if (!value.is_object())
        return;

    auto name = value.as_object().get_string("name"sv);
    auto contents = value.as_object().get_string("contents"sv);
    if (!name.has_value() || name->is_empty() || !contents.has_value())
        return;

    if (auto result = Application::the().import_local_content_blocker_list(*name, *contents); result.is_error())
        report_content_blocker_result("import"_string, MUST(String::formatted("Unable to import list: {}", result.error())));
    else
        report_content_blocker_result("import"_string, MUST(String::formatted("{} imported.", *name)));
}

void SettingsUI::set_custom_content_blocker_filters(JsonValue const& value)
{
    if (!value.is_string() || value.as_string().bytes().size() > 4 * MiB)
        return;

    Application::settings().set_custom_content_blocker_filters(value.as_string());
}

void SettingsUI::report_content_blocker_result(String operation, String message)
{
    JsonObject result;
    result.set("operation"sv, move(operation));
    result.set("message"sv, move(message));
    async_send_message("contentBlockerResult"sv, result);
}

void SettingsUI::set_dns_settings(JsonValue const& dns_settings)
{
    Application::settings().set_dns_settings(Settings::parse_dns_settings(dns_settings));
}

}
