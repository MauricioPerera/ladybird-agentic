/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibWebView/ContainerStore.h>
#include <LibWebView/Vault/VaultStore.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class VaultUI final
    : public WebUI
    , public VaultStoreObserver
    , public ContainerStoreObserver {
    WEB_UI(VaultUI);

public:
    virtual ~VaultUI() override;

private:
    virtual void register_interfaces() override;

    virtual void vault_changed() override;
    virtual void containers_changed() override;

    void send_state();
    void save_credential(JsonValue const&);
    void set_credential_enabled(JsonValue const&);
    void delete_credential(JsonValue const&);
    void test_totp(JsonValue const&);
    void send_action_result(bool ok, Optional<String> message = {}, Optional<String> credential_id = {});

    WeakPtr<VaultStore> m_store;
    WeakPtr<ContainerStore> m_containers;
};

}
