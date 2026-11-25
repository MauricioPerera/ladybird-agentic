/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/FixedArray.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/YUVData.h>
#include <LibMediaCodecs/CodedFrame.h>
#include <LibMediaCodecs/FFmpeg/FFmpegVideoDecoder.h>
#include <LibMediaCodecs/SpeakerTimeline.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationCamera.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationMp4Recorder.h>
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

TEST_CASE(vp8_encode_and_decode_roundtrip)
{
    constexpr u32 WIDTH = 320;
    constexpr u32 HEIGHT = 240;

    Media::WindowsMediaFoundation::VideoEncoderConfig config {
        .codec_id = Media::CodecID::VP8,
        .width = WIDTH,
        .height = HEIGHT,
        .framerate = 30,
        .bitrate_bps = 800'000,
        .keyframe_interval_frames = 30,
    };

    auto encoder_or_err = Media::WindowsMediaFoundation::WindowsMediaFoundationVideoEncoder::try_create(config);
    if (encoder_or_err.is_error()) {
        warnln("Media Foundation VP8 encoder unavailable ({}), skipping", encoder_or_err.error().description());
        return;
    }
    auto encoder = encoder_or_err.release_value();

    auto decoder_or_err = Media::FFmpeg::FFmpegVideoDecoder::try_create(Media::CodecID::VP8, {});
    if (decoder_or_err.is_error()) {
        warnln("FFmpeg VP8 decoder unavailable ({}), skipping", decoder_or_err.error().description());
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
            decoded_frames++;
        }
    };

    for (auto const& packet : encoded_packets) {
        EXPECT(!packet.data.is_empty());
        if (packet.is_keyframe)
            saw_keyframe = true;

        auto raw_fixed = MUST(FixedArray<u8>::create(packet.data.bytes()));
        Media::CodedFrame coded_frame {
            Media::CodecID::VP8,
            packet.timestamp,
            packet.timestamp,
            packet.duration.is_zero() ? AK::Duration::from_milliseconds(33) : packet.duration,
            packet.is_keyframe ? Media::FrameFlags::Keyframe : Media::FrameFlags::None,
            move(raw_fixed),
        };
        MUST(decoder->receive_coded_data(coded_frame, Media::DecodeIntent::Output));
        drain_decoder();
    }
    decoder->signal_end_of_stream();
    drain_decoder();

    EXPECT(saw_keyframe);
    EXPECT(decoded_frames >= 1u);
}

TEST_CASE(video_capture_concurrent_streams_share_device)
{
    auto devices = Media::WindowsMediaFoundation::enumerate_video_capture_devices();
    if (devices.is_empty())
        return;

    auto const& device_id = devices[0].device_id;
    Atomic<size_t> frames1 { 0 };
    Atomic<size_t> frames2 { 0 };
    Atomic<size_t> ended1 { 0 };
    Atomic<size_t> ended2 { 0 };

    auto stream1_or_err = Media::WindowsMediaFoundation::VideoCaptureStream::create(
        device_id,
        640,
        480,
        30,
        [&](Gfx::DecodedImageFrame) { frames1.fetch_add(1, AK::MemoryOrder::memory_order_relaxed); },
        [&]() { ended1.fetch_add(1, AK::MemoryOrder::memory_order_relaxed); });
    if (stream1_or_err.is_error())
        return;
    auto stream1 = stream1_or_err.release_value();

    auto stream2_or_err = Media::WindowsMediaFoundation::VideoCaptureStream::create(
        device_id,
        640,
        480,
        30,
        [&](Gfx::DecodedImageFrame) { frames2.fetch_add(1, AK::MemoryOrder::memory_order_relaxed); },
        [&]() { ended2.fetch_add(1, AK::MemoryOrder::memory_order_relaxed); });
    EXPECT(!stream2_or_err.is_error());
    auto stream2 = stream2_or_err.release_value();

    EXPECT(stream1->is_running());
    EXPECT(stream2->is_running());

    for (int attempt = 0; attempt < 40 && (frames1.load(AK::MemoryOrder::memory_order_relaxed) == 0 || frames2.load(AK::MemoryOrder::memory_order_relaxed) == 0); ++attempt)
        MUST(Core::System::sleep_ms(50));

    EXPECT(frames1.load(AK::MemoryOrder::memory_order_relaxed) >= 1u);
    EXPECT(frames2.load(AK::MemoryOrder::memory_order_relaxed) >= 1u);
    EXPECT_EQ(ended1.load(AK::MemoryOrder::memory_order_relaxed), 0u);
    EXPECT_EQ(ended2.load(AK::MemoryOrder::memory_order_relaxed), 0u);

    stream1->stop();
    EXPECT(!stream1->is_running());
    EXPECT(stream2->is_running());

    auto frames2_before = frames2.load(AK::MemoryOrder::memory_order_relaxed);
    for (int attempt = 0; attempt < 20 && frames2.load(AK::MemoryOrder::memory_order_relaxed) == frames2_before; ++attempt)
        MUST(Core::System::sleep_ms(50));
    EXPECT(frames2.load(AK::MemoryOrder::memory_order_relaxed) > frames2_before);

    stream2->stop();
    EXPECT(!stream2->is_running());
    EXPECT_EQ(ended1.load(AK::MemoryOrder::memory_order_relaxed), 0u);
    EXPECT_EQ(ended2.load(AK::MemoryOrder::memory_order_relaxed), 0u);
}

