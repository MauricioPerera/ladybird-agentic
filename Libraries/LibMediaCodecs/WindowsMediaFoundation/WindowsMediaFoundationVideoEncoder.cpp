/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/Bitmap.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundation.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationVideoEncoder.h>

#include <initguid.h>
#include <icodecapi.h>
#include <codecapi.h>
#include <oleauto.h>

namespace Media::WindowsMediaFoundation {

// CLSID_CMSH264EncoderMFT ({6ca50344-051a-4ded-9779-a43305165e35})
static constexpr GUID CLSID_CMSH264EncoderMFT_VALUE = {
    0x6ca50344,
    0x051a,
    0x4ded,
    { 0x97, 0x79, 0xa4, 0x33, 0x05, 0x16, 0x5e, 0x35 }
};

struct WindowsMediaFoundationVideoEncoder::State {
    AK_ALLOC_WITH_KMALLOC;

    ComPtr<IMFTransform> transform;
    ComPtr<ICodecAPI> codec_api;
    OutputStreamInfo output_info {};
    VideoEncoderConfig config {};
    Vector<u8> nv12_buffer;
    ByteBuffer cached_parameter_sets;
    u64 frame_index { 0 };
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

void set_codec_api_ui4(ICodecAPI* api, GUID const& guid, u32 value)
{
    if (!api)
        return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = value;
    (void)api->SetValue(&guid, &var);
}

void set_codec_api_bool(ICodecAPI* api, GUID const& guid, bool value)
{
    if (!api)
        return;
    VARIANT var;
    VariantInit(&var);
    var.vt = VT_BOOL;
    var.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    (void)api->SetValue(&guid, &var);
}

struct AnnexBInspection {
    bool has_sps { false };
    bool has_pps { false };
    bool has_idr { false };
    ByteBuffer parameter_sets;
};

AnnexBInspection inspect_annex_b_nalus(ReadonlyBytes data)
{
    AnnexBInspection result;
    static constexpr Array<u8, 4> START_CODE { 0, 0, 0, 1 };

    auto record_nalu = [&](size_t start, size_t end) {
        if (end <= start)
            return;
        auto nalu = data.slice(start, end - start);
        u8 nalu_type = nalu[0] & 0x1F;
        if (nalu_type == 7) {
            result.has_sps = true;
            (void)result.parameter_sets.try_append(START_CODE.span());
            (void)result.parameter_sets.try_append(nalu);
        } else if (nalu_type == 8) {
            result.has_pps = true;
            (void)result.parameter_sets.try_append(START_CODE.span());
            (void)result.parameter_sets.try_append(nalu);
        } else if (nalu_type == 5) {
            result.has_idr = true;
        }
    };

    size_t idx = 0;
    Optional<size_t> nalu_start;
    while (idx < data.size()) {
        size_t start_code_len = 0;
        if (idx + 4 <= data.size() && data[idx] == 0 && data[idx + 1] == 0 && data[idx + 2] == 0 && data[idx + 3] == 1)
            start_code_len = 4;
        else if (idx + 3 <= data.size() && data[idx] == 0 && data[idx + 1] == 0 && data[idx + 2] == 1)
            start_code_len = 3;

        if (start_code_len > 0) {
            if (nalu_start.has_value())
                record_nalu(*nalu_start, idx);
            idx += start_code_len;
            nalu_start = idx;
        } else {
            idx++;
        }
    }
    if (nalu_start.has_value())
        record_nalu(*nalu_start, data.size());

    return result;
}

DecoderErrorOr<ComPtr<IMFTransform>> create_h264_encoder_transform()
{
    if (!ensure_media_foundation_started())
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Media Foundation could not be started"sv);

    ComPtr<IMFTransform> transform;
    if (SUCCEEDED(CoCreateInstance(CLSID_CMSH264EncoderMFT_VALUE, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&transform))) && transform)
        return transform;

