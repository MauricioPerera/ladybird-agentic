/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/CharacterTypes.h>
#include <AK/IPv4Address.h>
#include <AK/IPv6Address.h>
#include <AK/JsonArray.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibURL/Parser.h>
#include <LibWebView/ContainerStore.h>
#include <LibWebView/Vault/VaultTypes.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

namespace WebView {

bool is_valid_vault_credential_id(StringView id)
{
    if (id.is_empty() || id.length() > 64)
        return false;
    if (id.starts_with('-') || id.ends_with('-'))
        return false;
    bool prev_hyphen = false;
    for (auto byte : id.bytes()) {
        bool valid = (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-';
        if (!valid)
            return false;
        if (byte == '-' && prev_hyphen)
            return false;
        prev_hyphen = (byte == '-');
    }
    return true;
}

ErrorOr<String> normalize_vault_origin(StringView raw)
{
    auto trimmed = raw.trim(" \t\r\n"sv);
    if (trimmed.is_empty())
        return Error::from_string_literal("Allowed origin cannot be empty");
    if (trimmed.contains('*'))
        return Error::from_string_literal("Wildcards '*' are not allowed in Vault origins; pin exact https:// origins");

    auto parsed = URL::Parser::basic_parse(trimmed);
    if (!parsed.has_value())
        return Error::from_string_literal("Invalid origin URL");

    auto scheme = parsed->scheme();
    auto host = parsed->serialized_host();
    if (host.is_empty())
        return Error::from_string_literal("Origin URL must have a valid host");

    bool is_local = host.equals_ignoring_ascii_case("localhost"sv) || host == "127.0.0.1"sv || host == "[::1]"sv;
    if (scheme != "https"sv && !(scheme == "http"sv && is_local))
        return Error::from_string_literal("Vault origins must use https:// (or http://localhost / http://127.0.0.1)");

    return parsed->origin().serialize();
}

bool vault_credential_matches_origin(VaultCredential const& cred, URL::URL const& url)
{
    if (!cred.enabled)
        return false;
    auto scheme = url.scheme();
    if (scheme != "http"sv && scheme != "https"sv)
        return false;
    auto serialized = url.origin().serialize();
    if (serialized.is_empty() || serialized == "null"sv)
        return false;
    for (auto const& origin : cred.allowed_origins) {
        if (origin == serialized)
            return true;
    }
    return false;
}

bool vault_credential_matches_container(VaultCredential const& cred, StringView tab_container_id)
{
    if (cred.container_id.is_empty())
        return true;
    return cred.container_id == tab_container_id;
}

static ErrorOr<ByteBuffer> decode_base32_rfc4648(StringView secret)
{
    u32 buffer = 0;
    u32 bits_left = 0;
    ByteBuffer out;

    for (auto raw_byte : secret.bytes()) {
        if (raw_byte == ' ' || raw_byte == '-' || raw_byte == '\t' || raw_byte == '\r' || raw_byte == '\n' || raw_byte == '=')
            continue;

        auto ch = static_cast<char>(to_ascii_uppercase(raw_byte));
        u32 val = 0;
        if (ch >= 'A' && ch <= 'Z')
            val = static_cast<u32>(ch - 'A');
        else if (ch >= '2' && ch <= '7')
            val = static_cast<u32>(26 + (ch - '2'));
        else
            return Error::from_string_literal("Invalid Base32 character in TOTP secret");

        buffer = (buffer << 5) | val;
        bits_left += 5;
        if (bits_left >= 8) {
            bits_left -= 8;
            TRY(out.try_append(static_cast<u8>((buffer >> bits_left) & 0xff)));
        }
    }

    if (out.is_empty())
        return Error::from_string_literal("TOTP secret decoded to empty key");

    return out;
}

ErrorOr<String> generate_totp_code(StringView base32_secret, Optional<i64> unix_seconds)
{
    auto key = TRY(decode_base32_rfc4648(base32_secret));
    i64 now = unix_seconds.value_or(UnixDateTime::now().seconds_since_epoch());
    if (now < 0)
        now = 0;

    u64 counter = static_cast<u64>(now) / 30ULL;
    u8 msg[8];
    for (int i = 7; i >= 0; --i) {
        msg[i] = static_cast<u8>(counter & 0xffU);
        counter >>= 8;
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    if (!HMAC(EVP_sha1(), key.data(), static_cast<int>(key.size()), msg, sizeof(msg), digest, &digest_len) || digest_len < 20)
        return Error::from_string_literal("Failed computing HMAC-SHA1 for TOTP");

    size_t offset = static_cast<size_t>(digest[digest_len - 1] & 0x0f);
    u32 binary = ((static_cast<u32>(digest[offset] & 0x7f) << 24)
        | (static_cast<u32>(digest[offset + 1] & 0xff) << 16)
        | (static_cast<u32>(digest[offset + 2] & 0xff) << 8)
        | static_cast<u32>(digest[offset + 3] & 0xff));
    u32 otp = binary % 1'000'000U;
    return String::formatted("{:06}", otp);
}

ErrorOr<EncryptedVaultBlob> vault_encrypt_aes_256_gcm(ReadonlyBytes key_32, ReadonlyBytes plaintext, ReadonlyBytes aad)
{
    if (key_32.size() != 32)
        return Error::from_string_literal("AES-256-GCM requires a 32-byte key");

    EncryptedVaultBlob blob;
    blob.nonce = TRY(ByteBuffer::create_uninitialized(12));
    if (RAND_bytes(blob.nonce.data(), 12) != 1)
        return Error::from_string_literal("Failed generating AES-GCM nonce");

    blob.tag = TRY(ByteBuffer::create_uninitialized(16));
    blob.ciphertext = TRY(ByteBuffer::create_uninitialized(plaintext.size() + 16));

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return Error::from_string_literal("EVP_CIPHER_CTX_new failed");
    ScopeGuard free_ctx = [ctx] { EVP_CIPHER_CTX_free(ctx); };

    if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return Error::from_string_literal("EVP_EncryptInit_ex(aes-256-gcm) failed");
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1)
        return Error::from_string_literal("EVP_CTRL_GCM_SET_IVLEN failed");
    if (EVP_EncryptInit_ex(ctx, nullptr, nullptr, key_32.data(), blob.nonce.data()) != 1)
        return Error::from_string_literal("EVP_EncryptInit_ex(key/iv) failed");

    int len = 0;
    if (!aad.is_empty()) {
        if (EVP_EncryptUpdate(ctx, nullptr, &len, aad.data(), static_cast<int>(aad.size())) != 1)
            return Error::from_string_literal("EVP_EncryptUpdate(aad) failed");
    }

    int total_len = 0;
    if (!plaintext.is_empty()) {
        if (EVP_EncryptUpdate(ctx, blob.ciphertext.data(), &len, plaintext.data(), static_cast<int>(plaintext.size())) != 1)
            return Error::from_string_literal("EVP_EncryptUpdate failed");
        total_len += len;
    }

    if (EVP_EncryptFinal_ex(ctx, blob.ciphertext.data() + total_len, &len) != 1)
        return Error::from_string_literal("EVP_EncryptFinal_ex failed");
    total_len += len;

    blob.ciphertext.resize(static_cast<size_t>(total_len));
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, blob.tag.data()) != 1)
        return Error::from_string_literal("EVP_CTRL_GCM_GET_TAG failed");

