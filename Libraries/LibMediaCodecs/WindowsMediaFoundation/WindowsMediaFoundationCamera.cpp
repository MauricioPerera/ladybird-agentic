/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <AK/Mutex.h>
#include <AK/ScopeGuard.h>
#include <LibGfx/Bitmap.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundation.h>
#include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationCamera.h>

#include <mfreadwrite.h>

namespace Media::WindowsMediaFoundation {

struct CameraWorkerState final : public AtomicRefCounted<CameraWorkerState> {
    AK_ALLOC_WITH_KMALLOC;

    Mutex state_mutex;
    ComPtr<IMFMediaSource> source;
    ComPtr<IMFSourceReader> reader;
    GUID subtype { GUID_NULL };
    u32 width { 0 };
    u32 height { 0 };
    i32 default_stride { 0 };
    double frame_rate { 30.0 };

    Mutex callback_mutex;
    VideoCaptureStream::FrameCallback on_frame;
    VideoCaptureStream::ErrorCallback on_ended;

    Atomic<bool> stop_requested { false };
    Atomic<bool> is_running { false };
    Atomic<bool> logged_first_frame { false };
    Atomic<bool> logged_convert_failure { false };
};

struct VideoCaptureStream::Impl {
    AK_ALLOC_WITH_KMALLOC;

    NonnullRefPtr<CameraWorkerState> worker;

    explicit Impl(NonnullRefPtr<CameraWorkerState> w)
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
    auto buffer_or_err = ByteBuffer::create_uninitialized(static_cast<size_t>(needed));
    if (buffer_or_err.is_error())
        return {};
    auto buffer = buffer_or_err.release_value();
    char* ptr = reinterpret_cast<char*>(buffer.data());
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

class SourceReaderCallback final : public IMFSourceReaderCallback {
public:
    explicit SourceReaderCallback(NonnullRefPtr<CameraWorkerState> worker)
        : m_worker(move(worker))
    {
    }

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSourceReaderCallback)) {
            *ppv = static_cast<IMFSourceReaderCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return m_ref_count.fetch_add(1, AK::MemoryOrder::memory_order_relaxed) + 1;
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG count = m_ref_count.fetch_sub(1, AK::MemoryOrder::memory_order_acq_rel) - 1;
        if (count == 0)
            delete this;
        return count;
    }

    STDMETHODIMP OnReadSample(HRESULT hr_status, DWORD, DWORD stream_flags, LONGLONG, IMFSample* sample) override
    {
        if (m_worker->stop_requested.load(AK::MemoryOrder::memory_order_acquire))
            return S_OK;

        if (FAILED(hr_status) || (stream_flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) != 0) {
            dbgln("WMF camera: OnReadSample ended hr=0x{:08x} flags=0x{:x}", static_cast<u32>(hr_status), static_cast<u32>(stream_flags));
            m_worker->is_running.store(false, AK::MemoryOrder::memory_order_release);
            VideoCaptureStream::ErrorCallback ended_cb;
            {
                MutexLocker cb_locker(m_worker->callback_mutex);
                ended_cb = move(m_worker->on_ended);
                m_worker->on_frame = nullptr;
            }
            if (ended_cb && !m_worker->stop_requested.load(AK::MemoryOrder::memory_order_acquire))
                ended_cb();
            return S_OK;
        }

        GUID subtype = GUID_NULL;
        u32 width = 0;
        u32 height = 0;
        i32 stride = 0;
        ComPtr<IMFSourceReader> reader;
        {
            MutexLocker state_locker(m_worker->state_mutex);
            reader = m_worker->reader;
            if ((stream_flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0 && reader) {
                ComPtr<IMFMediaType> updated_type;
                if (SUCCEEDED(reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &updated_type)) && updated_type) {
                    UINT32 new_w = 0;
                    UINT32 new_h = 0;
                    GUID new_sub = GUID_NULL;
                    if (SUCCEEDED(MFGetAttributeSize(updated_type.Get(), MF_MT_FRAME_SIZE, &new_w, &new_h)) && new_w > 0 && new_h > 0) {
                        m_worker->width = new_w;
                        m_worker->height = new_h;
                    }
                    if (SUCCEEDED(updated_type->GetGUID(MF_MT_SUBTYPE, &new_sub)))
                        m_worker->subtype = new_sub;
                    UINT32 new_stride = 0;
                    if (SUCCEEDED(updated_type->GetUINT32(MF_MT_DEFAULT_STRIDE, &new_stride)))
                        m_worker->default_stride = static_cast<i32>(new_stride);
                }
            }
            subtype = m_worker->subtype;
            width = m_worker->width;
            height = m_worker->height;
            stride = m_worker->default_stride;
        }

        if (sample && !m_worker->stop_requested.load(AK::MemoryOrder::memory_order_acquire)) {
            ComPtr<IMFMediaBuffer> contiguous;
            if (SUCCEEDED(sample->ConvertToContiguousBuffer(&contiguous)) && contiguous) {
                BYTE* ptr = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(contiguous->Lock(&ptr, nullptr, &len))) {
                    Optional<Gfx::DecodedImageFrame> frame;
                    if (ptr && len > 0)
                        frame = convert_sample_to_frame({ ptr, len }, subtype, width, height, stride);
                    (void)contiguous->Unlock();

                    if (!frame.has_value() && !m_worker->logged_convert_failure.exchange(true, AK::MemoryOrder::memory_order_acq_rel))
                        dbgln("WMF camera: convert_sample_to_frame failed len={} {}x{} stride={}", len, width, height, stride);

                    if (frame.has_value() && !m_worker->stop_requested.load(AK::MemoryOrder::memory_order_acquire)) {
                        if (!m_worker->logged_first_frame.exchange(true, AK::MemoryOrder::memory_order_acq_rel))
                            dbgln("WMF camera: first frame delivered {}x{}", frame->width(), frame->height());
                        MutexLocker cb_locker(m_worker->callback_mutex);
                        if (m_worker->on_frame && !m_worker->stop_requested.load(AK::MemoryOrder::memory_order_acquire))
                            m_worker->on_frame(frame.release_value());
                    }
                }
            }
        }

