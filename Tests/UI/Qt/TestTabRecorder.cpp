/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/TabRecorder.h>

#include <QAction>
#include <QActionGroup>
#include <QMenu>

TEST_CASE(enumerate_microphones_and_select_in_menu)
{
    auto original_settings = Ladybird::TabRecorder::settings();
    ScopeGuard restore_settings = [&] {
        Ladybird::TabRecorder::settings() = original_settings;
    };

    Vector<Ladybird::MicrophoneDeviceInfo> mics;
    for (int i = 0; i < 3; ++i)
        mics = Ladybird::TabRecorder::enumerate_microphones();

    QMenu menu;
    auto* mic_menu = menu.addMenu("Microphone");
    auto* mic_group = new QActionGroup(mic_menu);
    mic_group->setExclusive(true);

    auto* default_action = mic_menu->addAction("System Default");
    default_action->setCheckable(true);
    mic_group->addAction(default_action);
    QObject::connect(default_action, &QAction::triggered, &menu, [] {
        Ladybird::TabRecorder::settings().microphone_device_id = {};
    });

    QAction* first_mic_action = nullptr;
    String first_mic_id;
    for (auto const& mic : mics) {
        EXPECT(!mic.device_id.is_empty());
        EXPECT(!mic.label.is_empty());
        auto* action = mic_menu->addAction(qstring_from_ak_string(mic.label));
        action->setCheckable(true);
        mic_group->addAction(action);
        QObject::connect(action, &QAction::triggered, &menu, [device_id = mic.device_id] {
            Ladybird::TabRecorder::settings().microphone_device_id = device_id;
        });
        if (!first_mic_action) {
            first_mic_action = action;
            first_mic_id = mic.device_id;
        }
    }

    if (first_mic_action) {
        first_mic_action->trigger();
        EXPECT_EQ(Ladybird::TabRecorder::settings().microphone_device_id, first_mic_id);
    }

    default_action->trigger();
    EXPECT(Ladybird::TabRecorder::settings().microphone_device_id.is_empty());
}
