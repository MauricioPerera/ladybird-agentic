/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/MCPBanner.h>
#include <UI/Qt/StringUtils.h>

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
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

    m_tab_access_button = new QPushButton(this);
    connect(m_tab_access_button, &QPushButton::clicked, this, &MCPBanner::toggle_current_tab_access_requested);
    layout->addWidget(m_tab_access_button);

    m_copy_config_button = new QPushButton("Copy Config", this);
    connect(m_copy_config_button, &QPushButton::clicked, this, &MCPBanner::copy_config_requested);
    layout->addWidget(m_copy_config_button);

    auto* disable_button = new QPushButton("Disable MCP", this);
    connect(disable_button, &QPushButton::clicked, this, &MCPBanner::disable_requested);
    layout->addWidget(disable_button);
}

void MCPBanner::update_state(u16 port, bool tab_loaded, bool tab_allowed)
{
    auto status_text = !tab_loaded ? "Unavailable"sv : (tab_allowed ? "Allowed"sv : "Blocked"sv);
    m_label->setText(qformatted("MCP active on 127.0.0.1:{} | This tab: {}", port, status_text));
    m_tab_access_button->setEnabled(tab_loaded);
    m_tab_access_button->setText(tab_loaded && tab_allowed ? "Revoke This Tab" : "Allow This Tab");
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