        if (!m_worker->stop_requested.load(AK::MemoryOrder::memory_order_acquire) && reader) {
            HRESULT next_hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
            if (FAILED(next_hr) && !m_worker->stop_requested.load(AK::MemoryOrder::memory_order_acquire)) {
                dbgln("WMF camera: next ReadSample failed hr=0x{:08x}", static_cast<u32>(next_hr));
                m_worker->is_running.store(false, AK::MemoryOrder::memory_order_release);
                VideoCaptureStream::ErrorCallback ended_cb;
                {
                    MutexLocker cb_locker(m_worker->callback_mutex);
                    ended_cb = move(m_worker->on_ended);
                    m_worker->on_frame = nullptr;
                }
                if (ended_cb)
                    ended_cb();
            }
        }

        return S_OK;
    }

    STDMETHODIMP OnFlush(DWORD) override
    {
        return S_OK;
    }

    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override
    {
        return S_OK;
    }

private:
    ~SourceReaderCallback() = default;

    Atomic<ULONG> m_ref_count { 1 };
    NonnullRefPtr<CameraWorkerState> m_worker;
};

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

    auto worker = TRY(try_make_ref_counted<CameraWorkerState>());
    {
        MutexLocker cb_locker(worker->callback_mutex);
        worker->on_frame = move(on_frame);
        worker->on_ended = move(on_ended);
    }

    ComPtr<SourceReaderCallback> callback;
    callback.Attach(new (nothrow) SourceReaderCallback(worker));
    if (!callback) {
        (void)source->Shutdown();
        return Error::from_errno(ENOMEM);
    }

    ComPtr<IMFAttributes> reader_attributes;
    if (FAILED(MFCreateAttributes(&reader_attributes, 2))
        || FAILED(reader_attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE))
        || FAILED(reader_attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get()))) {
        (void)source->Shutdown();
        return Error::from_errno(EIO);
    }

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

    {
        MutexLocker state_locker(worker->state_mutex);
        worker->source = source;
        worker->reader = reader;
        worker->subtype = actual_subtype;
        worker->width = actual_width;
        worker->height = actual_height;
        worker->default_stride = default_stride;
        worker->frame_rate = fps;
    }

    worker->is_running.store(true, AK::MemoryOrder::memory_order_release);
    HRESULT read_hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(read_hr)) {
        worker->stop_requested.store(true, AK::MemoryOrder::memory_order_release);
        worker->is_running.store(false, AK::MemoryOrder::memory_order_release);
        {
            MutexLocker state_locker(worker->state_mutex);
            worker->reader.Reset();
            worker->source.Reset();
        }
        (void)reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        reader.Reset();
        (void)source->Shutdown();
        source.Reset();
        return Error::from_errno(EIO);
    }

    auto impl = TRY(adopt_nonnull_own_or_enomem(new (nothrow) Impl(worker)));
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
    m_impl->worker->is_running.store(false, AK::MemoryOrder::memory_order_release);

    {
        MutexLocker cb_locker(m_impl->worker->callback_mutex);
        m_impl->worker->on_frame = nullptr;
        m_impl->worker->on_ended = nullptr;
    }

    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaSource> source;
    {
        MutexLocker state_locker(m_impl->worker->state_mutex);
        reader = move(m_impl->worker->reader);
        source = move(m_impl->worker->source);
    }

    if (reader) {
        (void)reader->Flush(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
        reader.Reset();
    }
    if (source) {
        (void)source->Shutdown();
        source.Reset();
    }
}

u32 VideoCaptureStream::width() const
{
    MutexLocker state_locker(m_impl->worker->state_mutex);
    return m_impl->worker->width;
}

u32 VideoCaptureStream::height() const
{
    MutexLocker state_locker(m_impl->worker->state_mutex);
    return m_impl->worker->height;
}

double VideoCaptureStream::frame_rate() const
{
    MutexLocker state_locker(m_impl->worker->state_mutex);
    return m_impl->worker->frame_rate;
}

bool VideoCaptureStream::is_running() const
{
    return m_impl->worker->is_running.load(AK::MemoryOrder::memory_order_acquire);
}

}
