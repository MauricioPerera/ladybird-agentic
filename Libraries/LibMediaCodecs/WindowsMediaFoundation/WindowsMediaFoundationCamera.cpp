/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/Bitmap.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundation.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationCamera.h>
#include <LibThreading/Thread.h>

#include <mfreadwrite.h>

namespace Media::WindowsMediaFoundation {

struct VideoCaptureStream::WorkerState final : public AtomicRefCounted<WorkerState> {
    AK_ALLOC_WITH_KMALLOC;

    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    GUID subtype { GUID_NULL };
    u32 width { 0 };
    u32 height { 0 };
    i32 default_stride { 0 };
    double frame_rate { 30.0 };
    Atomic<bool> stop_requested { false };
    Atomic<bool> is_running { false };
    Atomic<u32> capture_thread_id { 0 };
    FrameCallback on_frame;
    ErrorCallback on_ended;
};

struct VideoCaptureStream::Impl {
    AK_ALLOC_WITH_KMALLOC;

    NonnullRefPtr<WorkerState> worker;
    RefPtr<Threading::Thread> thread;

    explicit Impl(NonnullRefPtr<WorkerState> w)
        : worker(move(w))
    {
    }
};

namespace {

Optional<String> utf16_alloc_string_to_utf8(WCHAR const* wide, UINT32 length)
{
    if (!wide || length == 0)
        return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    ByteString bytes;
    char* ptr = nullptr;
    auto buffer_or_err = ByteBuffer::create_uninitialized(static_cast<size_t>(needed));
    if (buffer_or_err.is_error())
        return {};
    auto buffer = buffer_or_err.release_value();
    ptr = reinterpret_cast<char*>(buffer.data());
    int written = WideCharToMultiByte(CP_UTF8, 0, wide, static_cast<int>(length), ptr, needed, nullptr, nullptr);
    if (written <= 0)
        return {};
    auto str_or_err = String::from_utf8(StringView { ptr, static_cast<size_t>(written) });
    if (str_or_err.is_error())
        return {};
    return str_or_err.release_value();
}

Optional<String> read_allocated_string_attribute(IMFAttributes& attrs, GUID const& key)
{
    WCHAR* wide = nullptr;
    UINT32 length = 0;
    if (FAILED(attrs.GetAllocatedString(key, &wide, &length)) || !wide)
        return {};
    ScopeGuard free_wide = [&] { CoTaskMemFree(wide); };
    return utf16_alloc_string_to_utf8(wide, length);
}

u8 clamp_u8(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return static_cast<u8>(value);
}

void write_yuv_to_bgrx(int y, int u, int v, u8* dst)
{
    int c = y - 16;
    int d = u - 128;
    int e = v - 128;
    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;
    dst[0] = clamp_u8(b);
    dst[1] = clamp_u8(g);
    dst[2] = clamp_u8(r);
    dst[3] = 0xFF;
}

Optional<Gfx::DecodedImageFrame> convert_sample_to_frame(
    ReadonlyBytes data,
    GUID const& subtype,
    u32 width,
    u32 height,
    i32 default_stride)
{
    if (width == 0 || height == 0 || data.is_empty())
        return {};

    auto bitmap_or_err = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRx8888, { static_cast<int>(width), static_cast<int>(height) });
    if (bitmap_or_err.is_error())
        return {};
    auto bitmap = bitmap_or_err.release_value();

