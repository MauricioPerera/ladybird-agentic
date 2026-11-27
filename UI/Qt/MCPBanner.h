/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/kmalloc.h>
#include <LibWebView/ViewImplementation.h>

#include <QWidget>

class QComboBox;
class QLabel;
class QPushButton;

namespace Ladybird {

class MCPBanner final : public QWidget {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit MCPBanner(QWidget* parent = nullptr);

    void update_state(u16 port, bool tab_loaded, WebView::MCPAccessMode mode, bool paused);
    void show_copied_feedback();

signals:
    void tab_access_mode_selected(WebView::MCPAccessMode);
    void toggle_pause_requested();
    void open_agent_requested();
    void open_flows_requested();
    void open_skills_requested();
    void open_vault_requested();
    void copy_config_requested();
    void disable_requested();

private:
    virtual bool event(QEvent*) override;

    void update_chrome_style();

    QLabel* m_label { nullptr };
    QComboBox* m_tab_mode_combo { nullptr };
    QPushButton* m_pause_button { nullptr };
    QPushButton* m_copy_config_button { nullptr };
    bool m_is_updating_chrome_style { false };
    u64 m_copy_feedback_generation { 0 };
};

}
