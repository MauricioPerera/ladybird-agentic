/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Mutex.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundation.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationMp4Recorder.h>

#include <codecapi.h>
#include <mfreadwrite.h>

namespace Media::WindowsMediaFoundation {

struct WindowsMediaFoundationMp4Recorder::State {
    AK_ALLOC_WITH_KMALLOC;

    Mutex mutex;
    ComPtr<IMFSinkWriter> sink_writer;
    DWORD video_stream_index { 0 };
    Optional<DWORD> audio_stream_index;
    Mp4RecorderConfig config {};
    Vector<u8> nv12_buffer;
    bool finalized { false };
    bool has_last_video_timestamp { false };
    AK::Duration last_video_timestamp { AK::Duration::zero() };
    bool has_last_audio_timestamp { false };
    AK::Duration last_audio_timestamp { AK::Duration::zero() };
};

namespace {

u8 clamp_u8(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return static_cast<u8>(value);
}

void convert_packed_32bpp_to_nv12(
    ReadonlyBytes pixels,
    u32 src_width,
    u32 src_height,
    size_t src_pitch,
    bool is_rgb_order,
    u32 dst_width,
    u32 dst_height,
    Vector<u8>& nv12_out)
{
    size_t y_plane_size = static_cast<size_t>(dst_width) * dst_height;
    size_t uv_plane_size = y_plane_size / 2;
    nv12_out.resize(y_plane_size + uv_plane_size);

    u8* y_plane = nv12_out.data();
    u8* uv_plane = y_plane + y_plane_size;
    u8 const* base = pixels.data();

    auto sample_rgb = [&](u32 dx, u32 dy, int& r, int& g, int& b) {
        u32 sx = min(src_width - 1, static_cast<u32>((static_cast<u64>(dx) * src_width) / dst_width));
        u32 sy = min(src_height - 1, static_cast<u32>((static_cast<u64>(dy) * src_height) / dst_height));
        u8 const* px = base + static_cast<size_t>(sy) * src_pitch + static_cast<size_t>(sx) * 4;
        if (is_rgb_order) {
            r = px[0];
            g = px[1];
            b = px[2];
        } else {
            b = px[0];
            g = px[1];
            r = px[2];
        }
    };

    for (u32 y = 0; y < dst_height; y += 2) {
        u8* y_row0 = y_plane + static_cast<size_t>(y) * dst_width;
        u8* y_row1 = y_plane + static_cast<size_t>(y + 1) * dst_width;
        u8* uv_row = uv_plane + static_cast<size_t>(y / 2) * dst_width;

        for (u32 x = 0; x < dst_width; x += 2) {
            int r00 = 0, g00 = 0, b00 = 0;
            int r01 = 0, g01 = 0, b01 = 0;
            int r10 = 0, g10 = 0, b10 = 0;
            int r11 = 0, g11 = 0, b11 = 0;

            sample_rgb(x, y, r00, g00, b00);
            sample_rgb(x + 1, y, r01, g01, b01);
            sample_rgb(x, y + 1, r10, g10, b10);
            sample_rgb(x + 1, y + 1, r11, g11, b11);

            y_row0[x] = clamp_u8(((66 * r00 + 129 * g00 + 25 * b00 + 128) >> 8) + 16);
            y_row0[x + 1] = clamp_u8(((66 * r01 + 129 * g01 + 25 * b01 + 128) >> 8) + 16);
            y_row1[x] = clamp_u8(((66 * r10 + 129 * g10 + 25 * b10 + 128) >> 8) + 16);
            y_row1[x + 1] = clamp_u8(((66 * r11 + 129 * g11 + 25 * b11 + 128) >> 8) + 16);

            int r_avg = (r00 + r01 + r10 + r11 + 2) >> 2;
            int g_avg = (g00 + g01 + g10 + g11 + 2) >> 2;
            int b_avg = (b00 + b01 + b10 + b11 + 2) >> 2;

            uv_row[x] = clamp_u8(((-38 * r_avg - 74 * g_avg + 112 * b_avg + 128) >> 8) + 128);
            uv_row[x + 1] = clamp_u8(((112 * r_avg - 94 * g_avg - 18 * b_avg + 128) >> 8) + 128);
        }
    }
}

ErrorOr<Vector<wchar_t>> utf8_to_wide_null_terminated(StringView value)
{
    if (value.is_empty())
        return Error::from_errno(EINVAL);
    int length = MultiByteToWideChar(CP_UTF8, 0, value.characters_without_null_termination(), static_cast<int>(value.length()), nullptr, 0);
    if (length <= 0)
        return Error::from_errno(EINVAL);
    Vector<wchar_t> wide;
    TRY(wide.try_resize(static_cast<size_t>(length) + 1));
    int written = MultiByteToWideChar(CP_UTF8, 0, value.characters_without_null_termination(), static_cast<int>(value.length()), wide.data(), length);
    if (written <= 0)
        return Error::from_errno(EINVAL);
    wide[length] = L'\0';
    return wide;
}

Mp4RecorderConfig normalize_config(Mp4RecorderConfig const& requested)
{
    Mp4RecorderConfig config = requested;
    u32 w = max<u32>(64, config.width);
    u32 h = max<u32>(64, config.height);
    constexpr u32 MAX_WIDTH = 1920;
    constexpr u32 MAX_HEIGHT = 1080;
    if (w > MAX_WIDTH || h > MAX_HEIGHT) {
        double scale = min(static_cast<double>(MAX_WIDTH) / w, static_cast<double>(MAX_HEIGHT) / h);
        w = max<u32>(64, static_cast<u32>(w * scale));
        h = max<u32>(64, static_cast<u32>(h * scale));
    }
    config.width = w & ~1u;
    config.height = h & ~1u;
    if (config.fps == 0)
        config.fps = 20;
    if (config.video_bitrate_bps == 0)
        config.video_bitrate_bps = 3'000'000;
    if (config.audio_sample_rate == 0)
        config.audio_sample_rate = 48000;
    if (config.audio_channels == 0)
        config.audio_channels = 2;
    if (config.audio_bitrate_bps == 0)
        config.audio_bitrate_bps = 192'000;
    return config;
}

HRESULT add_h264_video_stream(IMFSinkWriter& sink_writer, Mp4RecorderConfig const& config, u32 h264_profile, DWORD& out_stream_index)
{
    ComPtr<IMFMediaType> output_type;
    HRESULT hr = MFCreateMediaType(&output_type);
    if (FAILED(hr))
        return hr;
    if (FAILED(hr = output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
        || FAILED(hr = output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264))
        || FAILED(hr = output_type->SetUINT32(MF_MT_AVG_BITRATE, config.video_bitrate_bps))
        || FAILED(hr = output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))
        || FAILED(hr = MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, config.width, config.height))
        || FAILED(hr = MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, config.fps, 1))
        || FAILED(hr = MFSetAttributeRatio(output_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1))) {
        return hr;
    }
    (void)output_type->SetUINT32(MF_MT_MPEG2_PROFILE, h264_profile);

    DWORD stream_index = 0;
    hr = sink_writer.AddStream(output_type.Get(), &stream_index);
    if (FAILED(hr))
        return hr;

    ComPtr<IMFMediaType> input_type;
    hr = MFCreateMediaType(&input_type);
    if (FAILED(hr))
        return hr;
    if (FAILED(hr = input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
        || FAILED(hr = input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12))
        || FAILED(hr = input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))
        || FAILED(hr = MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, config.width, config.height))
        || FAILED(hr = MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, config.fps, 1))
        || FAILED(hr = MFSetAttributeRatio(input_type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1))) {
        return hr;
    }

    hr = sink_writer.SetInputMediaType(stream_index, input_type.Get(), nullptr);
    if (FAILED(hr))
        return hr;

    out_stream_index = stream_index;
    return S_OK;
}