    if (subtype == MFVideoFormat_RGB32 || subtype == MFVideoFormat_ARGB32) {
        i32 stride = default_stride != 0 ? default_stride : static_cast<i32>(width * 4);
        size_t abs_stride = static_cast<size_t>(stride < 0 ? -stride : stride);
        if (abs_stride < static_cast<size_t>(width) * 4 || data.size() < abs_stride * height)
            return {};

        u8 const* src_base = data.data();
        for (u32 row = 0; row < height; ++row) {
            u32 src_row = stride < 0 ? (height - 1 - row) : row;
            u8 const* src = src_base + static_cast<size_t>(src_row) * abs_stride;
            u8* dst = bitmap->scanline_u8(static_cast<int>(row));
            memcpy(dst, src, static_cast<size_t>(width) * 4);
            for (u32 col = 0; col < width; ++col)
                dst[col * 4 + 3] = 0xFF;
        }
    } else if (subtype == MFVideoFormat_NV12) {
        size_t stride = default_stride > 0 ? static_cast<size_t>(default_stride) : static_cast<size_t>(width);
        if (stride < width || data.size() < stride * height + (stride * height) / 2)
            return {};
        u8 const* y_plane = data.data();
        u8 const* uv_plane = y_plane + stride * height;
        for (u32 row = 0; row < height; ++row) {
            u8 const* y_row = y_plane + static_cast<size_t>(row) * stride;
            u8 const* uv_row = uv_plane + static_cast<size_t>(row / 2) * stride;
            u8* dst = bitmap->scanline_u8(static_cast<int>(row));
            for (u32 col = 0; col < width; ++col) {
                size_t uv_col = (col / 2) * 2;
                write_yuv_to_bgrx(y_row[col], uv_row[uv_col], uv_row[uv_col + 1], dst + col * 4);
            }
        }
    } else if (subtype == MFVideoFormat_YUY2) {
        size_t stride = default_stride > 0 ? static_cast<size_t>(default_stride) : static_cast<size_t>(width) * 2;
        if (stride < static_cast<size_t>(width) * 2 || data.size() < stride * height)
            return {};
        for (u32 row = 0; row < height; ++row) {
            u8 const* src = data.data() + static_cast<size_t>(row) * stride;
            u8* dst = bitmap->scanline_u8(static_cast<int>(row));
            for (u32 col = 0; col + 1 < width; col += 2) {
                u8 y0 = src[col * 2 + 0];
                u8 u = src[col * 2 + 1];
                u8 y1 = src[col * 2 + 2];
                u8 v = src[col * 2 + 3];
                write_yuv_to_bgrx(y0, u, v, dst + col * 4);
                write_yuv_to_bgrx(y1, u, v, dst + (col + 1) * 4);
            }
        }
    } else {
        return {};
    }

    auto frame_or_err = Gfx::DecodedImageFrame::create(*bitmap);
    if (frame_or_err.is_error())
        return {};
    return frame_or_err.release_value();
}

HRESULT try_configure_reader_output(IMFSourceReader& reader, GUID const& subtype, u32 width, u32 height)
{
    ComPtr<IMFMediaType> media_type;
    HRESULT hr = MFCreateMediaType(&media_type);
    if (FAILED(hr))
        return hr;
    hr = media_type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (FAILED(hr))
        return hr;
    hr = media_type->SetGUID(MF_MT_SUBTYPE, subtype);
    if (FAILED(hr))
        return hr;
    if (width > 0 && height > 0)
        (void)MFSetAttributeSize(media_type.Get(), MF_MT_FRAME_SIZE, width, height);
    return reader.SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, media_type.Get());
}

}

