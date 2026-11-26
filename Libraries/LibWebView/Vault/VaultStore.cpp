/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Vault/VaultStore.h>
#include <cstring>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdlib.h>

#if defined(AK_OS_WINDOWS)
#    include <windows.h>
#    include <dpapi.h>
#endif

namespace WebView {

static constexpr StringView VAULT_AAD = "ladybird-vault-v1"sv;
static constexpr u32 VAULT_PBKDF2_ITERATIONS = 210000;
static constexpr size_t MAX_AUDIT_ENTRIES = 200;

static ErrorOr<void> write_bytes_file(ByteString const& path, ReadonlyBytes bytes, mode_t mode = 0600)
{
    auto parent = LexicalPath { path }.parent();
    TRY(Core::Directory::create(parent, Core::Directory::CreateDirectories::Yes));
    auto tmp_path = ByteString::formatted("{}.tmp", path);
    bool moved = false;
    ScopeGuard cleanup_tmp = [&] {
        if (!moved)
            (void)FileSystem::remove(tmp_path, FileSystem::RecursionMode::Disallowed);
    };
    {
        auto file = TRY(Core::File::open(tmp_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate, mode));
        TRY(file->write_until_depleted(bytes));
    }
    TRY(FileSystem::move_file(path, tmp_path));
    moved = true;
    return {};
}

VaultStore::VaultStore(ByteString storage_directory, Optional<String> passphrase)
    : m_storage_directory(move(storage_directory))
    , m_passphrase(move(passphrase))
{
    if (!m_passphrase.has_value() || m_passphrase->is_empty()) {
        if (char const* env_pass = getenv("LADYBIRD_VAULT_PASSPHRASE"); env_pass && env_pass[0] != '\0')
            m_passphrase = String::from_utf8_without_validation(ReadonlyBytes { reinterpret_cast<u8 const*>(env_pass), strlen(env_pass) });
    }
    reload();
}

VaultStore::~VaultStore()
{
    if (!m_master_key.is_empty())
        OPENSSL_cleanse(m_master_key.data(), m_master_key.size());
    if (!m_kdf_salt.is_empty())
        OPENSSL_cleanse(m_kdf_salt.data(), m_kdf_salt.size());
    m_passphrase.clear();
    for (auto& [_, cred] : m_credentials) {
        if (!cred.password.bytes().is_empty())
            cred.password = {};
        if (!cred.totp_secret.bytes().is_empty())
            cred.totp_secret = {};
    }
}

ByteString VaultStore::vault_file_path() const
{
    return LexicalPath::join(m_storage_directory, "vault.enc.json"sv).string();
}

ByteString VaultStore::master_key_dpapi_path() const
{
    return LexicalPath::join(m_storage_directory, "master.dpapi"sv).string();
}

ByteString VaultStore::master_key_file_path() const
{
    return LexicalPath::join(m_storage_directory, "master.key"sv).string();
}

ByteString VaultStore::audit_log_path() const
{
    return LexicalPath::join(m_storage_directory, "audit.jsonl"sv).string();
}

static ErrorOr<ByteBuffer> load_or_create_platform_master_key(ByteString const& dpapi_path, ByteString const& key_file_path, String& out_mode)
{
#if defined(AK_OS_WINDOWS)
    (void)key_file_path;
    out_mode = "Windows DPAPI + AES-256-GCM"_string;
    if (FileSystem::exists(dpapi_path)) {
        auto file = TRY(Core::File::open(dpapi_path, Core::File::OpenMode::Read));
        auto wrapped = TRY(file->read_until_eof());
        DATA_BLOB in_blob { static_cast<DWORD>(wrapped.size()), wrapped.data() };
        DATA_BLOB out_blob {};
        if (!CryptUnprotectData(&in_blob, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out_blob))
            return Error::from_string_literal("Windows DPAPI CryptUnprotectData failed for vault master key");
        ScopeGuard free_blob = [&] { LocalFree(out_blob.pbData); };
        auto key = TRY(ByteBuffer::copy(out_blob.pbData, out_blob.cbData));
        OPENSSL_cleanse(out_blob.pbData, out_blob.cbData);
        if (key.size() != 32)
            return Error::from_string_literal("Unwrapped DPAPI master key has invalid length");
        return key;
    }

    auto key = TRY(ByteBuffer::create_uninitialized(32));
    if (RAND_bytes(key.data(), 32) != 1)
        return Error::from_string_literal("RAND_bytes failed generating DPAPI master key");

    DATA_BLOB in_blob { static_cast<DWORD>(key.size()), key.data() };
    DATA_BLOB out_blob {};
    if (!CryptProtectData(&in_blob, L"Ladybird Credential Vault Master Key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out_blob))
        return Error::from_string_literal("Windows DPAPI CryptProtectData failed");
    ScopeGuard free_blob = [&] { LocalFree(out_blob.pbData); };
    TRY(write_bytes_file(dpapi_path, ReadonlyBytes { out_blob.pbData, out_blob.cbData }, 0600));
    return key;
#else
    (void)dpapi_path;
    out_mode = "Local Key File (0600) + AES-256-GCM"_string;
    if (FileSystem::exists(key_file_path)) {
        auto file = TRY(Core::File::open(key_file_path, Core::File::OpenMode::Read));
        auto key = TRY(file->read_until_eof());
        if (key.size() != 32)
            return Error::from_string_literal("Vault master.key file has invalid length");
        return key;
    }
    auto key = TRY(ByteBuffer::create_uninitialized(32));
    if (RAND_bytes(key.data(), 32) != 1)
        return Error::from_string_literal("RAND_bytes failed generating master key");
    TRY(write_bytes_file(key_file_path, key.bytes(), 0600));
    return key;
#endif
}

ErrorOr<void> VaultStore::ensure_master_key_for_save()
{
    if (!m_master_key.is_empty())
        return {};

    if (m_passphrase.has_value() && !m_passphrase->is_empty()) {
        m_key_mode_name = "PBKDF2-HMAC-SHA256 + AES-256-GCM"_string;
        if (m_kdf_salt.is_empty()) {
            m_kdf_salt = TRY(ByteBuffer::create_uninitialized(16));
            if (RAND_bytes(m_kdf_salt.data(), 16) != 1)
                return Error::from_string_literal("RAND_bytes failed generating PBKDF2 salt");
        }
        m_master_key = TRY(vault_derive_key_pbkdf2(*m_passphrase, m_kdf_salt.bytes(), VAULT_PBKDF2_ITERATIONS));
        return {};
    }

    m_master_key = TRY(load_or_create_platform_master_key(master_key_dpapi_path(), master_key_file_path(), m_key_mode_name));
    return {};
}

ErrorOr<void> VaultStore::load_encrypted_vault()
{
    auto path = vault_file_path();
    if (!FileSystem::exists(path)) {
        if (m_passphrase.has_value() && !m_passphrase->is_empty()) {
            m_key_mode_name = "PBKDF2-HMAC-SHA256 + AES-256-GCM"_string;
        } else {
#if defined(AK_OS_WINDOWS)
            m_key_mode_name = "Windows DPAPI + AES-256-GCM"_string;
#else
            m_key_mode_name = "Local Key File (0600) + AES-256-GCM"_string;
#endif
        }
        m_unlocked = true;
        return {};
    }

    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
    auto raw = TRY(file->read_until_eof());
    auto json = TRY(JsonValue::from_string(raw));
    if (!json.is_object())
        return Error::from_string_literal("vault.enc.json must be a JSON object");

    auto const& obj = json.as_object();
    auto kdf = obj.get_string("kdf"sv).value_or("platform"_string);
    auto nonce_b64 = obj.get_string("nonce_b64"sv);
    auto tag_b64 = obj.get_string("tag_b64"sv);
    auto ct_b64 = obj.get_string("ciphertext_b64"sv);
    if (!nonce_b64.has_value() || !tag_b64.has_value() || !ct_b64.has_value())
        return Error::from_string_literal("Malformed vault.enc.json envelope");

    auto nonce = TRY(decode_base64(*nonce_b64));
    auto tag = TRY(decode_base64(*tag_b64));
    auto ciphertext = TRY(decode_base64(*ct_b64));

    if (kdf == "pbkdf2-sha256"sv) {
        m_key_mode_name = "PBKDF2-HMAC-SHA256 + AES-256-GCM"_string;
        if (!m_passphrase.has_value() || m_passphrase->is_empty())
            return Error::from_string_literal("Vault is passphrase-protected; provide --mcp-vault-passphrase or LADYBIRD_VAULT_PASSPHRASE");
        auto salt_b64 = obj.get_string("salt_b64"sv);
        if (!salt_b64.has_value())
            return Error::from_string_literal("Missing salt_b64 in passphrase-encrypted vault");
        m_kdf_salt = TRY(decode_base64(*salt_b64));
        u32 iterations = obj.get_integer<u32>("iterations"sv).value_or(VAULT_PBKDF2_ITERATIONS);
        if (iterations < 100'000 || iterations > 10'000'000)
            return Error::from_string_literal("Invalid PBKDF2 iteration count in vault");
        m_master_key = TRY(vault_derive_key_pbkdf2(*m_passphrase, m_kdf_salt.bytes(), iterations));
    } else {
        TRY(ensure_master_key_for_save());
    }

    auto plaintext = TRY(vault_decrypt_aes_256_gcm(m_master_key.bytes(), nonce.bytes(), ciphertext.bytes(), tag.bytes(), VAULT_AAD.bytes()));
    ScopeGuard cleanse_plain = [&] {
        if (!plaintext.is_empty())
            OPENSSL_cleanse(plaintext.data(), plaintext.size());
    };

    auto payload_json = TRY(JsonValue::from_string(StringView { plaintext }));
    if (!payload_json.is_object())
        return Error::from_string_literal("Decrypted vault payload is not a JSON object");

    auto creds_arr = payload_json.as_object().get_array("credentials"sv);
    if (!creds_arr.has_value())
        return Error::from_string_literal("Decrypted vault payload missing 'credentials' array");

    for (auto const& val : creds_arr->values()) {
        if (!val.is_object())
            continue;
        auto const& cobj = val.as_object();
        auto id = cobj.get_string("id"sv);
        if (!id.has_value() || !is_valid_vault_credential_id(*id))
            continue;

        VaultCredential cred;
        cred.id = *id;
        cred.label = cobj.get_string("label"sv).value_or(*id);
        cred.container_id = cobj.get_string("container_id"sv).value_or(String {});
        cred.username = cobj.get_string("username"sv).value_or(String {});
        cred.password = cobj.get_string("password"sv).value_or(String {});
        cred.totp_secret = cobj.get_string("totp_secret"sv).value_or(String {});
        cred.notes = cobj.get_string("notes"sv).value_or(String {});
        cred.enabled = cobj.get_bool("enabled"sv).value_or(true);
        cred.created_at_ms = cobj.get_integer<i64>("created_at_ms"sv).value_or(0);
        cred.updated_at_ms = cobj.get_integer<i64>("updated_at_ms"sv).value_or(0);
        cred.last_used_at_ms = cobj.get_integer<i64>("last_used_at_ms"sv).value_or(0);
        if (auto origins = cobj.get_array("allowed_origins"sv); origins.has_value()) {
            for (auto const& o : origins->values()) {
                if (o.is_string()) {
                    if (auto norm = normalize_vault_origin(o.as_string()); !norm.is_error())
                        cred.allowed_origins.append(norm.release_value());
                }
            }
        }
        auto cred_id = cred.id;
        m_credentials.set(move(cred_id), move(cred));
    }

    m_unlocked = true;
    return {};
}

ErrorOr<void> VaultStore::save_encrypted_vault()
{
    if (!m_unlocked)
        return Error::from_string_literal("Vault is locked");

    TRY(ensure_master_key_for_save());

    JsonArray creds_arr;
    for (auto const& cred : list_credentials(true)) {
        JsonObject cobj;
        cobj.set("id"sv, cred.id);
        cobj.set("label"sv, cred.label);
        JsonArray origins;
        for (auto const& o : cred.allowed_origins)
            origins.must_append(o);
        cobj.set("allowed_origins"sv, move(origins));
        cobj.set("container_id"sv, cred.container_id);
        cobj.set("username"sv, cred.username);
        cobj.set("password"sv, cred.password);
        cobj.set("totp_secret"sv, cred.totp_secret);
        cobj.set("notes"sv, cred.notes);
        cobj.set("enabled"sv, cred.enabled);
        cobj.set("created_at_ms"sv, cred.created_at_ms);
        cobj.set("updated_at_ms"sv, cred.updated_at_ms);
        cobj.set("last_used_at_ms"sv, cred.last_used_at_ms);
        creds_arr.must_append(move(cobj));
    }

    JsonObject payload;
    payload.set("credentials"sv, move(creds_arr));
    auto serialized = payload.serialized();
    auto blob = TRY(vault_encrypt_aes_256_gcm(m_master_key.bytes(), serialized.bytes(), VAULT_AAD.bytes()));

    JsonObject envelope;
    envelope.set("version"sv, 1);
    if (m_passphrase.has_value() && !m_passphrase->is_empty()) {
        envelope.set("kdf"sv, "pbkdf2-sha256"sv);
        envelope.set("iterations"sv, VAULT_PBKDF2_ITERATIONS);
        envelope.set("salt_b64"sv, TRY(encode_base64(m_kdf_salt.bytes())));
    } else {
        envelope.set("kdf"sv, "platform"sv);
    }
    envelope.set("nonce_b64"sv, TRY(encode_base64(blob.nonce.bytes())));
    envelope.set("tag_b64"sv, TRY(encode_base64(blob.tag.bytes())));
    envelope.set("ciphertext_b64"sv, TRY(encode_base64(blob.ciphertext.bytes())));

    auto envelope_str = envelope.serialized();
    TRY(write_bytes_file(vault_file_path(), envelope_str.bytes(), 0600));
    return {};
}

void VaultStore::load_audit_log()
{
    m_audit.clear();
    auto path = audit_log_path();
    if (!FileSystem::exists(path))
        return;

    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return;
    auto raw = file.value()->read_until_eof();
    if (raw.is_error())
        return;

    StringView view { raw.value().bytes() };
    for (auto line : view.split_view('\n', SplitBehavior::Nothing)) {
        auto trimmed = line.trim(" \t\r"sv);
        if (trimmed.is_empty())
            continue;
        auto parsed = JsonValue::from_string(trimmed);
        if (parsed.is_error() || !parsed.value().is_object())
            continue;
        auto const& obj = parsed.value().as_object();
        VaultAuditEntry entry;
        entry.timestamp_ms = obj.get_integer<i64>("timestampMs"sv).value_or(0);
        entry.action = obj.get_string("action"sv).value_or(String {});
        entry.credential_id = obj.get_string("credentialId"sv).value_or(String {});
        entry.origin = obj.get_string("origin"sv).value_or(String {});
        entry.field = obj.get_string("field"sv).value_or(String {});
        entry.ok = obj.get_bool("ok"sv).value_or(false);
        entry.detail = obj.get_string("detail"sv).value_or(String {});
        m_audit.append(move(entry));
    }
    if (m_audit.size() > MAX_AUDIT_ENTRIES) {
        size_t excess = m_audit.size() - MAX_AUDIT_ENTRIES;
        m_audit.remove(0, excess);
    }
}

void VaultStore::append_audit_to_disk(VaultAuditEntry const& entry) const
{
    auto path = audit_log_path();
    auto parent = LexicalPath { path }.parent();
    if (Core::Directory::create(parent, Core::Directory::CreateDirectories::Yes).is_error())
        return;
    auto file = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Append, 0600);
    if (file.is_error())
        return;
    auto line = MUST(String::formatted("{}\n", serialize_vault_audit_entry(entry).serialized()));
    (void)file.value()->write_until_depleted(line.bytes());
}

void VaultStore::reload()
{
    m_credentials.clear();
    m_last_error.clear();
    m_unlocked = false;

    if (!m_master_key.is_empty()) {
        OPENSSL_cleanse(m_master_key.data(), m_master_key.size());
        m_master_key.clear();
    }

    if (auto res = load_encrypted_vault(); res.is_error()) {
        m_last_error = MUST(String::formatted("{}", res.error()));
        warnln("VaultStore unlock failed: {}", *m_last_error);
        m_unlocked = false;
    }

    load_audit_log();
}

Vector<VaultCredential> VaultStore::list_credentials(bool include_disabled) const
{
    Vector<VaultCredential> creds;
    creds.ensure_capacity(m_credentials.size());
    for (auto const& [_, cred] : m_credentials) {
        if (include_disabled || cred.enabled)
            creds.unchecked_append(cred);
    }
    quick_sort(creds, [](VaultCredential const& a, VaultCredential const& b) {
        return a.id < b.id;
    });
    return creds;
}

Vector<VaultCredential> VaultStore::matching_credentials(URL::URL const& url, StringView tab_container_id) const
{
    if (!m_unlocked)
        return {};
    Vector<VaultCredential> matches;
    for (auto const& [_, cred] : m_credentials) {
        if (vault_credential_matches_origin(cred, url) && vault_credential_matches_container(cred, tab_container_id))
            matches.append(cred);
    }
    quick_sort(matches, [](VaultCredential const& a, VaultCredential const& b) {
        return a.id < b.id;
    });
    return matches;
}

Optional<VaultCredential> VaultStore::get_credential(StringView id, bool include_disabled) const
{
    if (!is_valid_vault_credential_id(id))
        return {};
    auto found = m_credentials.get(id);
    if (!found.has_value())
        return {};
    if (!include_disabled && !found->enabled)
        return {};
    return *found;
}

ErrorOr<VaultCredential> VaultStore::save_credential(VaultCredential cred, bool preserve_existing_secrets_if_empty, bool clear_password, bool clear_totp)
{
    if (!m_unlocked)
        return Error::from_string_literal("Vault is locked");
    if (!is_valid_vault_credential_id(cred.id))
        return Error::from_string_literal("Invalid credential 'id'");
    if (cred.allowed_origins.is_empty())
        return Error::from_string_literal("At least one allowed origin is required");

    Vector<String> normalized_origins;
    for (auto const& raw_origin : cred.allowed_origins) {
        auto norm = TRY(normalize_vault_origin(raw_origin));
        if (!normalized_origins.contains_slow(norm))
            normalized_origins.append(move(norm));
    }
    cred.allowed_origins = move(normalized_origins);

    if (!cred.totp_secret.is_empty())
        (void)TRY(generate_totp_code(cred.totp_secret, 0));

    auto now_ms = UnixDateTime::now().milliseconds_since_epoch();
    if (auto existing = m_credentials.get(cred.id); existing.has_value()) {
        if (existing->created_at_ms > 0)
            cred.created_at_ms = existing->created_at_ms;
        if (cred.last_used_at_ms <= 0)
            cred.last_used_at_ms = existing->last_used_at_ms;
        if (!clear_password && preserve_existing_secrets_if_empty && cred.password.is_empty())
            cred.password = existing->password;
        if (!clear_totp && preserve_existing_secrets_if_empty && cred.totp_secret.is_empty())
            cred.totp_secret = existing->totp_secret;
    }

    if (clear_password)
        cred.password = {};
    if (clear_totp)
        cred.totp_secret = {};

    if (cred.created_at_ms <= 0)
        cred.created_at_ms = now_ms;
    cred.updated_at_ms = now_ms;

    auto id_copy = cred.id;
    m_credentials.set(id_copy, cred);
    if (auto save_res = save_encrypted_vault(); save_res.is_error()) {
        reload();
        return save_res.release_error();
    }

    record_audit("save_credential"_string, id_copy, cred.allowed_origins.first(), String {}, true, "Credential saved in encrypted vault"_string);
    notify_vault_changed();
    return cred;
}

ErrorOr<VaultCredential> VaultStore::set_credential_enabled(StringView id, bool enabled)
{
    auto existing = get_credential(id, true);
    if (!existing.has_value())
        return Error::from_string_literal("Credential not found");

    auto updated = *existing;
    updated.enabled = enabled;
    return save_credential(move(updated), true);
}

ErrorOr<void> VaultStore::delete_credential(StringView id)
{
    if (!m_unlocked)
        return Error::from_string_literal("Vault is locked");
    if (!is_valid_vault_credential_id(id))
        return Error::from_string_literal("Invalid credential 'id'");
    auto existing_cred = m_credentials.get(id);
    if (!existing_cred.has_value())
        return Error::from_string_literal("Credential not found");
    auto backup = *existing_cred;
    m_credentials.remove(id);

    if (auto save_res = save_encrypted_vault(); save_res.is_error()) {
        m_credentials.set(backup.id, move(backup));
        return save_res.release_error();
    }
    record_audit("delete_credential"_string, MUST(String::from_utf8(id)), String {}, String {}, true, "Credential deleted"_string);
    notify_vault_changed();
    return {};
}

ErrorOr<String> VaultStore::resolve_field_value(StringView id, StringView field, URL::URL const& committed_url, StringView tab_container_id)
{
    String origin_str = committed_url.origin().serialize();
    String id_str = MUST(String::from_utf8(id));
    String field_str = MUST(String::from_utf8(field));

    auto audit_fail = [&](StringView detail) -> Error {
        record_audit("resolve_field"_string, id_str, origin_str, field_str, false, MUST(String::from_utf8(detail)));
        return Error::from_string_literal("Vault access denied");
    };

    if (!m_unlocked)
        return audit_fail("Vault is locked"sv);

    auto existing = get_credential(id, true);
    if (!existing.has_value()) {
        record_audit("resolve_field"_string, id_str, origin_str, field_str, false, "Unknown credential ID"_string);
        return Error::from_string_literal("Unknown vault credential ID");
    }

    if (!existing->enabled) {
        record_audit("resolve_field"_string, id_str, origin_str, field_str, false, "Credential is disabled"_string);
        return Error::from_string_literal("Vault credential is disabled");
    }

    if (!vault_credential_matches_origin(*existing, committed_url)) {
        record_audit("resolve_field"_string, id_str, origin_str, field_str, false, "Origin mismatch against pinned allowed_origins"_string);
        return Error::from_string_literal("Tab origin does not match credential's pinned allowed_origins");
    }

    if (!vault_credential_matches_container(*existing, tab_container_id)) {
        record_audit("resolve_field"_string, id_str, origin_str, field_str, false, "Container ID mismatch"_string);
        return Error::from_string_literal("Tab container_id does not match credential's pinned container_id");
    }

    String value;
    if (field == "username"sv) {
        if (existing->username.is_empty()) {
            record_audit("resolve_field"_string, id_str, origin_str, field_str, false, "Username is empty"_string);
            return Error::from_string_literal("Credential has no username");
        }
        value = existing->username;
    } else if (field == "password"sv) {
        if (existing->password.is_empty()) {
            record_audit("resolve_field"_string, id_str, origin_str, field_str, false, "Password is empty"_string);
            return Error::from_string_literal("Credential has no password");
        }
        value = existing->password;
    } else if (field == "totp"sv) {
        if (existing->totp_secret.is_empty()) {
            record_audit("resolve_field"_string, id_str, origin_str, field_str, false, "TOTP secret is empty"_string);
            return Error::from_string_literal("Credential has no TOTP secret configured");
        }
        auto code = generate_totp_code(existing->totp_secret);
        if (code.is_error()) {
            record_audit("resolve_field"_string, id_str, origin_str, field_str, false, "Failed generating TOTP"_string);
            return code.release_error();
        }
        value = code.release_value();
    } else {
        record_audit("resolve_field"_string, id_str, origin_str, field_str, false, "Unsupported field name"_string);
        return Error::from_string_literal("Field must be 'username', 'password', or 'totp'");
    }

    if (auto mut = m_credentials.get(existing->id); mut.has_value()) {
        mut->last_used_at_ms = UnixDateTime::now().milliseconds_since_epoch();
        (void)save_encrypted_vault();
    }

    record_audit("resolve_field"_string, id_str, origin_str, field_str, true, "Blind field injected into page"_string);
    notify_vault_changed();
    return value;
}

void VaultStore::record_audit(String action, String credential_id, String origin, String field, bool ok, String detail)
{
    VaultAuditEntry entry {
        .timestamp_ms = UnixDateTime::now().milliseconds_since_epoch(),
        .action = move(action),
        .credential_id = move(credential_id),
        .origin = move(origin),
        .field = move(field),
        .ok = ok,
        .detail = move(detail),
    };
    m_audit.append(entry);
    if (m_audit.size() > MAX_AUDIT_ENTRIES)
        m_audit.take_first();
    append_audit_to_disk(entry);
}

Vector<VaultAuditEntry> VaultStore::recent_audit(size_t limit) const
{
    Vector<VaultAuditEntry> out;
    size_t count = min(limit, m_audit.size());
    out.ensure_capacity(count);
    for (size_t i = 0; i < count; ++i)
        out.unchecked_append(m_audit[m_audit.size() - 1 - i]);
    return out;
}

void VaultStore::add_observer(VaultStoreObserver& observer)
{
    m_observers.append(observer);
}

void VaultStore::remove_observer(VaultStoreObserver& observer)
{
    m_observers.remove_first_matching([&](auto const& entry) {
        return &entry == &observer;
    });
}

void VaultStore::notify_vault_changed()
{
    for (auto& observer : m_observers)
        observer.vault_changed();
}

}