Optional<DWORD> try_add_aac_audio_stream(IMFSinkWriter& sink_writer, Mp4RecorderConfig const& config)
{
    ComPtr<IMFMediaType> output_type;
    if (FAILED(MFCreateMediaType(&output_type)))
        return {};
    if (FAILED(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio))
        || FAILED(output_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC))
        || FAILED(output_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, config.audio_sample_rate))
        || FAILED(output_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, config.audio_channels))
        || FAILED(output_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16))
        || FAILED(output_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, config.audio_bitrate_bps / 8))
        || FAILED(output_type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0))
        || FAILED(output_type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29))) {
        return {};
    }

    DWORD stream_index = 0;
    if (FAILED(sink_writer.AddStream(output_type.Get(), &stream_index)))
        return {};

    u32 block_align = static_cast<u32>(config.audio_channels) * 2u;
    u32 pcm_avg_bytes = config.audio_sample_rate * block_align;

    ComPtr<IMFMediaType> input_type;
    if (FAILED(MFCreateMediaType(&input_type)))
        return {};
    if (FAILED(input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio))
        || FAILED(input_type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM))
        || FAILED(input_type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, config.audio_sample_rate))
        || FAILED(input_type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, config.audio_channels))
        || FAILED(input_type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16))
        || FAILED(input_type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, block_align))
        || FAILED(input_type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, pcm_avg_bytes))) {
        return {};
    }

    if (FAILED(sink_writer.SetInputMediaType(stream_index, input_type.Get(), nullptr)))
        return {};

    return stream_index;
}

}