    MFT_REGISTER_TYPE_INFO output_type_info { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** activates = nullptr;
    UINT32 activate_count = 0;
    auto flags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;
    if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, nullptr, &output_type_info, &activates, &activate_count)))
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Media Foundation could not enumerate H.264 video encoders"sv);

    ScopeGuard release_activates = [&] {
        for (UINT32 i = 0; i < activate_count; ++i)
            activates[i]->Release();
        CoTaskMemFree(static_cast<void*>(activates));
    };

    for (UINT32 i = 0; i < activate_count; ++i) {
        if (SUCCEEDED(activates[i]->ActivateObject(IID_PPV_ARGS(&transform))) && transform)
            return transform;
    }

    return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "No Media Foundation H.264 encoder transform available"sv);
}

}

WindowsMediaFoundationVideoEncoder::WindowsMediaFoundationVideoEncoder(NonnullOwnPtr<State> state)
    : m_state(move(state))
{
}

WindowsMediaFoundationVideoEncoder::~WindowsMediaFoundationVideoEncoder()
{
    if (m_state->transform) {
        (void)m_state->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        (void)m_state->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
}

u32 WindowsMediaFoundationVideoEncoder::width() const
{
    return m_state->config.width;
}

u32 WindowsMediaFoundationVideoEncoder::height() const
{
    return m_state->config.height;
}

DecoderErrorOr<NonnullOwnPtr<WindowsMediaFoundationVideoEncoder>> WindowsMediaFoundationVideoEncoder::try_create(VideoEncoderConfig const& requested_config)
{
    VideoEncoderConfig config = requested_config;
    config.width &= ~1u;
    config.height &= ~1u;
    if (config.width < 16 || config.height < 16)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "H.264 encoder requires even dimensions of at least 16x16"sv);
    if (config.framerate == 0)
        config.framerate = 30;
    if (config.bitrate_bps == 0)
        config.bitrate_bps = 1'200'000;
    if (config.keyframe_interval_frames == 0)
        config.keyframe_interval_frames = 60;

    auto transform = TRY(create_h264_encoder_transform());

    ComPtr<ICodecAPI> codec_api;
    (void)transform.As(&codec_api);
    set_codec_api_ui4(codec_api.Get(), CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_CBR);
    set_codec_api_ui4(codec_api.Get(), CODECAPI_AVEncCommonMeanBitRate, config.bitrate_bps);
    set_codec_api_ui4(codec_api.Get(), CODECAPI_AVEncMPVGOPSize, config.keyframe_interval_frames);
    set_codec_api_bool(codec_api.Get(), CODECAPI_AVLowLatencyMode, true);
    set_codec_api_ui4(codec_api.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0);

    // CMSH264EncoderMFT requires SetOutputType BEFORE SetInputType.
    ComPtr<IMFMediaType> output_type;
    if (FAILED(MFCreateMediaType(&output_type)))
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate H.264 encoder output media type"sv);
    if (FAILED(output_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
        || FAILED(output_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264))
        || FAILED(output_type->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate_bps))
        || FAILED(MFSetAttributeRatio(output_type.Get(), MF_MT_FRAME_RATE, config.framerate, 1))
        || FAILED(MFSetAttributeSize(output_type.Get(), MF_MT_FRAME_SIZE, config.width, config.height))
        || FAILED(output_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))) {
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Failed to populate H.264 encoder output media type"sv);
    }
    (void)output_type->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);

    auto output_hr = transform->SetOutputType(0, output_type.Get(), 0);
    if (FAILED(output_hr))
        return DecoderError::format(DecoderErrorCategory::Invalid, "H.264 encoder rejected output media type: {:#x}", static_cast<u32>(output_hr));

    ComPtr<IMFMediaType> input_type;
    if (FAILED(MFCreateMediaType(&input_type)))
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate H.264 encoder input media type"sv);
    if (FAILED(input_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video))
        || FAILED(input_type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12))
        || FAILED(MFSetAttributeSize(input_type.Get(), MF_MT_FRAME_SIZE, config.width, config.height))
        || FAILED(MFSetAttributeRatio(input_type.Get(), MF_MT_FRAME_RATE, config.framerate, 1))
        || FAILED(input_type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))) {
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Failed to populate H.264 encoder input media type"sv);
    }

    auto input_hr = transform->SetInputType(0, input_type.Get(), 0);
    if (FAILED(input_hr))
        return DecoderError::format(DecoderErrorCategory::Invalid, "H.264 encoder rejected NV12 input media type: {:#x}", static_cast<u32>(input_hr));

    if (FAILED(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0))
        || FAILED(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0))) {
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "H.264 encoder failed to start streaming"sv);
    }

    auto state = DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) State));
    state->transform = move(transform);
    state->codec_api = move(codec_api);
    state->config = config;

    if (auto stream_info = output_stream_info(*state->transform.Get()); !stream_info.is_error()) {
        state->output_info = stream_info.release_value();
        if (!state->output_info.transform_provides_samples && state->output_info.sample_byte_count == 0)
            state->output_info.sample_byte_count = max<u32>(256u * 1024u, config.width * config.height);
    } else {
        state->output_info.sample_byte_count = max<u32>(256u * 1024u, config.width * config.height);
    }

    // Seed cached parameter sets if the encoder already exposes MF_MT_MPEG_SEQUENCE_HEADER.
    ComPtr<IMFMediaType> negotiated_output_type;
    if (SUCCEEDED(state->transform->GetOutputCurrentType(0, &negotiated_output_type)) && negotiated_output_type) {
        UINT32 seq_header_len = 0;
        if (SUCCEEDED(negotiated_output_type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &seq_header_len)) && seq_header_len > 0) {
            if (auto header_buf = ByteBuffer::create_uninitialized(seq_header_len); !header_buf.is_error()) {
                if (SUCCEEDED(negotiated_output_type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, header_buf.value().data(), seq_header_len, &seq_header_len))) {
                    auto inspection = inspect_annex_b_nalus(header_buf.value().bytes());
                    if (!inspection.parameter_sets.is_empty())
                        state->cached_parameter_sets = move(inspection.parameter_sets);
                }
            }
        }
    }

    return DECODER_TRY_ALLOC(adopt_nonnull_own_or_enomem(new (nothrow) WindowsMediaFoundationVideoEncoder(move(state))));
}

