/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibWebView/Automation/FlowTypes.h>

namespace WebView {

class FlowStoreObserver;

class WEBVIEW_API FlowStore final : public Weakable<FlowStore> {
    AK_MAKE_NONCOPYABLE(FlowStore);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit FlowStore(ByteString storage_directory);
    ~FlowStore();

    void reload();

    Vector<FlowDefinition> list_flows() const;
    Optional<FlowDefinition> get_flow(StringView id) const;
    ErrorOr<FlowDefinition> save_flow(FlowDefinition flow);
    ErrorOr<FlowDefinition> save_flow_from_ui(FlowDefinition flow);
    bool requires_mcp_permission(FlowDefinition const&) const;
    ErrorOr<void> delete_flow(StringView id);

    void upsert_run(FlowRunRecord run, bool persist_to_disk = false);
    Vector<FlowRunRecord> list_runs(Optional<StringView> flow_id = {}, size_t limit = 25) const;
    Optional<FlowRunRecord> get_run(StringView run_id) const;

    ByteString const& storage_directory() const { return m_storage_directory; }
    ByteString runs_directory() const;

    void add_observer(FlowStoreObserver&);
    void remove_observer(FlowStoreObserver&);

private:
    ByteString flow_file_path(StringView id) const;
    ByteString run_file_path(StringView run_id) const;
    void remove_run_artifacts_from_disk(StringView run_id) const;
    void notify_flows_changed();
    void notify_flow_runs_changed();

    ByteString m_storage_directory;
    HashMap<String, FlowDefinition> m_flows;
    Vector<FlowRunRecord> m_runs;
    Vector<FlowStoreObserver&> m_observers;
};

class WEBVIEW_API FlowStoreObserver {
public:
    virtual ~FlowStoreObserver() = default;

    virtual void flows_changed() { }
    virtual void flow_runs_changed() { }
};

}