Vector<VideoCaptureDeviceInfo> enumerate_video_capture_devices()
{
    Vector<VideoCaptureDeviceInfo> devices;
    if (!ensure_media_foundation_started())
        return devices;

    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 1)))
        return devices;
    if (FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)))
        return devices;

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attributes.Get(), &activates, &count)))
        return devices;

    ScopeGuard release_activates = [&] {
        for (UINT32 i = 0; i < count; ++i)
            activates[i]->Release();
        CoTaskMemFree(static_cast<void*>(activates));
    };

    for (UINT32 i = 0; i < count; ++i) {
        auto label = read_allocated_string_attribute(*activates[i], MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
        auto sym_link = read_allocated_string_attribute(*activates[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);

        String device_id;
        if (sym_link.has_value() && !sym_link->is_empty())
            device_id = MUST(String::formatted("wmf:video:{}", *sym_link));
        else
            device_id = MUST(String::formatted("wmf:video:index:{}", i));

        devices.append(VideoCaptureDeviceInfo {
            .device_id = move(device_id),
            .label = label.value_or("Camera"_string),
            .group_id = {},
            .is_default = (i == 0),
        });
    }

    return devices;
}

ErrorOr<NonnullRefPtr<VideoCaptureStream>> VideoCaptureStream::create(
    StringView device_id,
    u32 requested_width,
    u32 requested_height,
    u32 requested_fps,
    FrameCallback on_frame,
    ErrorCallback on_ended)
{
    (void)requested_fps;
    if (!ensure_media_foundation_started())
        return Error::from_string_literal("Media Foundation could not be started");

    ComPtr<IMFAttributes> attributes;
    if (FAILED(MFCreateAttributes(&attributes, 1))
        || FAILED(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID))) {
        return Error::from_string_literal("Failed to create Media Foundation video capture attributes");
    }

    IMFActivate** activates = nullptr;
    UINT32 count = 0;
    if (FAILED(MFEnumDeviceSources(attributes.Get(), &activates, &count)))
        return Error::from_errno(EIO);

    ScopeGuard release_activates = [&] {
        for (UINT32 i = 0; i < count; ++i)
            activates[i]->Release();
        CoTaskMemFree(static_cast<void*>(activates));
    };

    if (count == 0)
        return Error::from_errno(ENOENT);

    Optional<UINT32> selected_index;
    if (device_id.is_empty()) {
        selected_index = 0;
    } else {
        for (UINT32 i = 0; i < count; ++i) {
            auto sym_link = read_allocated_string_attribute(*activates[i], MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
            String candidate_id = sym_link.has_value() && !sym_link->is_empty()
                ? MUST(String::formatted("wmf:video:{}", *sym_link))
                : MUST(String::formatted("wmf:video:index:{}", i));
            if (candidate_id == device_id || (sym_link.has_value() && sym_link.value() == device_id)) {
                selected_index = i;
                break;
            }
        }
    }

    if (!selected_index.has_value())
        return Error::from_errno(ENOENT);

    ComPtr<IMFMediaSource> source;
    if (FAILED(activates[*selected_index]->ActivateObject(IID_PPV_ARGS(&source))) || !source)
        return Error::from_errno(EIO);

    ComPtr<IMFAttributes> reader_attributes;
    if (SUCCEEDED(MFCreateAttributes(&reader_attributes, 1)))
        (void)reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromMediaSource(source.Get(), reader_attributes.Get(), &reader)) || !reader) {
        (void)source->Shutdown();
        return Error::from_errno(EIO);
    }

    (void)reader->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    (void)reader->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    HRESULT format_hr = try_configure_reader_output(*reader.Get(), MFVideoFormat_RGB32, requested_width, requested_height);
    if (FAILED(format_hr))
        format_hr = try_configure_reader_output(*reader.Get(), MFVideoFormat_RGB32, 0, 0);
    if (FAILED(format_hr))
        format_hr = try_configure_reader_output(*reader.Get(), MFVideoFormat_NV12, 0, 0);
    if (FAILED(format_hr))
        format_hr = try_configure_reader_output(*reader.Get(), MFVideoFormat_YUY2, 0, 0);
    if (FAILED(format_hr)) {
        (void)source->Shutdown();
        return Error::from_errno(EIO);
    }

    ComPtr<IMFMediaType> current_type;
    if (FAILED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &current_type)) || !current_type) {
        (void)source->Shutdown();
        return Error::from_errno(EIO);
    }

    UINT32 actual_width = 0;
    UINT32 actual_height = 0;
    GUID actual_subtype = GUID_NULL;
    if (FAILED(MFGetAttributeSize(current_type.Get(), MF_MT_FRAME_SIZE, &actual_width, &actual_height))
        || FAILED(current_type->GetGUID(MF_MT_SUBTYPE, &actual_subtype))
        || actual_width == 0 || actual_height == 0) {
        (void)source->Shutdown();
        return Error::from_errno(EIO);
    }

    i32 default_stride = 0;
    UINT32 raw_stride = 0;
    if (SUCCEEDED(current_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &raw_stride)))
        default_stride = static_cast<i32>(raw_stride);

    double fps = 30.0;
    UINT32 fps_num = 0;
    UINT32 fps_den = 0;
    if (SUCCEEDED(MFGetAttributeRatio(current_type.Get(), MF_MT_FRAME_RATE, &fps_num, &fps_den)) && fps_num > 0 && fps_den > 0)
        fps = static_cast<double>(fps_num) / static_cast<double>(fps_den);

    auto worker = TRY(try_make_ref_counted<WorkerState>());
    worker->source = move(source);
    worker->reader = move(reader);
    worker->subtype = actual_subtype;
    worker->width = actual_width;
    worker->height = actual_height;
    worker->default_stride = default_stride;
    worker->frame_rate = fps;
    worker->on_frame = move(on_frame);
    worker->on_ended = move(on_ended);
    worker->is_running.store(true, AK::MemoryOrder::memory_order_release);

    auto impl = TRY(adopt_nonnull_own_or_enomem(new (nothrow) Impl(worker)));
    auto thread = TRY(Threading::Thread::try_create("WMF Camera"sv, [worker]() -> intptr_t {
        worker->capture_thread_id.store(GetCurrentThreadId(), AK::MemoryOrder::memory_order_release);
        bool unexpected_end = false;

        while (!worker->stop_requested.load(AK::MemoryOrder::memory_order_acquire)) {
            DWORD stream_index = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            HRESULT read_hr = worker->reader->ReadSample(
                MF_SOURCE_READER_FIRST_VIDEO_STREAM,
                0,
                &stream_index,
                &flags,
                &timestamp,
                &sample);

            if (worker->stop_requested.load(AK::MemoryOrder::memory_order_acquire))
                break;

            if (FAILED(read_hr) || (flags & MF_SOURCE_READERF_ENDOFSTREAM) || (flags & MF_SOURCE_READERF_ERROR)) {
                unexpected_end = true;
                break;
            }

            if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
                ComPtr<IMFMediaType> updated_type;
                if (SUCCEEDED(worker->reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &updated_type)) && updated_type) {
                    UINT32 new_w = 0;
                    UINT32 new_h = 0;
                    GUID new_sub = GUID_NULL;
                    if (SUCCEEDED(MFGetAttributeSize(updated_type.Get(), MF_MT_FRAME_SIZE, &new_w, &new_h)) && new_w > 0 && new_h > 0) {
                        worker->width = new_w;
                        worker->height = new_h;
                    }
                    if (SUCCEEDED(updated_type->GetGUID(MF_MT_SUBTYPE, &new_sub)))
                        worker->subtype = new_sub;
                    UINT32 new_stride = 0;
                    if (SUCCEEDED(updated_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &new_stride)))
                        worker->default_stride = static_cast<i32>(new_stride);
                }
            }

            if (!sample)
                continue;

            ComPtr<IMFMediaBuffer> contiguous;
            if (FAILED(sample->ConvertToContiguousBuffer(&contiguous)) || !contiguous)
                continue;

            BYTE* ptr = nullptr;
            DWORD len = 0;
            if (FAILED(contiguous->Lock(&ptr, nullptr, &len)))
                continue;

            Optional<Gfx::DecodedImageFrame> frame;
            if (ptr && len > 0)
                frame = convert_sample_to_frame({ ptr, len }, worker->subtype, worker->width, worker->height, worker->default_stride);

            (void)contiguous->Unlock();

            if (frame.has_value() && worker->on_frame)
                worker->on_frame(frame.release_value());
        }

        worker->is_running.store(false, AK::MemoryOrder::memory_order_release);
        if (unexpected_end && !worker->stop_requested.load(AK::MemoryOrder::memory_order_acquire) && worker->on_ended)
            worker->on_ended();

        worker->reader.Reset();
        if (worker->source) {
            (void)worker->source->Shutdown();
            worker->source.Reset();
        }
        return 0;
    }));

    impl->thread = thread;
    thread->start();

    return TRY(adopt_nonnull_ref_or_enomem(new (nothrow) VideoCaptureStream(move(impl))));
}

