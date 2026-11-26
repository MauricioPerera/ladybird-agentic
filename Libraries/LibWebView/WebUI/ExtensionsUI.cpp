/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibWebView/Application.h>
#include <LibWebView/Extensions/ExtensionManager.h>
#include <LibWebView/Extensions/ExtensionTypes.h>
#include <LibWebView/WebUI/ExtensionsUI.h>

namespace WebView {

ExtensionsUI::~ExtensionsUI()
{
    if (m_store)
        m_store->remove_observer(*this);
}

void ExtensionsUI::register_interfaces()
{
    (void)Application::the().extension_manager();
    auto& store = Application::the().extension_store();
    store.add_observer(*this);
    m_store = store.make_weak_ptr();

    register_interface("loadState"sv, [this](auto const&) {
        send_state();
    });
    register_interface("installUnpacked"sv, [this](auto const& data) {
        install_unpacked(data);
    });
    register_interface("createExampleExtension"sv, [this](auto const&) {
        create_example_extension();
    });
    register_interface("setEnabled"sv, [this](auto const& data) {
        set_enabled(data);
    });
    register_interface("setAllowInPrivate"sv, [this](auto const& data) {
        set_allow_in_private(data);
    });
    register_interface("reloadExtension"sv, [this](auto const& data) {
        reload_extension(data);
    });
    register_interface("removeExtension"sv, [this](auto const& data) {
        remove_extension(data);
    });
    register_interface("getStorage"sv, [this](auto const& data) {
        get_storage(data);
    });
    register_interface("setStorage"sv, [this](auto const& data) {
        set_storage(data);
    });
    register_interface("injectActiveTab"sv, [this](auto const& data) {
        inject_active_tab(data);
    });
}

void ExtensionsUI::extensions_changed()
{
    if (has_client())
        send_state();
}

void ExtensionsUI::send_state()
{
    auto& app = Application::the();
    auto& store = app.extension_store();

    JsonArray list;
    for (auto const& ext : store.list_extensions())
        list.must_append(serialize_extension(ext));

    JsonObject state;
    state.set("extensions"sv, move(list));
    state.set("storageDirectory"sv, String::from_utf8_without_validation(store.storage_directory().bytes()));
    state.set("mcpEnabled"sv, app.mcp_enabled());
    async_send_message("stateUpdated"sv, move(state));
}

void ExtensionsUI::send_action_result(bool ok, Optional<String> message, Optional<String> extension_id)
{
    JsonObject result;
    result.set("ok"sv, ok);
    if (message.has_value())
        result.set("message"sv, *message);
    if (extension_id.has_value())
        result.set("extensionId"sv, *extension_id);
    async_send_message("actionResult"sv, move(result));
}

void ExtensionsUI::install_unpacked(JsonValue const& data)
{
    if (!data.is_object()) {
        send_action_result(false, "Expected object payload for installUnpacked"_string);
        return;
    }
    auto path = data.as_object().get_string("path"sv);
    if (!path.has_value() || path->bytes_as_string_view().trim_whitespace().is_empty()) {
        send_action_result(false, "Please enter a valid unpacked extension directory path."_string);
        return;
    }

    auto installed = Application::the().extension_store().install_unpacked(path->to_byte_string());
    if (installed.is_error()) {
        send_action_result(false, MUST(String::formatted("Failed to load extension: {}", installed.error())));
        return;
    }

    send_action_result(true, MUST(String::formatted("Loaded extension '{}' ({})", installed.value().manifest.name, installed.value().manifest.id)), installed.value().manifest.id);
}

void ExtensionsUI::create_example_extension()
{
    auto created = Application::the().extension_store().create_example_extension();
    if (created.is_error()) {
        send_action_result(false, MUST(String::formatted("Could not create example extension: {}", created.error())));
        return;
    }

    send_action_result(true, MUST(String::formatted("Example extension '{}' ready.", created.value().manifest.name)), created.value().manifest.id);
}

void ExtensionsUI::set_enabled(JsonValue const& data)
{
    if (!data.is_object())
        return;
    auto const& obj = data.as_object();
    auto id = obj.get_string("id"sv);
    auto enabled = obj.get_bool("enabled"sv);
    if (!id.has_value() || !enabled.has_value())
        return;

    auto res = Application::the().extension_store().set_enabled(*id, *enabled);
    if (res.is_error())
        send_action_result(false, MUST(String::formatted("Failed updating extension: {}", res.error())), *id);
    else
        send_action_result(true, *enabled ? "Extension enabled."_string : "Extension disabled."_string, *id);
}

void ExtensionsUI::set_allow_in_private(JsonValue const& data)
{
    if (!data.is_object())
        return;
    auto const& obj = data.as_object();
    auto id = obj.get_string("id"sv);
    auto allow = obj.get_bool("allow"sv);
    if (!id.has_value() || !allow.has_value())
        return;

    auto res = Application::the().extension_store().set_allow_in_private(*id, *allow);
    if (res.is_error())
        send_action_result(false, MUST(String::formatted("Failed updating private setting: {}", res.error())), *id);
}

void ExtensionsUI::reload_extension(JsonValue const& data)
{
    if (!data.is_string())
        return;
    auto id = data.as_string();
    auto res = Application::the().extension_store().reload_extension(id);
    if (res.is_error())
        send_action_result(false, MUST(String::formatted("Reload failed: {}", res.error())), id);
    else
        send_action_result(true, "Extension reloaded from disk."_string, id);
}

void ExtensionsUI::remove_extension(JsonValue const& data)
{
    if (!data.is_string())
        return;
    auto id = data.as_string();
    if (auto res = Application::the().extension_store().remove_extension(id); res.is_error())
        send_action_result(false, MUST(String::formatted("Failed removing extension: {}", res.error())), id);
    else
        send_action_result(true, "Extension removed."_string, id);
}

void ExtensionsUI::get_storage(JsonValue const& data)
{
    if (!data.is_string())
        return;
    auto id = data.as_string();
    JsonObject out;
    out.set("id"sv, id);
    out.set("storage"sv, Application::the().extension_store().get_storage(id));
    async_send_message("storageLoaded"sv, move(out));
}

void ExtensionsUI::set_storage(JsonValue const& data)
{
    if (!data.is_object())
        return;
    auto const& obj = data.as_object();
    auto id = obj.get_string("id"sv);
    auto storage = obj.get_object("storage"sv);
    if (!id.has_value() || !storage.has_value()) {
        send_action_result(false, "Storage must be a JSON object."_string);
        return;
    }
    if (auto res = Application::the().extension_store().set_storage(*id, *storage); res.is_error())
        send_action_result(false, MUST(String::formatted("Failed saving storage: {}", res.error())), *id);
    else
        send_action_result(true, "Extension storage saved."_string, *id);
}

void ExtensionsUI::inject_active_tab(JsonValue const& data)
{
    if (!data.is_string())
        return;
    auto id = data.as_string();
    auto res = Application::the().extension_manager().inject_extension_into_active_tab(id);
    if (res.is_error())
        send_action_result(false, MUST(String::formatted("Could not inject extension: {}", res.error())), id);
    else
        send_action_result(true, MUST(String::formatted("Injected {} script group(s) into active tab.", res.value())), id);
}

}