    return blob;
}

ErrorOr<ByteBuffer> vault_decrypt_aes_256_gcm(ReadonlyBytes key_32, ReadonlyBytes nonce_12, ReadonlyBytes ciphertext, ReadonlyBytes tag_16, ReadonlyBytes aad)
{
    if (key_32.size() != 32 || nonce_12.size() != 12 || tag_16.size() != 16)
        return Error::from_string_literal("Invalid AES-256-GCM key, nonce, or tag size");

    auto plaintext = TRY(ByteBuffer::create_uninitialized(ciphertext.size() + 16));

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx)
        return Error::from_string_literal("EVP_CIPHER_CTX_new failed");
    ScopeGuard free_ctx = [ctx] { EVP_CIPHER_CTX_free(ctx); };

    if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1)
        return Error::from_string_literal("EVP_DecryptInit_ex(aes-256-gcm) failed");
    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) != 1)
        return Error::from_string_literal("EVP_CTRL_GCM_SET_IVLEN failed");
    if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, key_32.data(), nonce_12.data()) != 1)
        return Error::from_string_literal("EVP_DecryptInit_ex(key/iv) failed");

    int len = 0;
    if (!aad.is_empty()) {
        if (EVP_DecryptUpdate(ctx, nullptr, &len, aad.data(), static_cast<int>(aad.size())) != 1)
            return Error::from_string_literal("EVP_DecryptUpdate(aad) failed");
    }

    int total_len = 0;
    if (!ciphertext.is_empty()) {
        if (EVP_DecryptUpdate(ctx, plaintext.data(), &len, ciphertext.data(), static_cast<int>(ciphertext.size())) != 1)
            return Error::from_string_literal("EVP_DecryptUpdate failed");
        total_len += len;
    }

    if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<u8*>(tag_16.data())) != 1)
        return Error::from_string_literal("EVP_CTRL_GCM_SET_TAG failed");

    if (EVP_DecryptFinal_ex(ctx, plaintext.data() + total_len, &len) <= 0)
        return Error::from_string_literal("Vault decryption failed: authentication tag mismatch or wrong passphrase");
    total_len += len;

    plaintext.resize(static_cast<size_t>(total_len));
    return plaintext;
}

