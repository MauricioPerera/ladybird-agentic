/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <LibWebView/Skills/SkillStore.h>
#include <LibWebView/WebUI.h>

namespace WebView {

class SkillsUI final
    : public WebUI
    , public SkillStoreObserver {
    WEB_UI(SkillsUI);

public:
    virtual ~SkillsUI() override;

private:
    virtual void register_interfaces() override;

    virtual void skills_changed() override;

    void send_state();
    void save_skill(JsonValue const&);
    void set_skill_enabled(JsonValue const&);
    void delete_skill(JsonValue const&);
    void create_example_skill(JsonValue const&);
    void send_action_result(bool ok, Optional<String> message = {}, Optional<String> skill_name = {});

    WeakPtr<SkillStore> m_store;
};

}
