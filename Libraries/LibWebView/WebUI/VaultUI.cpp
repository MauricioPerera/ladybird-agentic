/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Time.h>
#include <LibWebView/Application.h>
#include <LibWebView/Vault/VaultTypes.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebUI/VaultUI.h>

namespace WebView {

VaultUI::~VaultUI()
{
    if (m_store)
        m_store->remove_observer(*this);
    if (m_containers)
        m_containers->remove_observer(*this);
}

void VaultUI::register_interfaces()
{
    auto& app = Application::the();
    auto& store = app.vault_store();
    store.add_observer(*this);
    m_store = store.make_weak_ptr();

    auto& containers = app.container_store();
    containers.add_observer(*this);
    m_containers = containers.make_weak_ptr();

    register_interface("loadState"sv, [this](auto const&) {
        send_state();
    });
    register_interface("saveCredential"sv, [this](auto const& data) {
        save_credential(data);
    });
    register_interface("setCredentialEnabled"sv, [this](auto const& data) {
        set_credential_enabled(data);
    });
    register_interface("deleteCredential"sv, [this](auto const& data) {
        delete_credential(data);
    });
    register_interface("testTotp"sv, [this](auto const& data) {
        test_totp(data);
    });
}

void VaultUI::vault_changed()
{
    if (has_client())
        send_state();
}

void VaultUI::containers_changed()
{
    if (has_client())
        send_state();
}

void VaultUI::send_state()
{
    auto& app = Application::the();
    auto& store = app.vault_store();

    JsonArray creds_json;
    for (auto const& cred : store.list_credentials(true))
        creds_json.must_append(serialize_vault_credential_for_ui(cred));

    JsonArray containers_json;
    for (auto const& c : app.container_store().list_containers()) {
        JsonObject obj;
        obj.set("id"sv, c.id);
        obj.set("name"sv, c.name);
        obj.set("color"sv, c.color);
        containers_json.must_append(move(obj));
    }

    JsonArray tabs_json;
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (view.owner_view_id().has_value() || view.is_private() == IsPrivate::Yes)
            return IterationDecision::Continue;
        if (!view.url().scheme().is_one_of("http"sv, "https"sv))
            return IterationDecision::Continue;

        JsonObject tab;
        tab.set("id"sv, view.view_id());
        tab.set("title"sv, view.title().to_well_formed_utf8());
        tab.set("url"sv, view.url().to_string());
        tab.set("origin"sv, view.url().origin().serialize());
        tab.set("containerId"sv, view.container_id());
        tab.set("mcpAllowed"sv, view.is_mcp_access_allowed());
        tabs_json.must_append(move(tab));
        return IterationDecision::Continue;
    });

    JsonArray audit_json;
    for (auto const& entry : store.recent_audit(50))
        audit_json.must_append(serialize_vault_audit_entry(entry));

    auto policy = app.mcp_security_policy();
    JsonObject policy_json;
    JsonArray allowed_containers;
    for (auto const& c : policy.allowed_containers)
        allowed_containers.must_append(c);
    JsonArray allowed_domains;
    for (auto const& d : policy.allowed_domains)
        allowed_domains.must_append(d);
    JsonArray blocked_domains;
    for (auto const& d : policy.blocked_domains)
        blocked_domains.must_append(d);
    policy_json.set("allowedContainers"sv, move(allowed_containers));
    policy_json.set("allowedDomains"sv, move(allowed_domains));
    policy_json.set("blockedDomains"sv, move(blocked_domains));
    policy_json.set("disableEvalJs"sv, policy.disable_eval_js);
    policy_json.set("maxTabs"sv, static_cast<u64>(policy.max_tabs));

    JsonObject state;
    state.set("unlocked"sv, store.is_unlocked());
    state.set("keyMode"sv, store.key_mode_name());
    if (store.last_error().has_value())
        state.set("lastError"sv, *store.last_error());
    state.set("credentials"sv, move(creds_json));
    state.set("containers"sv, move(containers_json));
    state.set("tabs"sv, move(tabs_json));
    state.set("audit"sv, move(audit_json));
    state.set("policy"sv, move(policy_json));
    state.set("mcpEnabled"sv, app.mcp_enabled());
    state.set("storageDirectory"sv, String::from_utf8_without_validation(store.storage_directory().bytes()));

    async_send_message("stateUpdated"sv, move(state));
}

