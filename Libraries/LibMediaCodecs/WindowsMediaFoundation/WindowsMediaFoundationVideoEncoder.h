/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibMediaCodecs/DecoderError.h>
#include <LibMediaCodecs/Export.h>

namespace Media::WindowsMediaFoundation {

struct VideoEncoderConfig {
    u32 width { 640 };
    u32 height { 480 };
    u32 framerate { 30 };
    u32 bitrate_bps { 1'200'000 };
    u32 keyframe_interval_frames { 60 };
};

struct EncodedVideoPacket {
    ByteBuffer data;
    AK::Duration timestamp;
    AK::Duration duration;
    bool is_keyframe { false };
};

class MEDIACODECS_API WindowsMediaFoundationVideoEncoder final {
    AK_MAKE_NONCOPYABLE(WindowsMediaFoundationVideoEncoder);
    AK_MAKE_NONMOVABLE(WindowsMediaFoundationVideoEncoder);

public:
    AK_ALLOC_WITH_KMALLOC;

    static DecoderErrorOr<NonnullOwnPtr<WindowsMediaFoundationVideoEncoder>> try_create(VideoEncoderConfig const&);
    ~WindowsMediaFoundationVideoEncoder();

    DecoderErrorOr<void> encode_frame(Gfx::DecodedImageFrame const& frame, AK::Duration timestamp, AK::Duration duration, bool force_keyframe, Vector<EncodedVideoPacket>& out_packets);
    DecoderErrorOr<void> encode_bgra_frame(ReadonlyBytes pixels, u32 src_width, u32 src_height, size_t src_pitch, AK::Duration timestamp, AK::Duration duration, bool force_keyframe, Vector<EncodedVideoPacket>& out_packets);
    DecoderErrorOr<void> drain(Vector<EncodedVideoPacket>& out_packets);

    u32 width() const;
    u32 height() const;

private:
    struct State;

    explicit WindowsMediaFoundationVideoEncoder(NonnullOwnPtr<State>);

    DecoderErrorOr<void> encode_pixels_internal(ReadonlyBytes pixels, u32 src_width, u32 src_height, size_t src_pitch, bool is_rgb_order, AK::Duration timestamp, AK::Duration duration, bool force_keyframe, Vector<EncodedVideoPacket>& out_packets);
    DecoderErrorOr<void> collect_outputs(Vector<EncodedVideoPacket>& out_packets);

    NonnullOwnPtr<State> m_state;
};

}
