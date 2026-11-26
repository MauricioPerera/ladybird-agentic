/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibURL/URL.h>
#include <LibWebView/Vault/VaultTypes.h>

namespace WebView {

class WEBVIEW_API VaultStoreObserver {
public:
    virtual ~VaultStoreObserver() = default;

    virtual void vault_changed() { }
};

class WEBVIEW_API VaultStore final : public Weakable<VaultStore> {
    AK_MAKE_NONCOPYABLE(VaultStore);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit VaultStore(ByteString storage_directory, Optional<String> passphrase = {});
    ~VaultStore();

    void reload();

    bool is_unlocked() const { return m_unlocked; }
    String const& key_mode_name() const { return m_key_mode_name; }
    Optional<String> const& last_error() const { return m_last_error; }

    Vector<VaultCredential> list_credentials(bool include_disabled = false) const;
    Vector<VaultCredential> matching_credentials(URL::URL const& url, StringView tab_container_id) const;
    Optional<VaultCredential> get_credential(StringView id, bool include_disabled = true) const;

    ErrorOr<VaultCredential> save_credential(VaultCredential cred, bool preserve_existing_secrets_if_empty = true, bool clear_password = false, bool clear_totp = false);
    ErrorOr<VaultCredential> set_credential_enabled(StringView id, bool enabled);
    ErrorOr<void> delete_credential(StringView id);

    ErrorOr<String> resolve_field_value(StringView id, StringView field, URL::URL const& committed_url, StringView tab_container_id);

    void record_audit(String action, String credential_id, String origin, String field, bool ok, String detail);
    Vector<VaultAuditEntry> recent_audit(size_t limit = 50) const;

    ByteString const& storage_directory() const { return m_storage_directory; }

    void add_observer(VaultStoreObserver&);
    void remove_observer(VaultStoreObserver&);

private:
    ByteString vault_file_path() const;
    ByteString master_key_dpapi_path() const;
    ByteString master_key_file_path() const;
    ByteString audit_log_path() const;

    ErrorOr<void> ensure_master_key_for_save();
    ErrorOr<void> load_encrypted_vault();
    ErrorOr<void> save_encrypted_vault();
    void load_audit_log();
    void append_audit_to_disk(VaultAuditEntry const&) const;
    void notify_vault_changed();

    ByteString m_storage_directory;
    Optional<String> m_passphrase;
    ByteBuffer m_master_key;
    ByteBuffer m_kdf_salt;
    String m_key_mode_name;
    Optional<String> m_last_error;
    bool m_unlocked { false };
    HashMap<String, VaultCredential> m_credentials;
    Vector<VaultAuditEntry> m_audit;
    Vector<VaultStoreObserver&> m_observers;
};

}
