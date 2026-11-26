/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibWebView/Extensions/ExtensionTypes.h>

namespace WebView {

class ExtensionStoreObserver;

class WEBVIEW_API ExtensionStore final : public Weakable<ExtensionStore> {
    AK_MAKE_NONCOPYABLE(ExtensionStore);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ExtensionStore(ByteString storage_directory);
    ~ExtensionStore();

    void reload();

    Vector<InstalledExtension> list_extensions() const;
    Optional<InstalledExtension> get_extension(StringView id) const;

    ErrorOr<InstalledExtension> install_unpacked(ByteString directory_path);
    ErrorOr<InstalledExtension> reload_extension(StringView id);
    ErrorOr<InstalledExtension> set_enabled(StringView id, bool enabled);
    ErrorOr<InstalledExtension> set_allow_in_private(StringView id, bool allow);
    ErrorOr<void> remove_extension(StringView id);

    void set_last_error(StringView id, Optional<String> error);
    void set_registered_flow_ids(StringView id, Vector<String> flow_ids);

    JsonObject get_storage(StringView id) const;
    ErrorOr<void> set_storage(StringView id, JsonObject data);

    ErrorOr<InstalledExtension> create_example_extension();

    ByteString const& storage_directory() const { return m_storage_directory; }

    void add_observer(ExtensionStoreObserver&);
    void remove_observer(ExtensionStoreObserver&);

private:
    ByteString registry_file_path() const;
    ByteString storage_file_path(StringView id) const;
    ErrorOr<void> save_registry();
    void notify_extensions_changed();

    ByteString m_storage_directory;
    HashMap<String, InstalledExtension> m_extensions;
    Vector<ExtensionStoreObserver&> m_observers;
};

class WEBVIEW_API ExtensionStoreObserver {
public:
    virtual ~ExtensionStoreObserver() = default;

    virtual void extensions_changed() { }
};

}
