/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebView/Application.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class AgentUI final
    : public WebUI
    , public MCPAuditObserver {
    WEB_UI(AgentUI);

public:
    virtual ~AgentUI() override;

private:
    virtual void register_interfaces() override;
    virtual void mcp_state_changed() override;

    void send_state();
    void set_paused(JsonValue const&);
    void set_tab_mode(JsonValue const&);
    void clear_egress_taint();
    void clear_audit_log();

    bool m_observing { false };
};

}