WindowsMediaFoundationMp4Recorder::WindowsMediaFoundationMp4Recorder(NonnullOwnPtr<State> state)
    : m_state(move(state))
{
}

WindowsMediaFoundationMp4Recorder::~WindowsMediaFoundationMp4Recorder()
{
    (void)finalize();
}

ErrorOr<NonnullOwnPtr<WindowsMediaFoundationMp4Recorder>> WindowsMediaFoundationMp4Recorder::create(
    StringView output_mp4_path,
    Mp4RecorderConfig const& requested_config)
{
    if (!ensure_media_foundation_started())
        return Error::from_string_literal("Media Foundation could not be started");

    auto config = normalize_config(requested_config);
    auto wide_path = TRY(utf8_to_wide_null_terminated(output_mp4_path));

    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 3))
        || FAILED(attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE))
        || FAILED(attributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE))
        || FAILED(attributes->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4))) {
        return Error::from_string_literal("Failed to configure Media Foundation MP4 sink writer attributes");
    }

    ComPtr<IMFSinkWriter> sink_writer;
    HRESULT hr = MFCreateSinkWriterFromURL(wide_path.data(), nullptr, attributes.Get(), &sink_writer);
    if (FAILED(hr) || !sink_writer)
        return Error::from_windows_error(hr);

    DWORD video_stream_index = 0;
    hr = add_h264_video_stream(*sink_writer.Get(), config, eAVEncH264VProfile_Main, video_stream_index);
    if (FAILED(hr))
        hr = add_h264_video_stream(*sink_writer.Get(), config, eAVEncH264VProfile_Base, video_stream_index);
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    Optional<DWORD> audio_stream_index;
    if (config.record_audio) {
        audio_stream_index = try_add_aac_audio_stream(*sink_writer.Get(), config);
        if (!audio_stream_index.has_value())
            dbgln("WMF MP4 recorder: AAC audio stream setup failed, continuing with video-only MP4");
    }

    hr = sink_writer->BeginWriting();
    if (FAILED(hr))
        return Error::from_windows_error(hr);

    auto state = TRY(adopt_nonnull_own_or_enomem(new (nothrow) State));
    state->sink_writer = move(sink_writer);
    state->video_stream_index = video_stream_index;
    state->audio_stream_index = audio_stream_index;
    state->config = config;

    return TRY(adopt_nonnull_own_or_enomem(new (nothrow) WindowsMediaFoundationMp4Recorder(move(state))));
}

