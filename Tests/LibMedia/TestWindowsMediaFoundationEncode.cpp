/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/FixedArray.h>
#include <LibCore/System.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/YUVData.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationCamera.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationVideoDecoder.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationVideoEncoder.h>

#include "TestMediaCommon.h"

namespace {

ByteBuffer annex_b_to_avcc(ReadonlyBytes data)
{
    ByteBuffer avcc;
    size_t idx = 0;
    Optional<size_t> nalu_start;

    auto append_nalu = [&](size_t start, size_t end) {
        if (end <= start)
            return;
        auto length = static_cast<u32>(end - start);
        u8 length_bytes[4] = {
            static_cast<u8>((length >> 24) & 0xFF),
            static_cast<u8>((length >> 16) & 0xFF),
            static_cast<u8>((length >> 8) & 0xFF),
            static_cast<u8>(length & 0xFF),
        };
        avcc.append(length_bytes, sizeof(length_bytes));
        avcc.append(data.slice(start, end - start));
    };

    while (idx < data.size()) {
        size_t start_code_len = 0;
        if (idx + 4 <= data.size() && data[idx] == 0 && data[idx + 1] == 0 && data[idx + 2] == 0 && data[idx + 3] == 1)
            start_code_len = 4;
        else if (idx + 3 <= data.size() && data[idx] == 0 && data[idx + 1] == 0 && data[idx + 2] == 1)
            start_code_len = 3;

        if (start_code_len > 0) {
            if (nalu_start.has_value())
                append_nalu(*nalu_start, idx);
            idx += start_code_len;
            nalu_start = idx;
        } else {
            idx++;
        }
    }
    if (nalu_start.has_value())
        append_nalu(*nalu_start, data.size());

    return avcc;
}

}

TEST_CASE(h264_encode_and_decode_roundtrip)
{
    constexpr u32 WIDTH = 320;
    constexpr u32 HEIGHT = 240;

    Media::WindowsMediaFoundation::VideoEncoderConfig config {
        .width = WIDTH,
        .height = HEIGHT,
        .framerate = 30,
        .bitrate_bps = 800'000,
        .keyframe_interval_frames = 30,
    };

    auto encoder_or_err = Media::WindowsMediaFoundation::WindowsMediaFoundationVideoEncoder::try_create(config);
    if (encoder_or_err.is_error()) {
        EXPECT_EQ(encoder_or_err.error().category(), Media::DecoderErrorCategory::NotImplemented);
        warnln("Media Foundation has no H.264 encoder, skipping");
        return;
    }
    auto encoder = encoder_or_err.release_value();
    EXPECT_EQ(encoder->width(), WIDTH);
    EXPECT_EQ(encoder->height(), HEIGHT);

    auto decoder_or_err = Media::WindowsMediaFoundation::WindowsMediaFoundationVideoDecoder::try_create(Media::CodecID::H264, {});
    if (decoder_or_err.is_error()) {
        EXPECT_EQ(decoder_or_err.error().category(), Media::DecoderErrorCategory::NotImplemented);
        warnln("Media Foundation has no H.264 decoder, skipping");
        return;
    }
    auto decoder = decoder_or_err.release_value();

    Vector<Media::WindowsMediaFoundation::EncodedVideoPacket> encoded_packets;
    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, { static_cast<int>(WIDTH), static_cast<int>(HEIGHT) }));

    for (u32 frame_idx = 0; frame_idx < 10; ++frame_idx) {
        for (int y = 0; y < static_cast<int>(HEIGHT); ++y) {
            auto* scanline = bitmap->scanline_u8(y);
            for (int x = 0; x < static_cast<int>(WIDTH); ++x) {
                scanline[x * 4 + 0] = static_cast<u8>((x + frame_idx * 16) & 0xFF);
                scanline[x * 4 + 1] = static_cast<u8>((y + frame_idx * 8) & 0xFF);
                scanline[x * 4 + 2] = static_cast<u8>(((x ^ y) + frame_idx * 12) & 0xFF);
                scanline[x * 4 + 3] = 0xFF;
            }
        }
        auto frame = MUST(Gfx::DecodedImageFrame::create(*bitmap));
        auto pts = AK::Duration::from_milliseconds(static_cast<i64>(frame_idx) * 33);
        auto dur = AK::Duration::from_milliseconds(33);
        bool force_key = (frame_idx == 0 || frame_idx == 5);
        MUST(encoder->encode_frame(frame, pts, dur, force_key, encoded_packets));
    }
    MUST(encoder->drain(encoded_packets));

    EXPECT(!encoded_packets.is_empty());
    bool saw_keyframe = false;
    size_t decoded_frames = 0;

    auto drain_decoder = [&] {
        while (true) {
            auto out = decoder->take_next_output({});
            if (out.is_error())
                break;
            auto decoded = out.release_value();
            EXPECT_EQ(decoded->size(), (Gfx::Size<u32> { WIDTH, HEIGHT }));
            auto yuv = decoded->yuv_data();
            EXPECT(yuv.has_value());
            decoded_frames++;
        }
    };

    for (auto const& packet : encoded_packets) {
        EXPECT(!packet.data.is_empty());
        if (packet.is_keyframe)
            saw_keyframe = true;

        auto avcc = annex_b_to_avcc(packet.data.bytes());
        EXPECT(!avcc.is_empty());
        auto avcc_fixed = MUST(FixedArray<u8>::create(avcc.bytes()));
        Media::CodedFrame coded_frame {
            Media::CodecID::H264,
            packet.timestamp,
            packet.timestamp,
            packet.duration.is_zero() ? AK::Duration::from_milliseconds(33) : packet.duration,
            packet.is_keyframe ? Media::FrameFlags::Keyframe : Media::FrameFlags::None,
            move(avcc_fixed),
        };
        MUST(decoder->receive_coded_data(coded_frame, Media::DecodeIntent::Output));
        drain_decoder();
    }
    decoder->signal_end_of_stream();
    drain_decoder();

    EXPECT(saw_keyframe);
    EXPECT(decoded_frames >= 1u);
}