DecoderErrorOr<void> WindowsMediaFoundationVideoEncoder::encode_frame(
    Gfx::DecodedImageFrame const& frame,
    AK::Duration timestamp,
    AK::Duration duration,
    bool force_keyframe,
    Vector<EncodedVideoPacket>& out_packets)
{
    if (frame.width() <= 0 || frame.height() <= 0)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Cannot encode empty video frame"sv);
    bool is_rgb_order = frame.format() == Gfx::BitmapFormat::RGBx8888 || frame.format() == Gfx::BitmapFormat::RGBA8888;
    return encode_pixels_internal(
        frame.pixels(),
        static_cast<u32>(frame.width()),
        static_cast<u32>(frame.height()),
        frame.pitch(),
        is_rgb_order,
        timestamp,
        duration,
        force_keyframe,
        out_packets);
}

DecoderErrorOr<void> WindowsMediaFoundationVideoEncoder::encode_bgra_frame(
    ReadonlyBytes pixels,
    u32 src_width,
    u32 src_height,
    size_t src_pitch,
    AK::Duration timestamp,
    AK::Duration duration,
    bool force_keyframe,
    Vector<EncodedVideoPacket>& out_packets)
{
    return encode_pixels_internal(pixels, src_width, src_height, src_pitch, false, timestamp, duration, force_keyframe, out_packets);
}

