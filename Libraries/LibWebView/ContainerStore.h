/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibWebView/Export.h>

namespace WebView {

struct TabContainer {
    String id;
    String name;
    String color;

    bool operator==(TabContainer const&) const = default;
};

constexpr StringView EPHEMERAL_AGENT_CONTAINER_PREFIX = "agent-ephemeral-"sv;

WEBVIEW_API bool is_valid_container_id(StringView id);
WEBVIEW_API bool is_valid_container_color(StringView color);
WEBVIEW_API bool is_ephemeral_container_id(StringView id);
WEBVIEW_API bool is_agent_isolated_container_id(StringView id);
WEBVIEW_API Vector<TabContainer> default_tab_containers();

class ContainerStoreObserver;

class WEBVIEW_API ContainerStore final : public Weakable<ContainerStore> {
    AK_MAKE_NONCOPYABLE(ContainerStore);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit ContainerStore(ByteString persistence_path);
    ~ContainerStore();

    void reload();

    Vector<TabContainer> list_containers() const { return m_containers; }
    Optional<TabContainer> get_container(StringView id) const;
    ErrorOr<TabContainer> create_container(String name, String color, Optional<String> requested_id = {});
    ErrorOr<void> delete_container(StringView id);

    void add_observer(ContainerStoreObserver&);
    void remove_observer(ContainerStoreObserver&);

private:
    ErrorOr<void> save() const;
    void notify_containers_changed();

    ByteString m_persistence_path;
    Vector<TabContainer> m_containers;
    Vector<ContainerStoreObserver&> m_observers;
};

class WEBVIEW_API ContainerStoreObserver {
public:
    virtual ~ContainerStoreObserver() = default;

    virtual void containers_changed() { }
};

}
