/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibWebView/Extensions/ExtensionStore.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class ExtensionsUI final
    : public WebUI
    , public ExtensionStoreObserver {
    WEB_UI(ExtensionsUI);

public:
    virtual ~ExtensionsUI() override;

private:
    virtual void register_interfaces() override;
    virtual void extensions_changed() override;

    void send_state();
    void install_unpacked(JsonValue const&);
    void create_example_extension();
    void set_enabled(JsonValue const&);
    void set_allow_in_private(JsonValue const&);
    void reload_extension(JsonValue const&);
    void remove_extension(JsonValue const&);
    void get_storage(JsonValue const&);
    void set_storage(JsonValue const&);
    void inject_active_tab(JsonValue const&);
    void send_action_result(bool ok, Optional<String> message = {}, Optional<String> extension_id = {});

    WeakPtr<ExtensionStore> m_store;
};

}