DecoderErrorOr<void> WindowsMediaFoundationVideoEncoder::encode_pixels_internal(
    ReadonlyBytes pixels,
    u32 src_width,
    u32 src_height,
    size_t src_pitch,
    bool is_rgb_order,
    AK::Duration timestamp,
    AK::Duration duration,
    bool force_keyframe,
    Vector<EncodedVideoPacket>& out_packets)
{
    if (src_width == 0 || src_height == 0 || src_pitch < static_cast<size_t>(src_width) * 4)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Invalid source pixel geometry for H.264 encoder"sv);
    size_t required_bytes = static_cast<size_t>(src_height - 1) * src_pitch + static_cast<size_t>(src_width) * 4;
    if (pixels.size() < required_bytes)
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "Truncated source pixel buffer for H.264 encoder"sv);

    convert_packed_32bpp_to_nv12(
        pixels,
        src_width,
        src_height,
        src_pitch,
        is_rgb_order,
        m_state->config.width,
        m_state->config.height,
        m_state->nv12_buffer);

    if (force_keyframe || m_state->frame_index == 0)
        set_codec_api_ui4(m_state->codec_api.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);

    auto input_sample = TRY(create_input_sample(m_state->nv12_buffer.span(), timestamp, duration));
    auto result = m_state->transform->ProcessInput(0, input_sample.Get(), 0);
    if (result == MF_E_NOTACCEPTING) {
        TRY(collect_outputs(out_packets));
        result = m_state->transform->ProcessInput(0, input_sample.Get(), 0);
    }
    if (FAILED(result))
        return DecoderError::format(DecoderErrorCategory::Corrupted, "H.264 encoder ProcessInput failed: {:#x}", static_cast<u32>(result));

    m_state->frame_index++;
    return collect_outputs(out_packets);
}

DecoderErrorOr<void> WindowsMediaFoundationVideoEncoder::drain(Vector<EncodedVideoPacket>& out_packets)
{
    if (FAILED(m_state->transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0))
        || FAILED(m_state->transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0))) {
        return DecoderError::with_description(DecoderErrorCategory::Unknown, "H.264 encoder drain message failed"sv);
    }
    return collect_outputs(out_packets);
}

DecoderErrorOr<void> WindowsMediaFoundationVideoEncoder::collect_outputs(Vector<EncodedVideoPacket>& out_packets)
{
    while (true) {
        auto processed = TRY(process_output(*m_state->transform.Get(), m_state->output_info));
        if (processed.has<NeedsMoreInput>())
            return {};
        if (processed.has<OutputTypeNeeded>()) {
            auto refreshed_info = output_stream_info(*m_state->transform.Get());
            if (!refreshed_info.is_error()) {
                m_state->output_info = refreshed_info.release_value();
                if (!m_state->output_info.transform_provides_samples && m_state->output_info.sample_byte_count == 0)
                    m_state->output_info.sample_byte_count = max<u32>(256u * 1024u, m_state->config.width * m_state->config.height);
            }
            continue;
        }

        auto sample = processed.get<ComPtr<IMFSample>>();
        ComPtr<IMFMediaBuffer> contiguous_buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&contiguous_buffer)) || !contiguous_buffer)
            continue;

        BYTE* data_ptr = nullptr;
        DWORD data_len = 0;
        if (FAILED(contiguous_buffer->Lock(&data_ptr, nullptr, &data_len)))
            continue;
        ScopeGuard unlock_guard = [&] { (void)contiguous_buffer->Unlock(); };

        if (!data_ptr || data_len == 0)
            continue;

        LONGLONG sample_time = 0;
        LONGLONG sample_duration = 0;
        (void)sample->GetSampleTime(&sample_time);
        (void)sample->GetSampleDuration(&sample_duration);
        UINT32 clean_point = 0;
        bool is_clean_point = SUCCEEDED(sample->GetUINT32(MFSampleExtension_CleanPoint, &clean_point)) && clean_point != 0;

        ReadonlyBytes raw_bytes { data_ptr, data_len };
        auto inspection = inspect_annex_b_nalus(raw_bytes);
        if (inspection.has_sps && inspection.has_pps && !inspection.parameter_sets.is_empty())
            m_state->cached_parameter_sets = inspection.parameter_sets;

        bool is_keyframe = is_clean_point || inspection.has_idr || (inspection.has_sps && inspection.has_pps);
        ByteBuffer payload;
        if (is_keyframe && !(inspection.has_sps && inspection.has_pps) && !m_state->cached_parameter_sets.is_empty())
            DECODER_TRY_ALLOC(payload.try_append(m_state->cached_parameter_sets));
        DECODER_TRY_ALLOC(payload.try_append(raw_bytes));

        DECODER_TRY_ALLOC(out_packets.try_append(EncodedVideoPacket {
            .data = move(payload),
            .timestamp = duration_from_sample_time(sample_time),
            .duration = duration_from_sample_time(sample_duration),
            .is_keyframe = is_keyframe,
        }));
    }
}

}