ErrorOr<void> WindowsMediaFoundationMp4Recorder::write_video_frame(
    ReadonlyBytes pixels,
    u32 src_width,
    u32 src_height,
    size_t src_pitch,
    bool is_rgb_order,
    AK::Duration timestamp,
    AK::Duration duration)
{
    if (src_width == 0 || src_height == 0 || src_pitch < static_cast<size_t>(src_width) * 4)
        return Error::from_errno(EINVAL);
    size_t required_bytes = static_cast<size_t>(src_height - 1) * src_pitch + static_cast<size_t>(src_width) * 4;
    if (pixels.size() < required_bytes)
        return Error::from_errno(EINVAL);

    MutexLocker locker(m_state->mutex);
    if (m_state->finalized || !m_state->sink_writer)
        return {};

    if (m_state->has_last_video_timestamp && timestamp <= m_state->last_video_timestamp)
        timestamp = m_state->last_video_timestamp + AK::Duration::from_milliseconds(1);
    m_state->has_last_video_timestamp = true;
    m_state->last_video_timestamp = timestamp;

    if (duration <= AK::Duration::zero())
        duration = AK::Duration::from_milliseconds(1000 / max<u32>(1, m_state->config.fps));

    convert_packed_32bpp_to_nv12(
        pixels,
        src_width,
        src_height,
        src_pitch,
        is_rgb_order,
        m_state->config.width,
        m_state->config.height,
        m_state->nv12_buffer);

    auto sample_or_err = create_input_sample(m_state->nv12_buffer.span(), timestamp, duration);
    if (sample_or_err.is_error())
        return Error::from_string_literal("Failed to create Media Foundation video sample");

    HRESULT hr = m_state->sink_writer->WriteSample(m_state->video_stream_index, sample_or_err.value().Get());
    if (FAILED(hr))
        return Error::from_windows_error(hr);
    return {};
}

ErrorOr<void> WindowsMediaFoundationMp4Recorder::write_audio_pcm_s16le(
    ReadonlyBytes pcm_s16le,
    AK::Duration timestamp,
    AK::Duration duration)
{
    if (pcm_s16le.is_empty())
        return {};

    MutexLocker locker(m_state->mutex);
    if (m_state->finalized || !m_state->sink_writer || !m_state->audio_stream_index.has_value())
        return {};

    if (m_state->has_last_audio_timestamp && timestamp <= m_state->last_audio_timestamp)
        timestamp = m_state->last_audio_timestamp + AK::Duration::from_milliseconds(1);
    m_state->has_last_audio_timestamp = true;
    m_state->last_audio_timestamp = timestamp;

    if (duration <= AK::Duration::zero())
        duration = AK::Duration::from_milliseconds(20);

    auto sample_or_err = create_input_sample(pcm_s16le, timestamp, duration);
    if (sample_or_err.is_error())
        return Error::from_string_literal("Failed to create Media Foundation audio sample");

    HRESULT hr = m_state->sink_writer->WriteSample(*m_state->audio_stream_index, sample_or_err.value().Get());
    if (FAILED(hr))
        return Error::from_windows_error(hr);
    return {};
}

ErrorOr<void> WindowsMediaFoundationMp4Recorder::finalize()
{
    MutexLocker locker(m_state->mutex);
    if (m_state->finalized)
        return {};
    m_state->finalized = true;

    if (!m_state->sink_writer)
        return {};

    HRESULT hr = m_state->sink_writer->Finalize();
    m_state->sink_writer.Reset();
    if (FAILED(hr))
        return Error::from_windows_error(hr);
    return {};
}

u32 WindowsMediaFoundationMp4Recorder::width() const
{
    return m_state->config.width;
}

u32 WindowsMediaFoundationMp4Recorder::height() const
{
    return m_state->config.height;
}

bool WindowsMediaFoundationMp4Recorder::has_audio_stream() const
{
    return m_state->audio_stream_index.has_value();
}

}
