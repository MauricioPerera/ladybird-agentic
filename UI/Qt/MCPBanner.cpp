/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/MCPBanner.h>
#include <UI/Qt/StringUtils.h>

#include <QComboBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>

namespace Ladybird {

MCPBanner::MCPBanner(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("LadybirdMCPBanner");
    setAttribute(Qt::WA_StyledBackground);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    update_chrome_style();

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(12, 3, 12, 3);
    layout->setSpacing(6);

    m_label = new QLabel(this);
    layout->addWidget(m_label);
    layout->addStretch();

    m_tab_mode_combo = new QComboBox(this);
    m_tab_mode_combo->addItem("Tab: Blocked", to_underlying(WebView::MCPAccessMode::Blocked));
    m_tab_mode_combo->addItem("Tab: Read-Only", to_underlying(WebView::MCPAccessMode::ReadOnly));
    m_tab_mode_combo->addItem("Tab: Same-Origin", to_underlying(WebView::MCPAccessMode::SameOrigin));
    m_tab_mode_combo->addItem("Tab: Full", to_underlying(WebView::MCPAccessMode::Full));
    connect(m_tab_mode_combo, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (index < 0)
            return;
        auto mode = static_cast<WebView::MCPAccessMode>(m_tab_mode_combo->itemData(index).toUInt());
        emit tab_access_mode_selected(mode);
    });
    layout->addWidget(m_tab_mode_combo);

    m_pause_button = new QPushButton("Pause Agent", this);
    connect(m_pause_button, &QPushButton::clicked, this, &MCPBanner::toggle_pause_requested);
    layout->addWidget(m_pause_button);

    auto* agent_button = new QPushButton("Shield", this);
    connect(agent_button, &QPushButton::clicked, this, &MCPBanner::open_agent_requested);
    layout->addWidget(agent_button);

    auto* flows_button = new QPushButton("Flows", this);
    connect(flows_button, &QPushButton::clicked, this, &MCPBanner::open_flows_requested);
    layout->addWidget(flows_button);

    auto* skills_button = new QPushButton("Skills", this);
    connect(skills_button, &QPushButton::clicked, this, &MCPBanner::open_skills_requested);
    layout->addWidget(skills_button);

    auto* vault_button = new QPushButton("Vault", this);
    connect(vault_button, &QPushButton::clicked, this, &MCPBanner::open_vault_requested);
    layout->addWidget(vault_button);

    m_copy_config_button = new QPushButton("Copy Config", this);
    connect(m_copy_config_button, &QPushButton::clicked, this, &MCPBanner::copy_config_requested);
    layout->addWidget(m_copy_config_button);

    auto* disable_button = new QPushButton("Disable MCP", this);
    connect(disable_button, &QPushButton::clicked, this, &MCPBanner::disable_requested);
    layout->addWidget(disable_button);
}

void MCPBanner::update_state(u16 port, bool tab_loaded, WebView::MCPAccessMode mode, bool paused)
{
    StringView mode_text = "Blocked"sv;
    switch (mode) {
    case WebView::MCPAccessMode::Blocked:
        mode_text = "Blocked"sv;
        break;
    case WebView::MCPAccessMode::ReadOnly:
        mode_text = "Read-Only"sv;
        break;
    case WebView::MCPAccessMode::SameOrigin:
        mode_text = "Same-Origin"sv;
        break;
    case WebView::MCPAccessMode::Full:
        mode_text = "Full"sv;
        break;
    }
    auto status_text = !tab_loaded ? "Unavailable"sv : mode_text;
    auto pause_label = paused ? " [PAUSED]"sv : ""sv;
    m_label->setText(qformatted("MCP active on 127.0.0.1:{}{} | This tab: {}", port, pause_label, status_text));

    {
        QSignalBlocker blocker(m_tab_mode_combo);
        m_tab_mode_combo->setEnabled(tab_loaded);
        int idx = m_tab_mode_combo->findData(to_underlying(tab_loaded ? mode : WebView::MCPAccessMode::Blocked));
        if (idx >= 0)
            m_tab_mode_combo->setCurrentIndex(idx);
    }

    m_pause_button->setText(paused ? "Resume Agent" : "Pause Agent");
}

void MCPBanner::show_copied_feedback()
{
    auto generation = ++m_copy_feedback_generation;
    m_copy_config_button->setText("Copied!");
    QTimer::singleShot(1500, this, [this, generation] {
        if (m_copy_feedback_generation == generation)
            m_copy_config_button->setText("Copy Config");
    });
}

bool MCPBanner::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();

    return QWidget::event(event);
}

void MCPBanner::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::mcp_banner_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

}
