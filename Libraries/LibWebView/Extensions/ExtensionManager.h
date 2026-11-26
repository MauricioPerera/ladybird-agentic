/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibURL/URL.h>
#include <LibWebView/Automation/FlowStore.h>
#include <LibWebView/Extensions/ExtensionStore.h>
#include <LibWebView/Forward.h>

namespace WebView {

class WEBVIEW_API ExtensionManager final
    : public Weakable<ExtensionManager>
    , public ExtensionStoreObserver {
    AK_MAKE_NONCOPYABLE(ExtensionManager);

public:
    AK_ALLOC_WITH_KMALLOC;

    ExtensionManager(Application&, ExtensionStore&, FlowStore&);
    virtual ~ExtensionManager() override;

    void sync_bundled_flows();
    void did_finish_load(ViewImplementation&, URL::URL const&);
    ErrorOr<size_t> inject_extension_into_active_tab(StringView extension_id);

private:
    virtual void extensions_changed() override;
    size_t inject_extension_into_view(InstalledExtension const&, ViewImplementation&, URL::URL const&, bool match_url);

    Application& m_application;
    ExtensionStore& m_store;
    FlowStore& m_flow_store;
    HashMap<String, Vector<String>> m_registered_flows_by_ext;
    bool m_syncing_flows { false };
};

}
