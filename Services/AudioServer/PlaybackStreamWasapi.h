/*
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibAudio/PlaybackStream.h>
#include <LibThreading/Thread.h>

namespace Audio {

class PlaybackStreamWASAPI final : public PlaybackStream {
public:
    static NonnullRefPtr<CreatePromise> create(OutputState initial_output_state, u32 target_latency_ms, AudioDataRequestCallback&&);

    virtual SampleSpecification sample_specification() const override;

    virtual NonnullRefPtr<Core::ThreadedPromise<void>> resume() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() override;
    virtual NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() override;

    virtual void notify_data_available() override;

    virtual NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double) override;

private:
    struct AudioState;

    PlaybackStreamWASAPI(NonnullRefPtr<AudioState>, NonnullRefPtr<Threading::Thread>);
    ~PlaybackStreamWASAPI();

    NonnullRefPtr<AudioState> m_state;
    RefPtr<Threading::Thread> m_audio_thread;
};

}