TEST_CASE(speaker_timeline_tracks_local_and_multiple_remote_speakers)
{
    Media::SpeakerTimeline timeline;

    // Remote speaker 1 speaks [0.0s .. 1.2s]
    timeline.record_webrtc_activity(101, false, true, 0.25f, AK::Duration::from_milliseconds(0));
    timeline.record_webrtc_activity(101, false, false, 0.0f, AK::Duration::from_milliseconds(1200));

    // Remote speaker 2 speaks [1.3s .. 2.5s]
    timeline.record_webrtc_activity(202, false, true, 0.30f, AK::Duration::from_milliseconds(1300));
    timeline.record_webrtc_activity(202, false, false, 0.0f, AK::Duration::from_milliseconds(2500));

    // Local user speaks [2.7s .. 3.6s]
    timeline.record_webrtc_activity(0, true, true, 0.40f, AK::Duration::from_milliseconds(2700));
    timeline.record_webrtc_activity(0, true, false, 0.0f, AK::Duration::from_milliseconds(3600));

    // Short 80 ms blip (<150 ms) should be discarded.
    timeline.record_webrtc_activity(101, false, true, 0.12f, AK::Duration::from_milliseconds(4000));
    timeline.record_webrtc_activity(101, false, false, 0.0f, AK::Duration::from_milliseconds(4080));

    // Two turns for speaker 1 separated by 200 ms (<=500 ms) should coalesce.
    timeline.record_webrtc_activity(101, false, true, 0.20f, AK::Duration::from_milliseconds(4500));
    timeline.record_webrtc_activity(101, false, false, 0.0f, AK::Duration::from_milliseconds(5000));
    timeline.record_webrtc_activity(101, false, true, 0.22f, AK::Duration::from_milliseconds(5200));
    timeline.record_webrtc_activity(101, false, false, 0.0f, AK::Duration::from_milliseconds(5800));

    timeline.finalize(AK::Duration::from_milliseconds(6000));

    auto const& segments = timeline.segments();
    EXPECT_EQ(segments.size(), 4u);
    EXPECT_EQ(segments[0].speaker, "Asistente 1"_string);
    EXPECT(!segments[0].is_local);
    EXPECT_EQ(segments[1].speaker, "Asistente 2"_string);
    EXPECT(!segments[1].is_local);
    EXPECT_EQ(segments[2].speaker, "Yo"_string);
    EXPECT(segments[2].is_local);
    EXPECT_EQ(segments[3].speaker, "Asistente 1"_string);
    EXPECT_EQ(segments[3].start.to_milliseconds(), 4500);
    EXPECT_EQ(segments[3].end.to_milliseconds(), 5800);

    auto const& speakers = timeline.speakers();
    EXPECT_EQ(speakers.size(), 3u);

    auto json_str = timeline.to_json("meeting.mp4"sv, AK::Duration::from_milliseconds(6000));
    auto parsed = TRY_OR_FAIL(JsonValue::from_string(json_str));
    auto const& obj = parsed.as_object();
    EXPECT_EQ(obj.get_string("recording"sv).value(), "meeting.mp4"_string);
    EXPECT_EQ(obj.get_array("speakers"sv).value().size(), 3u);
    EXPECT_EQ(obj.get_array("segments"sv).value().size(), 4u);
}

