/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibAudio/Forward.h>
#include <LibCore/Forward.h>
#include <LibGC/Ptr.h>
#include <LibGC/Weak.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibMedia/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <thread>

namespace Web::HTML {

// Plays the audio tracks of a MediaStream assigned to a media element's srcObject, mixed into one output stream.
// https://w3c.github.io/mediacapture-main/#mediastreams-in-media-elements
class MediaStreamPlayback final : public Weakable<MediaStreamPlayback> {
    AK_MAKE_NONCOPYABLE(MediaStreamPlayback);
    AK_MAKE_NONMOVABLE(MediaStreamPlayback);
    AK_ALLOC_WITH_KMALLOC;

public:
    static NonnullOwnPtr<MediaStreamPlayback> create(NonnullRefPtr<Media::AudioOutputFactory>);
    ~MediaStreamPlayback();

    void set_tracks(ReadonlySpan<GC::Ref<MediaCapture::MediaStreamTrack>>);
    void set_video_frame_callback(Function<void(Optional<Gfx::DecodedImageFrame>)>);

    void play();
    void pause();
    void set_volume(double);

    // A MediaStream has no timeline of its own, so the position is the time spent playing.
    double current_time() const;

    struct Mixer;
    struct TrackInput;
    struct VideoDeliveryState;

private:
    explicit MediaStreamPlayback(NonnullRefPtr<Media::AudioOutputFactory>);

    void remove_input(size_t index);
    void clear_video_subscription();
    void deliver_video_frame_on_owner_thread(Gfx::DecodedImageFrame);

    NonnullRefPtr<Mixer> m_mixer;
    NonnullRefPtr<Media::AudioOutputFactory> m_audio_output_factory;
    std::thread::id m_owner_thread_id;
    RefPtr<Core::WeakEventLoopReference> m_owner_loop_weak;

    struct Subscription {
        GC::Weak<MediaCapture::MediaStreamTrack> track;
        NonnullRefPtr<MediaCapture::AudioFrameSink> sink;
        NonnullRefPtr<TrackInput> input;
    };
    Vector<Subscription> m_subscriptions;

    struct VideoSubscription {
        GC::Weak<MediaCapture::MediaStreamTrack> track;
        NonnullRefPtr<MediaCapture::VideoFrameSink> sink;
    };
    Optional<VideoSubscription> m_video_subscription;
    RefPtr<VideoDeliveryState> m_video_delivery_state;
    Function<void(Optional<Gfx::DecodedImageFrame>)> m_on_video_frame;
    Optional<Gfx::DecodedImageFrame> m_latest_video_frame;
    bool m_has_delivered_first_video_frame { false };

    RefPtr<Audio::PlaybackStream> m_output;
    bool m_creating_output { false };
    bool m_playing { false };
    double m_volume { 1.0 };

    AK::Duration m_time_played;
    Optional<MonotonicTime> m_playing_since;
};

}
