/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/NeverDestroyed.h>
#include <AK/Optional.h>
#include <AudioServer/PlatformAudio.h>
#include <LibAudio/ChannelMap.h>
#include <LibAudio/SampleSpecification.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibTest/TestCase.h>

TEST_CASE(platform_audio_devices_can_be_enumerated)
{
    Core::EventLoop event_loop;

    bool completed = false;
    Optional<ErrorOr<Audio::AudioDeviceEnumeration>> enumeration_result;

    Audio::watch_platform_audio_devices([&](ErrorOr<Audio::AudioDeviceEnumeration> devices) {
        enumeration_result = move(devices);
        completed = true;
    });

    if (!completed) {
        auto timeout_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(2000), [&] {
            completed = true;
        });
        timeout_timer->start();
        event_loop.spin_until([&] { return completed; });
    }

    EXPECT(enumeration_result.has_value());
    if (enumeration_result->is_error()) {
        warnln("Skipping WASAPI audio device enumeration check: {}", enumeration_result->error());
        return;
    }

    auto const& enumeration = enumeration_result->value();
    for (auto const& input : enumeration.inputs) {
        EXPECT(!input.dom_device_id.is_empty());
        EXPECT(!input.label.is_empty());
#if defined(AK_OS_WINDOWS)
        EXPECT(input.dom_device_id.starts_with("wasapi:input:"sv));
#endif
    }
    for (auto const& output : enumeration.outputs) {
        EXPECT(!output.dom_device_id.is_empty());
        EXPECT(!output.label.is_empty());
#if defined(AK_OS_WINDOWS)
        EXPECT(output.dom_device_id.starts_with("wasapi:output:"sv));
#endif
    }
}

TEST_CASE(platform_record_stream_can_be_created_and_stopped)
{
    Core::EventLoop event_loop;

    Optional<ErrorOr<Audio::AudioDeviceEnumeration>> enumeration_result;
    Audio::watch_platform_audio_devices([&](ErrorOr<Audio::AudioDeviceEnumeration> devices) {
        enumeration_result = move(devices);
    });
    if (!enumeration_result.has_value() || enumeration_result->is_error() || enumeration_result->value().inputs.is_empty()) {
        warnln("Skipping WASAPI record stream test: no active input device available");
        return;
    }

    auto const& input = enumeration_result->value().inputs.first();
    Atomic<size_t> callback_bytes { 0 };
    Atomic<bool> stream_stopped { false };
    Atomic<bool> callback_after_stop { false };
    RefPtr<Audio::RecordStream> stream;
    Optional<Error> creation_error;
    bool finished = false;

    RefPtr<Audio::RecordStream::CreatePromise> promise = Audio::create_platform_record_stream(
        Audio::SampleSpecification(48000, Audio::ChannelMap::mono()),
        1920,
        input.dom_device_id,
        [&](ReadonlyBytes bytes, Audio::SampleSpecification const&) {
            if (stream_stopped.load()) {
                callback_after_stop.store(true);
                return;
            }
            callback_bytes.fetch_add(bytes.size());
        });

    promise->when_resolved([&](NonnullRefPtr<Audio::RecordStream> const& created_stream) {
        stream = created_stream;
        finished = true;
    });
    promise->when_rejected([&](Error& error) {
        creation_error = move(error);
        finished = true;
    });

    auto timeout_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(2000), [&] {
        finished = true;
    });
    timeout_timer->start();
    event_loop.spin_until([&] { return finished; });
    promise.clear();

    EXPECT(stream || creation_error.has_value());
    if (creation_error.has_value()) {
        warnln("Skipping WASAPI record stream test: {}", *creation_error);
        return;
    }

    EXPECT(stream->sample_specification().is_valid());
    EXPECT_EQ(stream->sample_specification().channel_count(), 1u);

    bool settle_wait_done = false;
    auto settle_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(150), [&] {
        settle_wait_done = true;
    });
    settle_timer->start();
    event_loop.spin_until([&] { return settle_wait_done; });

    stream.clear();
    stream_stopped.store(true);

    bool post_stop_wait_done = false;
    auto post_stop_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(80), [&] {
        post_stop_wait_done = true;
    });
    post_stop_timer->start();
    event_loop.spin_until([&] { return post_stop_wait_done; });

    EXPECT(!callback_after_stop.load());
}