VideoCaptureStream::VideoCaptureStream(NonnullOwnPtr<Impl> impl)
    : m_impl(move(impl))
{
}

VideoCaptureStream::~VideoCaptureStream()
{
    stop();
}

void VideoCaptureStream::stop()
{
    m_impl->worker->stop_requested.store(true, AK::MemoryOrder::memory_order_release);
    if (m_impl->worker->source)
        (void)m_impl->worker->source->Shutdown();

    if (m_impl->thread) {
        auto thread = move(m_impl->thread);
        u32 capture_tid = m_impl->worker->capture_thread_id.load(AK::MemoryOrder::memory_order_acquire);
        if (capture_tid != 0 && capture_tid == GetCurrentThreadId())
            thread->detach();
        else
            (void)thread->join();
    }
    m_impl->worker->is_running.store(false, AK::MemoryOrder::memory_order_release);
}

u32 VideoCaptureStream::width() const
{
    return m_impl->worker->width;
}

u32 VideoCaptureStream::height() const
{
    return m_impl->worker->height;
}

double VideoCaptureStream::frame_rate() const
{
    return m_impl->worker->frame_rate;
}

bool VideoCaptureStream::is_running() const
{
    return m_impl->worker->is_running.load(AK::MemoryOrder::memory_order_acquire);
}

}