void VaultUI::send_action_result(bool ok, Optional<String> message, Optional<String> credential_id)
{
    JsonObject result;
    result.set("ok"sv, ok);
    if (message.has_value())
        result.set("message"sv, *message);
    if (credential_id.has_value())
        result.set("credentialId"sv, *credential_id);
    async_send_message("actionResult"sv, move(result));
}

void VaultUI::save_credential(JsonValue const& data)
{
    auto parsed = parse_vault_credential_ui_json(data);
    if (parsed.is_error()) {
        send_action_result(false, MUST(String::formatted("Invalid credential: {}", parsed.error())));
        return;
    }

    auto const& obj = data.as_object();
    bool clear_password = obj.get_bool("clearPassword"sv).value_or(false);
    bool clear_totp = obj.get_bool("clearTotp"sv).value_or(false);

    auto saved = Application::the().vault_store().save_credential(parsed.release_value(), true, clear_password, clear_totp);
    if (saved.is_error()) {
        send_action_result(false, MUST(String::formatted("Failed saving credential: {}", saved.error())));
        return;
    }

    send_action_result(true, "Credential saved in encrypted vault."_string, saved.value().id);
}

void VaultUI::set_credential_enabled(JsonValue const& data)
{
    if (!data.is_object()) {
        send_action_result(false, "Expected object payload for setCredentialEnabled"_string);
        return;
    }
    auto const& obj = data.as_object();
    auto id = obj.get_string("id"sv);
    auto enabled = obj.get_bool("enabled"sv);
    if (!id.has_value() || !enabled.has_value()) {
        send_action_result(false, "Missing 'id' or 'enabled' field"_string);
        return;
    }

    auto updated = Application::the().vault_store().set_credential_enabled(*id, *enabled);
    if (updated.is_error()) {
        send_action_result(false, MUST(String::formatted("Failed updating credential: {}", updated.error())));
        return;
    }

    send_action_result(true, *enabled ? "Credential enabled."_string : "Credential disabled."_string, updated.value().id);
}

void VaultUI::delete_credential(JsonValue const& data)
{
    if (!data.is_string()) {
        send_action_result(false, "Expected credential id string"_string);
        return;
    }
    if (auto res = Application::the().vault_store().delete_credential(data.as_string()); res.is_error()) {
        send_action_result(false, MUST(String::formatted("Failed deleting credential: {}", res.error())));
        return;
    }
    send_action_result(true, "Credential deleted."_string);
}

void VaultUI::test_totp(JsonValue const& data)
{
    String secret;
    if (data.is_object()) {
        auto const& obj = data.as_object();
        if (auto direct = obj.get_string("totpSecret"sv); direct.has_value() && !direct->is_empty()) {
            secret = *direct;
        } else if (auto id = obj.get_string("id"sv); id.has_value() && !id->is_empty()) {
            if (auto cred = Application::the().vault_store().get_credential(*id, true); cred.has_value())
                secret = cred->totp_secret;
        }
    }

    if (secret.is_empty()) {
        async_send_message("totpResult"sv, [] {
            JsonObject o;
            o.set("ok"sv, false);
            o.set("error"sv, "No TOTP secret entered or saved for this credential."sv);
            return o;
        }());
        return;
    }

    i64 now = UnixDateTime::now().seconds_since_epoch();
    auto code = generate_totp_code(secret, now);
    JsonObject out;
    if (code.is_error()) {
        out.set("ok"sv, false);
        out.set("error"sv, MUST(String::formatted("{}", code.error())));
    } else {
        out.set("ok"sv, true);
        out.set("code"sv, code.release_value());
        out.set("secondsRemaining"sv, 30 - (now % 30));
    }
    async_send_message("totpResult"sv, move(out));
}

}
