/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>
#include <LibMediaCodecs/Export.h>

namespace Media::WindowsMediaFoundation {

struct Mp4RecorderConfig {
    u32 width { 1280 };
    u32 height { 720 };
    u32 fps { 20 };
    u32 video_bitrate_bps { 3'000'000 };
    bool record_audio { true };
    u32 audio_sample_rate { 48000 };
    u16 audio_channels { 2 };
    u32 audio_bitrate_bps { 192'000 };
};

class MEDIACODECS_API WindowsMediaFoundationMp4Recorder final {
    AK_MAKE_NONCOPYABLE(WindowsMediaFoundationMp4Recorder);
    AK_MAKE_NONMOVABLE(WindowsMediaFoundationMp4Recorder);

public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<WindowsMediaFoundationMp4Recorder>> create(StringView output_mp4_path, Mp4RecorderConfig const&);
    ~WindowsMediaFoundationMp4Recorder();

    ErrorOr<void> write_video_frame(ReadonlyBytes pixels, u32 src_width, u32 src_height, size_t src_pitch, bool is_rgb_order, AK::Duration timestamp, AK::Duration duration);
    ErrorOr<void> write_audio_pcm_s16le(ReadonlyBytes pcm_s16le, AK::Duration timestamp, AK::Duration duration);
    ErrorOr<void> finalize();

    u32 width() const;
    u32 height() const;
    bool has_audio_stream() const;

private:
    struct State;

    explicit WindowsMediaFoundationMp4Recorder(NonnullOwnPtr<State>);

    NonnullOwnPtr<State> m_state;
};

}
