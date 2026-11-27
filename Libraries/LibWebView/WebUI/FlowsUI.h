/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibWebView/Automation/FlowStore.h>
#include <LibWebView/Settings.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class FlowsUI final
    : public WebUI
    , public FlowStoreObserver
    , public SettingsObserver {
    WEB_UI(FlowsUI);

public:
    virtual ~FlowsUI() override;

private:
    virtual void register_interfaces() override;

    virtual void flows_changed() override;
    virtual void flow_runs_changed() override;
    virtual void upload_directory_changed() override;

    void send_state(bool refresh_upload_files = false);
    void refresh_upload_files_cache();
    void save_flow(JsonValue const&);
    void delete_flow(JsonValue const&);
    void create_example_flow();
    void run_flow(JsonValue const&);
    void cancel_run(JsonValue const&);
    void set_upload_directory(JsonValue const&);
    void open_upload_directory();
    void send_action_result(bool ok, Optional<String> message = {}, Optional<String> flow_id = {}, Optional<String> run_id = {});

    WeakPtr<FlowStore> m_store;
    JsonArray m_cached_upload_files;
    bool m_has_cached_upload_files { false };
};

}
