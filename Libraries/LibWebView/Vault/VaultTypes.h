/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API VaultCredential {
    String id {};
    String label {};
    Vector<String> allowed_origins {};
    String container_id {};
    String username {};
    String password {};
    String totp_secret {};
    String notes {};
    bool enabled { true };
    i64 created_at_ms { 0 };
    i64 updated_at_ms { 0 };
    i64 last_used_at_ms { 0 };
};

struct WEBVIEW_API VaultAuditEntry {
    i64 timestamp_ms { 0 };
    String action {};
    String credential_id {};
    String origin {};
    String field {};
    bool ok { false };
    String detail {};
};

struct WEBVIEW_API EncryptedVaultBlob {
    ByteBuffer nonce {};
    ByteBuffer ciphertext {};
    ByteBuffer tag {};
};

struct WEBVIEW_API MCPSecurityPolicy {
    Vector<String> allowed_containers {};
    HashMap<String, String> container_policies {};
    Vector<String> allowed_domains {};
    Vector<String> blocked_domains {};
    bool allow_private_network { false };
    bool disable_eval_js { false };
    bool disable_webmcp { false };
    bool webmcp_only { false };
    bool disable_screenshots { false };
    bool redact_screenshot_media { false };
    size_t max_tabs { 16 };
};

WEBVIEW_API bool is_valid_vault_credential_id(StringView id);
WEBVIEW_API ErrorOr<String> normalize_vault_origin(StringView raw);
WEBVIEW_API bool vault_credential_matches_origin(VaultCredential const&, URL::URL const&);
WEBVIEW_API bool vault_credential_matches_container(VaultCredential const&, StringView tab_container_id);

WEBVIEW_API ErrorOr<String> generate_totp_code(StringView base32_secret, Optional<i64> unix_seconds = {});
WEBVIEW_API ErrorOr<EncryptedVaultBlob> vault_encrypt_aes_256_gcm(ReadonlyBytes key_32, ReadonlyBytes plaintext, ReadonlyBytes aad);
WEBVIEW_API ErrorOr<ByteBuffer> vault_decrypt_aes_256_gcm(ReadonlyBytes key_32, ReadonlyBytes nonce_12, ReadonlyBytes ciphertext, ReadonlyBytes tag_16, ReadonlyBytes aad);
WEBVIEW_API ErrorOr<ByteBuffer> vault_derive_key_pbkdf2(StringView passphrase, ReadonlyBytes salt_16, u32 iterations = 210000);
WEBVIEW_API String scrub_secrets_from_string(StringView input, ReadonlySpan<String> secrets);

WEBVIEW_API bool mcp_domain_matches_pattern(StringView host, StringView pattern);
WEBVIEW_API bool mcp_host_is_private_or_internal(StringView host);
WEBVIEW_API bool mcp_url_may_access_private_network(URL::URL const& url, MCPSecurityPolicy const& policy);
WEBVIEW_API bool mcp_resolved_network_access_is_allowed(ReadonlySpan<ByteString> resolved_addresses, bool proxy_resolves_hostnames);
WEBVIEW_API ErrorOr<void> mcp_validate_url_against_policy(URL::URL const& url, MCPSecurityPolicy const& policy);
WEBVIEW_API ErrorOr<HashMap<String, String>> mcp_parse_container_policies(StringView raw_spec);
WEBVIEW_API Optional<StringView> mcp_container_policy_mode(StringView container_id, MCPSecurityPolicy const& policy);
WEBVIEW_API ErrorOr<String> mcp_resolve_container_against_policy(Optional<String> requested_container, MCPSecurityPolicy const& policy);
WEBVIEW_API bool mcp_container_is_allowed_by_policy(StringView container_id, MCPSecurityPolicy const& policy);
WEBVIEW_API ErrorOr<JsonValue> mcp_resolve_artifact_references(
    JsonValue const& input,
    Function<Optional<JsonValue>(StringView artifact_id)> const& resolver,
    Vector<String>& out_resolved_ids,
    size_t max_depth = 16,
    size_t max_expanded_bytes = 4 * 1024 * 1024);

WEBVIEW_API Vector<String> vault_credential_available_fields(VaultCredential const&);
WEBVIEW_API JsonObject serialize_vault_credential_metadata(VaultCredential const&);
WEBVIEW_API JsonObject serialize_vault_credential_hint(VaultCredential const&);
WEBVIEW_API JsonObject serialize_vault_credential_for_ui(VaultCredential const&);
WEBVIEW_API JsonObject serialize_vault_audit_entry(VaultAuditEntry const&);
WEBVIEW_API ErrorOr<VaultCredential> parse_vault_credential_ui_json(JsonValue const& value);

}