TEST_CASE(mp4_recorder_writes_valid_h264_aac_mp4)
{
    StringView const test_path = "ladybird_test_mp4_recorder.mp4"sv;
    if (FileSystem::exists(test_path))
        MUST(FileSystem::remove(test_path, FileSystem::RecursionMode::Disallowed));
    ScopeGuard cleanup = [&] {
        if (FileSystem::exists(test_path))
            (void)FileSystem::remove(test_path, FileSystem::RecursionMode::Disallowed);
    };

    Media::WindowsMediaFoundation::Mp4RecorderConfig config;
    config.width = 320;
    config.height = 240;
    config.fps = 20;
    config.video_bitrate_bps = 1'000'000;
    config.record_audio = true;
    config.audio_sample_rate = 48000;
    config.audio_channels = 2;
    config.audio_bitrate_bps = 128'000;

    {
        auto recorder = TRY_OR_FAIL(Media::WindowsMediaFoundation::WindowsMediaFoundationMp4Recorder::create(test_path, config));

        Vector<u8> bgra_frame;
        bgra_frame.resize(320u * 240u * 4u);
        Vector<i16> pcm_chunk;
        pcm_chunk.resize(960u * 2u);

        for (u32 i = 0; i < 10; ++i) {
            u8 shade = static_cast<u8>(40 + i * 18);
            for (size_t px = 0; px < 320u * 240u; ++px) {
                bgra_frame[px * 4 + 0] = shade;
                bgra_frame[px * 4 + 1] = static_cast<u8>(255 - shade);
                bgra_frame[px * 4 + 2] = 96;
                bgra_frame[px * 4 + 3] = 255;
            }
            auto ts = AK::Duration::from_milliseconds(static_cast<i64>(i) * 50);
            TRY_OR_FAIL(recorder->write_video_frame(bgra_frame.span(), 320, 240, 320 * 4, false, ts, AK::Duration::from_milliseconds(50)));
        }

        if (recorder->has_audio_stream()) {
            for (u32 c = 0; c < 25; ++c) {
                for (size_t f = 0; f < 960; ++f) {
                    i16 sample = static_cast<i16>(((c * 960 + f) % 100 < 50) ? 4000 : -4000);
                    pcm_chunk[f * 2 + 0] = sample;
                    pcm_chunk[f * 2 + 1] = sample;
                }
                auto ts = AK::Duration::from_milliseconds(static_cast<i64>(c) * 20);
                ReadonlyBytes pcm_bytes { reinterpret_cast<u8 const*>(pcm_chunk.data()), pcm_chunk.size() * sizeof(i16) };
                TRY_OR_FAIL(recorder->write_audio_pcm_s16le(pcm_bytes, ts, AK::Duration::from_milliseconds(20)));
            }
        }

        TRY_OR_FAIL(recorder->finalize());
    }

    EXPECT(FileSystem::exists(test_path));
    auto file = TRY_OR_FAIL(Core::File::open(test_path, Core::File::OpenMode::Read));
    auto bytes = TRY_OR_FAIL(file->read_until_eof());
    EXPECT(bytes.size() > 1024u);

    auto sv = StringView { bytes.bytes() };
    EXPECT(sv.contains("ftyp"sv));
    EXPECT(sv.contains("moov"sv));
}