ErrorOr<ByteBuffer> vault_derive_key_pbkdf2(StringView passphrase, ReadonlyBytes salt_16, u32 iterations)
{
    if (passphrase.is_empty() || salt_16.is_empty() || iterations < 100'000 || iterations > 10'000'000)
        return Error::from_string_literal("Invalid PBKDF2 parameters");

    auto key = TRY(ByteBuffer::create_uninitialized(32));
    if (PKCS5_PBKDF2_HMAC(
            passphrase.characters_without_null_termination(),
            static_cast<int>(passphrase.length()),
            salt_16.data(),
            static_cast<int>(salt_16.size()),
            static_cast<int>(iterations),
            EVP_sha256(),
            32,
            key.data())
        != 1) {
        return Error::from_string_literal("PKCS5_PBKDF2_HMAC failed");
    }
    return key;
}

static String replace_all_occurrences(StringView input, StringView needle, StringView replacement)
{
    if (needle.is_empty() || input.is_empty() || !input.contains(needle))
        return String::from_utf8_with_replacement_character(input, String::WithBOMHandling::No);
    auto bs = ByteString { input }.replace(needle, replacement, ReplaceMode::All);
    return String::from_utf8_with_replacement_character(bs.view(), String::WithBOMHandling::No);
}

String scrub_secrets_from_string(StringView input, ReadonlySpan<String> secrets)
{
    if (input.is_empty() || secrets.is_empty())
        return String::from_utf8_with_replacement_character(input, String::WithBOMHandling::No);

    static constexpr auto redaction = "[REDACTED_VAULT_SECRET]"sv;
    String current = String::from_utf8_with_replacement_character(input, String::WithBOMHandling::No);
    for (auto const& secret : secrets) {
        if (secret.bytes().size() < 4)
            continue;
        current = replace_all_occurrences(current, secret, redaction);
        auto url_encoded = URL::percent_encode(secret, URL::PercentEncodeSet::Component);
        if (url_encoded != secret)
            current = replace_all_occurrences(current, url_encoded, redaction);
        auto form_encoded = URL::percent_encode(secret, URL::PercentEncodeSet::ApplicationXWWWFormUrlencoded);
        if (form_encoded != secret && form_encoded != url_encoded)
            current = replace_all_occurrences(current, form_encoded, redaction);
        auto form_plus_encoded = URL::percent_encode(secret, URL::PercentEncodeSet::ApplicationXWWWFormUrlencoded, URL::SpaceAsPlus::Yes);
        if (form_plus_encoded != secret && form_plus_encoded != url_encoded && form_plus_encoded != form_encoded)
            current = replace_all_occurrences(current, form_plus_encoded, redaction);
        if (auto b64 = encode_base64(secret.bytes()); !b64.is_error() && b64.value().bytes().size() >= 6)
            current = replace_all_occurrences(current, b64.value(), redaction);
    }
    return current;
}

bool mcp_domain_matches_pattern(StringView host, StringView pattern)
{
    auto pat = pattern.trim(" \t\r\n"sv);
    while (pat.ends_with('.'))
        pat = pat.substring_view(0, pat.length() - 1);
    while (host.ends_with('.'))
        host = host.substring_view(0, host.length() - 1);
    if (pat.is_empty() || host.is_empty())
        return false;
    if (pat == "*"sv)
        return true;
    if (pat.starts_with("*."sv)) {
        auto base = pat.substring_view(2);
        if (base.is_empty() || base.contains('*'))
            return false;
        if (host.equals_ignoring_ascii_case(base))
            return true;
        return host.length() > base.length() + 1
            && host.substring_view(host.length() - base.length()).equals_ignoring_ascii_case(base)
            && host[host.length() - base.length() - 1] == '.';
    }
    return host.equals_ignoring_ascii_case(pat);
}

static bool is_private_ipv4_octets(u8 a, u8 b)
{
    if (a == 0 || a == 10 || a == 127)
        return true;
    if (a == 100 && b >= 64 && b <= 127) // CGNAT 100.64.0.0/10
        return true;
    if (a == 169 && b == 254) // Link-local & cloud metadata 169.254.0.0/16
        return true;
    if (a == 172 && b >= 16 && b <= 31) // RFC1918 172.16.0.0/12
        return true;
    if (a == 192 && b == 168) // RFC1918 192.168.0.0/16
        return true;
    if (a >= 224) // Multicast & reserved 224.0.0.0/4+
        return true;
    return false;
}

bool mcp_host_is_private_or_internal(StringView host)
{
    auto clean = host.trim_whitespace();
    while (clean.ends_with('.'))
        clean = clean.substring_view(0, clean.length() - 1);
    if (clean.starts_with('[') && clean.ends_with(']') && clean.length() >= 2)
        clean = clean.substring_view(1, clean.length() - 2);
    if (clean.is_empty())
        return true;

    if (clean.equals_ignoring_ascii_case("localhost"sv)
        || clean.ends_with(".localhost"sv, CaseSensitivity::CaseInsensitive)
        || clean.ends_with(".local"sv, CaseSensitivity::CaseInsensitive)
        || clean.ends_with(".internal"sv, CaseSensitivity::CaseInsensitive)
        || clean.ends_with(".home.arpa"sv, CaseSensitivity::CaseInsensitive))
        return true;

    if (auto ipv4 = IPv4Address::from_string(clean); ipv4.has_value())
        return is_private_ipv4_octets((*ipv4)[0], (*ipv4)[1]);

    if (auto ipv6 = IPv6Address::from_string(clean); ipv6.has_value()) {
        if (ipv6->is_zero())
            return true;
        if (ipv6->is_ipv4_mapped()) {
            auto p6 = (*ipv6)[6];
            return is_private_ipv4_octets(static_cast<u8>(p6 >> 8), static_cast<u8>(p6 & 0xff));
        }
        bool is_loopback = true;
        for (size_t i = 0; i < 7; ++i) {
            if ((*ipv6)[i] != 0) {
                is_loopback = false;
                break;
            }
        }
        if (is_loopback && (*ipv6)[7] == 1)
            return true;
        auto p0 = (*ipv6)[0];
        if ((p0 & 0xfe00) == 0xfc00) // Unique local fc00::/7
            return true;
        if ((p0 & 0xffc0) == 0xfe80) // Link-local fe80::/10
            return true;
        if ((p0 & 0xff00) == 0xff00) // Multicast ff00::/8
            return true;
    }

    return false;
}

ErrorOr<void> mcp_validate_url_against_policy(URL::URL const& url, MCPSecurityPolicy const& policy)
{
    if (url == URL::about_blank())
        return {};

    auto scheme = url.scheme();
    if (scheme != "http"sv && scheme != "https"sv)
        return Error::from_string_literal("URL scheme blocked by MCP policy");

    auto host = url.serialized_host();
    if (host.is_empty())
        return Error::from_string_literal("URL has empty host");

    for (auto const& blocked : policy.blocked_domains) {
        if (mcp_domain_matches_pattern(host, blocked))
            return Error::from_string_literal("Navigation blocked by MCP domain denylist policy (--mcp-blocked-domains)");
    }

    bool explicitly_allowlisted = false;
    if (!policy.allowed_domains.is_empty()) {
        bool allowed = false;
        for (auto const& pat : policy.allowed_domains) {
            if (mcp_domain_matches_pattern(host, pat)) {
                allowed = true;
                if (pat.bytes_as_string_view().trim_whitespace() != "*"sv)
                    explicitly_allowlisted = true;
                break;
            }
        }
        if (!allowed)
            return Error::from_string_literal("Navigation blocked by MCP domain allowlist policy (--mcp-allowed-domains)");
    }

    if (!policy.allow_private_network && !explicitly_allowlisted && mcp_host_is_private_or_internal(host))
        return Error::from_string_literal("Navigation to private/local network address blocked by MCP SSRF protection (--mcp-allow-private-network or --mcp-allowed-domains required)");

    return {};
}

ErrorOr<String> mcp_resolve_container_against_policy(Optional<String> requested_container, MCPSecurityPolicy const& policy)
{
    String requested = requested_container.value_or(String {});
    if (policy.allowed_containers.is_empty())
        return requested.is_empty() ? "agent"_string : requested;

    if (requested.is_empty())
        return policy.allowed_containers.first();

    for (auto const& allowed : policy.allowed_containers) {
        if (allowed == requested)
            return requested;
    }

    return Error::from_string_literal("Tab container rejected by MCP container isolation policy (--mcp-allowed-containers)");
}

bool mcp_container_is_allowed_by_policy(StringView container_id, MCPSecurityPolicy const& policy)
{
    if (policy.allowed_containers.is_empty())
        return true;
    for (auto const& allowed : policy.allowed_containers) {
        if (allowed == container_id)
            return true;
    }
    return false;
}

Vector<String> vault_credential_available_fields(VaultCredential const& cred)
{
    Vector<String> fields;
    if (!cred.username.is_empty())
        fields.append("username"_string);
    if (!cred.password.is_empty())
        fields.append("password"_string);
    if (!cred.totp_secret.is_empty())
        fields.append("totp"_string);
    return fields;
}

JsonObject serialize_vault_credential_metadata(VaultCredential const& cred)
{
    JsonObject obj;
    obj.set("id"sv, cred.id);
    obj.set("label"sv, cred.label);
    JsonArray origins_json;
    for (auto const& o : cred.allowed_origins)
        origins_json.must_append(o);
    obj.set("allowed_origins"sv, move(origins_json));
    obj.set("container_id"sv, cred.container_id);
    JsonArray fields_json;
    for (auto const& f : vault_credential_available_fields(cred))
        fields_json.must_append(f);
    obj.set("fields"sv, move(fields_json));
    obj.set("enabled"sv, cred.enabled);
    obj.set("last_used_at_ms"sv, cred.last_used_at_ms);
    return obj;
}

JsonObject serialize_vault_credential_hint(VaultCredential const& cred)
{
    JsonObject hint;
    hint.set("id"sv, cred.id);
    hint.set("label"sv, cred.label);
    JsonArray fields_json;
    for (auto const& f : vault_credential_available_fields(cred))
        fields_json.must_append(f);
    hint.set("fields"sv, move(fields_json));
    return hint;
}

JsonObject serialize_vault_credential_for_ui(VaultCredential const& cred)
{
    JsonObject obj;
    obj.set("id"sv, cred.id);
    obj.set("label"sv, cred.label);
    JsonArray origins_json;
    for (auto const& o : cred.allowed_origins)
        origins_json.must_append(o);
    obj.set("allowedOrigins"sv, move(origins_json));
    obj.set("containerId"sv, cred.container_id);
    obj.set("username"sv, cred.username);
    obj.set("hasPassword"sv, !cred.password.is_empty());
    obj.set("hasTotp"sv, !cred.totp_secret.is_empty());
    obj.set("notes"sv, cred.notes);
    obj.set("enabled"sv, cred.enabled);
    obj.set("createdAtMs"sv, cred.created_at_ms);
    obj.set("updatedAtMs"sv, cred.updated_at_ms);
    obj.set("lastUsedAtMs"sv, cred.last_used_at_ms);
    return obj;
}

JsonObject serialize_vault_audit_entry(VaultAuditEntry const& entry)
{
    JsonObject obj;
    obj.set("timestampMs"sv, entry.timestamp_ms);
    obj.set("action"sv, entry.action);
    obj.set("credentialId"sv, entry.credential_id);
    obj.set("origin"sv, entry.origin);
    obj.set("field"sv, entry.field);
    obj.set("ok"sv, entry.ok);
    obj.set("detail"sv, entry.detail);
    return obj;
}

ErrorOr<VaultCredential> parse_vault_credential_ui_json(JsonValue const& value)
{
    if (!value.is_object())
        return Error::from_string_literal("Expected JSON object for vault credential");

    auto const& obj = value.as_object();
    auto id = obj.get_string("id"sv);
    if (!id.has_value() || !is_valid_vault_credential_id(*id))
        return Error::from_string_literal("Invalid credential 'id' (expected 1-64 lowercase a-z0-9 and hyphens)");

    auto label = obj.get_string("label"sv).value_or(String {});
    auto trimmed_label = label.bytes_as_string_view().trim(" \t\r\n"sv);
    if (trimmed_label.is_empty())
        trimmed_label = *id;

    VaultCredential cred;
    cred.id = *id;
    cred.label = MUST(String::from_utf8(trimmed_label));
    cred.enabled = obj.get_bool("enabled"sv).value_or(true);
    cred.username = obj.get_string("username"sv).value_or(String {});
    cred.password = obj.get_string("password"sv).value_or(String {});
    cred.totp_secret = obj.get_string("totpSecret"sv).value_or(String {});
    cred.notes = obj.get_string("notes"sv).value_or(String {});

    if (auto container_id = obj.get_string("containerId"sv); container_id.has_value() && !container_id->is_empty()) {
        if (!is_valid_container_id(*container_id))
            return Error::from_string_literal("Invalid 'containerId'");
        cred.container_id = *container_id;
    }

    auto origins_arr = obj.get_array("allowedOrigins"sv);
    if (!origins_arr.has_value() || origins_arr->is_empty())
        return Error::from_string_literal("At least one exact allowed origin (e.g. https://github.com) is required");

    for (auto const& item : origins_arr->values()) {
        if (!item.is_string())
            return Error::from_string_literal("Each allowed origin must be a string");
        auto normalized = TRY(normalize_vault_origin(item.as_string()));
        if (!cred.allowed_origins.contains_slow(normalized))
            cred.allowed_origins.append(move(normalized));
    }

    if (!cred.totp_secret.is_empty())
        (void)TRY(generate_totp_code(cred.totp_secret, 0));

    return cred;
}

}