TEST_CASE(video_capture_enumeration_does_not_crash)
{
    auto devices = Media::WindowsMediaFoundation::enumerate_video_capture_devices();
    for (size_t i = 0; i < devices.size(); ++i) {
        EXPECT(!devices[i].device_id.is_empty());
        EXPECT_EQ(devices[i].is_default, i == 0);
    }
}

TEST_CASE(video_capture_open_and_stop_does_not_deadlock)
{
    auto devices = Media::WindowsMediaFoundation::enumerate_video_capture_devices();
    if (devices.is_empty())
        return;

    auto const& device_id = devices[0].device_id;

    // Open and immediately stop before any frame arrives.
    {
        auto stream_or_err = Media::WindowsMediaFoundation::VideoCaptureStream::create(
            device_id,
            640,
            480,
            30,
            [](Gfx::DecodedImageFrame) {});
        if (stream_or_err.is_error())
            return;
        auto stream = stream_or_err.release_value();
        EXPECT(stream->is_running());
        stream->stop();
        EXPECT(!stream->is_running());
    }

    // Open, allow frames to arrive asynchronously, then stop cleanly.
    {
        Atomic<size_t> frame_count { 0 };
        auto stream_or_err = Media::WindowsMediaFoundation::VideoCaptureStream::create(
            device_id,
            640,
            480,
            30,
            [&](Gfx::DecodedImageFrame) {
                frame_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
            });
        if (stream_or_err.is_error())
            return;
        auto stream = stream_or_err.release_value();
        for (int attempt = 0; attempt < 40 && frame_count.load(AK::MemoryOrder::memory_order_relaxed) == 0; ++attempt)
            MUST(Core::System::sleep_ms(50));
        stream->stop();
        EXPECT(!stream->is_running());
    }
}
