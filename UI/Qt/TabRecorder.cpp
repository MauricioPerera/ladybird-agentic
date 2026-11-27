/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TabRecorder.h"
#include "StringUtils.h"
#include "WebContentView.h"

#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Math.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibMediaCodecs/SpeakerTimeline.h>
#include <LibThreading/Thread.h>

#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QStandardPaths>

#include <cstring>

#ifdef Q_OS_WIN
#    include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationMp4Recorder.h>
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <windows.h>
#    include <initguid.h>
#    include <audioclient.h>
#    include <mmdeviceapi.h>
#    include <propsys.h>
#    include <propkeydef.h>
#    include <functiondiscoverykeys_devpkey.h>
#    include <wrl/client.h>
#endif

namespace Ladybird {

namespace {

constexpr u32 DEFAULT_TARGET_FPS = 30;

#ifdef Q_OS_WIN

constexpr u32 TARGET_AUDIO_SAMPLE_RATE = 48000;
constexpr u16 TARGET_AUDIO_CHANNELS = 2;
constexpr size_t AUDIO_CHUNK_FRAMES = 960; // 20 ms at 48 kHz
constexpr float FALLBACK_VAD_RMS_THRESHOLD = 0.015f;
constexpr auto FALLBACK_VAD_HOLDOVER = AK::Duration::from_milliseconds(450);

String sanitize_filename_part(StringView input)
{
    StringBuilder builder;
    for (u8 byte : input.bytes()) {
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') || (byte >= '0' && byte <= '9') || byte == '-' || byte == '_') {
            builder.append(static_cast<char>(byte));
        } else if (byte == ' ') {
            builder.append('_');
        }
        if (builder.length() >= 32)
            break;
    }
    return MUST(builder.to_string());
}

using Microsoft::WRL::ComPtr;

Vector<wchar_t> utf8_to_wide(StringView value)
{
    if (value.is_empty())
        return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, value.characters_without_null_termination(), static_cast<int>(value.length()), nullptr, 0);
    if (length <= 0)
        return {};
    Vector<wchar_t> wide;
    wide.resize(static_cast<size_t>(length) + 1);
    int written = MultiByteToWideChar(CP_UTF8, 0, value.characters_without_null_termination(), static_cast<int>(value.length()), wide.data(), length);
    if (written <= 0)
        return {};
    wide[static_cast<size_t>(written)] = L'\0';
    return wide;
}

Optional<String> wide_to_utf8(WCHAR const* wide)
{
    if (!wide || *wide == L'\0')
        return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return {};
    auto buffer_or_err = ByteBuffer::create_uninitialized(static_cast<size_t>(needed));
    if (buffer_or_err.is_error())
        return {};
    auto buffer = buffer_or_err.release_value();
    int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, reinterpret_cast<char*>(buffer.data()), needed, nullptr, nullptr);
    if (written <= 1 || written > needed)
        return {};
    auto str_or_err = String::from_utf8(StringView { reinterpret_cast<char const*>(buffer.data()), static_cast<size_t>(written - 1) });
    if (str_or_err.is_error())
        return {};
    return str_or_err.release_value();
}

Optional<String> read_endpoint_friendly_name(IMMDevice& device)
{
    ComPtr<IPropertyStore> properties;
    if (FAILED(device.OpenPropertyStore(STGM_READ, &properties)) || !properties)
        return {};

    PROPVARIANT value;
    PropVariantInit(&value);
    ScopeGuard clear_prop = [&] { PropVariantClear(&value); };
    if (FAILED(properties->GetValue(PKEY_Device_FriendlyName, &value)))
        return {};
    if (value.vt != VT_LPWSTR || !value.pwszVal)
        return {};
    return wide_to_utf8(value.pwszVal);
}

struct ScopedComInitializer {
    explicit ScopedComInitializer(DWORD coinit = COINIT_MULTITHREADED)
    {
        HRESULT hr = CoInitializeEx(nullptr, coinit);
        if (hr == S_OK || hr == S_FALSE) {
            m_initialized = true;
            m_should_uninitialize = true;
        } else if (hr == RPC_E_CHANGED_MODE) {
            m_initialized = true;
            m_should_uninitialize = false;
        }
    }

    ~ScopedComInitializer()
    {
        if (m_should_uninitialize)
            CoUninitialize();
    }

    bool initialized() const { return m_initialized; }

private:
    bool m_initialized { false };
    bool m_should_uninitialize { false };
};

enum class SampleEncoding {
    Float32,
    Int16,
    Int24,
    Int32,
};

struct WasapiCaptureSource {
    ComPtr<IAudioClient> audio_client;
    ComPtr<IAudioCaptureClient> capture_client;
    u32 sample_rate { TARGET_AUDIO_SAMPLE_RATE };
    u32 channels { TARGET_AUDIO_CHANNELS };
    u32 bytes_per_sample { sizeof(float) };
    SampleEncoding encoding { SampleEncoding::Float32 };
    Vector<float> pending_stereo_samples;
    bool initialized { false };
    bool active { false };

    ~WasapiCaptureSource()
    {
        if (audio_client && active)
            audio_client->Stop();
    }
};

float decode_sample(u8 const* frame_ptr, u32 channel, u32 bytes_per_sample, SampleEncoding encoding)
{
    u8 const* ptr = frame_ptr + static_cast<size_t>(channel) * bytes_per_sample;
    switch (encoding) {
    case SampleEncoding::Float32: {
        float val = 0.0f;
        std::memcpy(&val, ptr, sizeof(val));
        return val;
    }
    case SampleEncoding::Int16: {
        i16 val = 0;
        std::memcpy(&val, ptr, sizeof(val));
        return static_cast<float>(val) / 32768.0f;
    }
    case SampleEncoding::Int24: {
        i32 raw = static_cast<i32>(ptr[0] | (ptr[1] << 8) | (ptr[2] << 16));
        if (raw & 0x800000)
            raw |= ~0xFFFFFF;
        return static_cast<float>(raw) / 8388608.0f;
    }
    case SampleEncoding::Int32: {
        i32 val = 0;
        std::memcpy(&val, ptr, sizeof(val));
        return static_cast<float>(static_cast<double>(val) / 2147483648.0);
    }
    }
    return 0.0f;
}

bool detect_sample_format(WAVEFORMATEX const* fmt, SampleEncoding& encoding, u32& bytes_per_sample)
{
    if (!fmt)
        return false;

    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && fmt->wBitsPerSample == 32) {
        encoding = SampleEncoding::Float32;
        bytes_per_sample = 4;
        return true;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_PCM) {
        if (fmt->wBitsPerSample == 16) {
            encoding = SampleEncoding::Int16;
            bytes_per_sample = 2;
            return true;
        }
        if (fmt->wBitsPerSample == 24) {
            encoding = SampleEncoding::Int24;
            bytes_per_sample = 3;
            return true;
        }
        if (fmt->wBitsPerSample == 32) {
            encoding = SampleEncoding::Int32;
            bytes_per_sample = 4;
            return true;
        }
    }
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        auto const* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE const*>(fmt);
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT && fmt->wBitsPerSample == 32) {
            encoding = SampleEncoding::Float32;
            bytes_per_sample = 4;
            return true;
        }
        if (ext->SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            if (fmt->wBitsPerSample == 16) {
                encoding = SampleEncoding::Int16;
                bytes_per_sample = 2;
                return true;
            }
            if (fmt->wBitsPerSample == 24) {
                encoding = SampleEncoding::Int24;
                bytes_per_sample = 3;
                return true;
            }
            if (fmt->wBitsPerSample == 32) {
                encoding = SampleEncoding::Int32;
                bytes_per_sample = 4;
                return true;
            }
        }
    }
    return false;
}

bool open_wasapi_source(WasapiCaptureSource& out, bool loopback, StringView preferred_device_id = {})
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator)
        return false;

    ComPtr<IMMDevice> device;
    if (!loopback && !preferred_device_id.is_empty()) {
        auto wide_id = utf8_to_wide(preferred_device_id);
        if (!wide_id.is_empty()) {
            ComPtr<IMMDevice> selected_device;
            if (SUCCEEDED(enumerator->GetDevice(wide_id.data(), &selected_device)) && selected_device) {
                DWORD state = 0;
                if (SUCCEEDED(selected_device->GetState(&state)) && state == DEVICE_STATE_ACTIVE)
                    device = move(selected_device);
            }
        }
    }

    if (!device) {
        hr = enumerator->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &device);
        if (FAILED(hr) || !device)
            return false;
    }

    ComPtr<IAudioClient> audio_client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audio_client.GetAddressOf()));
    if (FAILED(hr) || !audio_client)
        return false;

    WAVEFORMATEX* mix_format = nullptr;
    hr = audio_client->GetMixFormat(&mix_format);
    if (FAILED(hr) || !mix_format)
        return false;
    ScopeGuard free_mix_format = [&] { CoTaskMemFree(mix_format); };

    WAVEFORMATEXTENSIBLE desired {};
    desired.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    desired.Format.nChannels = TARGET_AUDIO_CHANNELS;
    desired.Format.nSamplesPerSec = TARGET_AUDIO_SAMPLE_RATE;
    desired.Format.wBitsPerSample = 32;
    desired.Format.nBlockAlign = TARGET_AUDIO_CHANNELS * sizeof(float);
    desired.Format.nAvgBytesPerSec = TARGET_AUDIO_SAMPLE_RATE * desired.Format.nBlockAlign;
    desired.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    desired.Samples.wValidBitsPerSample = 32;
    desired.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    desired.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    REFERENCE_TIME buffer_duration = 1'000'000; // 100 ms shared buffer
    DWORD base_flags = (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0) | AUDCLNT_STREAMFLAGS_NOPERSIST;
    DWORD autoconvert_flags = base_flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        autoconvert_flags,
        buffer_duration,
        0,
        reinterpret_cast<WAVEFORMATEX const*>(&desired),
        nullptr);

    if (SUCCEEDED(hr)) {
        out.sample_rate = TARGET_AUDIO_SAMPLE_RATE;
        out.channels = TARGET_AUDIO_CHANNELS;
        out.bytes_per_sample = sizeof(float);
        out.encoding = SampleEncoding::Float32;
    } else {
        if (!detect_sample_format(mix_format, out.encoding, out.bytes_per_sample))
            return false;
        out.sample_rate = mix_format->nSamplesPerSec > 0 ? mix_format->nSamplesPerSec : TARGET_AUDIO_SAMPLE_RATE;
        out.channels = mix_format->nChannels > 0 ? mix_format->nChannels : 1;

        hr = audio_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            base_flags,
            buffer_duration,
            0,
            mix_format,
            nullptr);
        if (FAILED(hr))
            return false;
    }

    ComPtr<IAudioCaptureClient> capture_client;
    hr = audio_client->GetService(IID_PPV_ARGS(&capture_client));
    if (FAILED(hr) || !capture_client)
        return false;

    hr = audio_client->Start();
    if (FAILED(hr))
        return false;

    out.audio_client = move(audio_client);
    out.capture_client = move(capture_client);
    out.initialized = true;
    out.active = true;
    return true;
}

void drain_wasapi_source(WasapiCaptureSource& source)
{
    if (!source.initialized || !source.capture_client)
        return;

    constexpr size_t MAX_BUFFERED_SAMPLES = TARGET_AUDIO_SAMPLE_RATE * TARGET_AUDIO_CHANNELS / 5; // 200 ms cap

    UINT32 packet_frames = 0;
    while (SUCCEEDED(source.capture_client->GetNextPacketSize(&packet_frames)) && packet_frames > 0) {
        BYTE* data = nullptr;
        UINT32 num_frames = 0;
        DWORD flags = 0;
        HRESULT hr = source.capture_client->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
        if (FAILED(hr))
            break;

        ScopeGuard release_buffer = [&] {
            source.capture_client->ReleaseBuffer(num_frames);
        };

        if (num_frames == 0)
            continue;

        bool is_silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr;
        size_t frame_stride = static_cast<size_t>(source.channels) * source.bytes_per_sample;

        if (source.sample_rate == TARGET_AUDIO_SAMPLE_RATE) {
            size_t old_size = source.pending_stereo_samples.size();
            source.pending_stereo_samples.resize(old_size + static_cast<size_t>(num_frames) * 2);
            for (UINT32 i = 0; i < num_frames; ++i) {
                float left = 0.0f;
                float right = 0.0f;
                if (!is_silent) {
                    u8 const* frame_ptr = data + static_cast<size_t>(i) * frame_stride;
                    left = decode_sample(frame_ptr, 0, source.bytes_per_sample, source.encoding);
                    right = source.channels > 1
                        ? decode_sample(frame_ptr, 1, source.bytes_per_sample, source.encoding)
                        : left;
                }
                source.pending_stereo_samples[old_size + i * 2] = left;
                source.pending_stereo_samples[old_size + i * 2 + 1] = right;
            }
        } else {
            double ratio = static_cast<double>(TARGET_AUDIO_SAMPLE_RATE) / static_cast<double>(source.sample_rate);
            size_t out_frames = max<size_t>(1, static_cast<size_t>(static_cast<double>(num_frames) * ratio));
            size_t old_size = source.pending_stereo_samples.size();
            source.pending_stereo_samples.resize(old_size + out_frames * 2);
            for (size_t i = 0; i < out_frames; ++i) {
                float left = 0.0f;
                float right = 0.0f;
                if (!is_silent) {
                    double src_pos = (static_cast<double>(i) * static_cast<double>(source.sample_rate)) / static_cast<double>(TARGET_AUDIO_SAMPLE_RATE);
                    UINT32 idx0 = min(static_cast<UINT32>(src_pos), num_frames - 1);
                    UINT32 idx1 = min(idx0 + 1, num_frames - 1);
                    float frac = static_cast<float>(src_pos - static_cast<double>(idx0));
                    u8 const* ptr0 = data + static_cast<size_t>(idx0) * frame_stride;
                    u8 const* ptr1 = data + static_cast<size_t>(idx1) * frame_stride;

                    float l0 = decode_sample(ptr0, 0, source.bytes_per_sample, source.encoding);
                    float l1 = decode_sample(ptr1, 0, source.bytes_per_sample, source.encoding);
                    left = l0 + (l1 - l0) * frac;

                    if (source.channels > 1) {
                        float r0 = decode_sample(ptr0, 1, source.bytes_per_sample, source.encoding);
                        float r1 = decode_sample(ptr1, 1, source.bytes_per_sample, source.encoding);
                        right = r0 + (r1 - r0) * frac;
                    } else {
                        right = left;
                    }
                }
                source.pending_stereo_samples[old_size + i * 2] = left;
                source.pending_stereo_samples[old_size + i * 2 + 1] = right;
            }
        }
    }

    if (source.pending_stereo_samples.size() > MAX_BUFFERED_SAMPLES) {
        size_t excess = source.pending_stereo_samples.size() - MAX_BUFFERED_SAMPLES;
        source.pending_stereo_samples.remove(0, excess);
    }
}

float pop_chunk_and_compute_rms(WasapiCaptureSource& source, Span<float> dst)
{
    size_t available = min(dst.size(), source.pending_stereo_samples.size());
    double sum_sq = 0.0;
    for (size_t i = 0; i < available; ++i) {
        float s = source.pending_stereo_samples[i];
        dst[i] = s;
        sum_sq += static_cast<double>(s) * static_cast<double>(s);
    }
    if (available > 0)
        source.pending_stereo_samples.remove(0, available);
    for (size_t i = available; i < dst.size(); ++i)
        dst[i] = 0.0f;
    return dst.is_empty() ? 0.0f : AK::sqrt(static_cast<float>(sum_sq / static_cast<double>(dst.size())));
}

#endif

}

TabRecorder::TabRecorder(WebContentView& view, QObject* parent)
    : QObject(parent)
    , m_view(view)
{
    m_frame_timer.setTimerType(Qt::PreciseTimer);
    m_frame_timer.setInterval(1000 / DEFAULT_TARGET_FPS);
    QObject::connect(&m_frame_timer, &QTimer::timeout, this, [this] { capture_video_tick(); });
}

namespace {

TabRecorderSettings& raw_settings_storage()
{
    static TabRecorderSettings s_settings;
    return s_settings;
}

}

TabRecorderSettings& TabRecorder::settings()
{
    static bool s_loaded = [] {
        load_settings();
        return true;
    }();
    (void)s_loaded;
    return raw_settings_storage();
}

String TabRecorder::settings_file_path()
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    if (dir.isEmpty())
        dir = qstring_from_ak_string(recordings_directory());
    QDir().mkpath(dir);
    return ak_string_from_qstring(QDir::toNativeSeparators(QDir(dir).filePath("tab_recorder_settings.json")));
}

void TabRecorder::load_settings()
{
    auto path = settings_file_path();
    auto file_or_err = Core::File::open(path, Core::File::OpenMode::Read);
    if (file_or_err.is_error())
        return;
    auto bytes_or_err = file_or_err.value()->read_until_eof();
    if (bytes_or_err.is_error())
        return;
    auto const& bytes = bytes_or_err.value();
    auto parsed_or_err = JsonValue::from_string(StringView { bytes });
    if (parsed_or_err.is_error() || !parsed_or_err.value().is_object())
        return;

    auto const& obj = parsed_or_err.value().as_object();
    auto& target = raw_settings_storage();
    if (auto v = obj.get_bool("record_microphone"sv); v.has_value())
        target.record_microphone = *v;
    if (auto v = obj.get_bool("record_system_audio"sv); v.has_value())
        target.record_system_audio = *v;
    if (auto v = obj.get_string("microphone_device_id"sv); v.has_value())
        target.microphone_device_id = *v;
    if (auto v = obj.get_bool("show_cursor"sv); v.has_value())
        target.show_cursor = *v;
    if (auto v = obj.get_bool("export_speaker_timeline"sv); v.has_value())
        target.export_speaker_timeline = *v;
    if (auto v = obj.get_u32("batch_duration_seconds"sv); v.has_value())
        target.batch_duration_seconds = *v;
    if (auto v = obj.get_u32("fps"sv); v.has_value())
        target.fps = clamp<u32>(*v, 10, 60);
}

void TabRecorder::save_settings()
{
    auto const& s = raw_settings_storage();
    JsonObject obj;
    obj.set("record_microphone"sv, s.record_microphone);
    obj.set("record_system_audio"sv, s.record_system_audio);
    obj.set("microphone_device_id"sv, s.microphone_device_id);
    obj.set("show_cursor"sv, s.show_cursor);
    obj.set("export_speaker_timeline"sv, s.export_speaker_timeline);
    obj.set("batch_duration_seconds"sv, s.batch_duration_seconds);
    obj.set("fps"sv, s.fps);

    auto json = obj.serialized();
    auto path = settings_file_path();
    auto file_or_err = Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate);
    if (file_or_err.is_error())
        return;
    (void)file_or_err.value()->write_until_depleted(json.bytes());
}

Vector<MicrophoneDeviceInfo> TabRecorder::enumerate_microphones()
{
    Vector<MicrophoneDeviceInfo> result;
#ifdef Q_OS_WIN
    ScopedComInitializer com(COINIT_MULTITHREADED);
    if (!com.initialized())
        return result;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) || !enumerator)
        return result;

    Optional<String> default_id;
    {
        ComPtr<IMMDevice> default_device;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &default_device)) && default_device) {
            LPWSTR default_id_wide = nullptr;
            if (SUCCEEDED(default_device->GetId(&default_id_wide)) && default_id_wide) {
                ScopeGuard free_default_id = [&] { CoTaskMemFree(default_id_wide); };
                default_id = wide_to_utf8(default_id_wide);
            }
        }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection)) || !collection)
        return result;

    UINT count = 0;
    if (FAILED(collection->GetCount(&count)))
        return result;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)) || !device)
            continue;

        LPWSTR id_wide = nullptr;
        if (FAILED(device->GetId(&id_wide)) || !id_wide)
            continue;
        ScopeGuard free_id = [&] { CoTaskMemFree(id_wide); };

        auto id_utf8 = wide_to_utf8(id_wide);
        if (!id_utf8.has_value() || id_utf8->is_empty())
            continue;

        auto label = read_endpoint_friendly_name(*device.Get()).value_or("Microphone"_string);
        bool is_default = default_id.has_value() && *default_id == *id_utf8;
        result.append(MicrophoneDeviceInfo {
            .device_id = *id_utf8,
            .label = move(label),
            .is_default = is_default,
        });
    }
#endif
    return result;
}

TabRecorder::~TabRecorder()
{
    if (m_is_recording) {
        (void)stop();
        return;
    }
#ifdef Q_OS_WIN
    m_audio_worker_stop.store(true, AK::MemoryOrder::memory_order_release);
    if (m_stop_event)
        SetEvent(static_cast<HANDLE>(m_stop_event));
    if (m_audio_thread) {
        (void)m_audio_thread->join();
        m_audio_thread = nullptr;
    }
    {
        AK::MutexLocker locker(m_video_queue_mutex);
        m_stop_requested = true;
    }
    if (m_video_event)
        SetEvent(static_cast<HANDLE>(m_video_event));
    if (m_video_thread) {
        (void)m_video_thread->join();
        m_video_thread = nullptr;
    }
    if (m_stop_event) {
        CloseHandle(static_cast<HANDLE>(m_stop_event));
        m_stop_event = nullptr;
    }
    if (m_video_event) {
        CloseHandle(static_cast<HANDLE>(m_video_event));
        m_video_event = nullptr;
    }
    if (m_recorder_init_event) {
        CloseHandle(static_cast<HANDLE>(m_recorder_init_event));
        m_recorder_init_event = nullptr;
    }
#endif
}

void TabRecorder::handle_worker_failure([[maybe_unused]] Error error)
{
#ifdef Q_OS_WIN
    m_recorder_finalize_error = move(error);
#endif
    m_audio_worker_stop.store(true, AK::MemoryOrder::memory_order_release);
#ifdef Q_OS_WIN
    if (m_stop_event)
        SetEvent(static_cast<HANDLE>(m_stop_event));
#endif
    if (!m_worker_failed.exchange(true, AK::MemoryOrder::memory_order_acq_rel)) {
        QPointer<TabRecorder> self(this);
        QMetaObject::invokeMethod(this, [self]() {
            if (self && self->is_recording())
                (void)self->stop();
        }, Qt::QueuedConnection);
    }
}

AK::Duration TabRecorder::elapsed() const
{
    if (!m_is_recording)
        return AK::Duration::zero();
    return MonotonicTime::now_coarse() - m_start_time;
}

String TabRecorder::recordings_directory()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/Videos";
    QString folder = QDir(base).filePath("Ladybird Recordings");
    QDir().mkpath(folder);
    return ak_string_from_qstring(QDir::toNativeSeparators(folder));
}

ErrorOr<void> TabRecorder::start([[maybe_unused]] StringView page_title)
{
    if (m_is_recording)
        return {};

#ifndef Q_OS_WIN
    return Error::from_string_literal("Native tab recording is currently only supported on Windows");
#else
    if (m_video_thread) {
        if (!m_video_thread->has_exited())
            return Error::from_string_literal("Previous recorder worker is still shutting down");
        if (m_video_thread->needs_to_be_joined())
            (void)m_video_thread->join();
        m_video_thread = nullptr;
    }
    if (m_stop_event) {
        CloseHandle(static_cast<HANDLE>(m_stop_event));
        m_stop_event = nullptr;
    }
    if (m_video_event) {
        CloseHandle(static_cast<HANDLE>(m_video_event));
        m_video_event = nullptr;
    }
    if (m_recorder_init_event) {
        CloseHandle(static_cast<HANDLE>(m_recorder_init_event));
        m_recorder_init_event = nullptr;
    }

    load_settings();
    m_active_settings = settings();
    u32 target_fps = clamp<u32>(m_active_settings.fps, 10, 60);
    m_frame_timer.setInterval(max(16, static_cast<int>(1000 / target_fps)));

    auto dir = recordings_directory();
    auto timestamp_qstr = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    auto sanitized = sanitize_filename_part(page_title);
    String base_name;
    if (sanitized.is_empty())
        base_name = MUST(String::formatted("Ladybird_{}", ak_string_from_qstring(timestamp_qstr)));
    else
        base_name = MUST(String::formatted("Ladybird_{}_{}", ak_string_from_qstring(timestamp_qstr), sanitized));

    auto qbase_dir = QDir(qstring_from_ak_string(dir));
    m_session_id = base_name;
    m_completed_batches.clear();
    if (m_active_settings.batch_duration_seconds > 0) {
        QString session_folder = qbase_dir.filePath(qstring_from_ak_string(m_session_id));
        QDir().mkpath(session_folder);
        m_session_dir = ak_string_from_qstring(QDir::toNativeSeparators(session_folder));
        m_current_batch_index.store(1, AK::MemoryOrder::memory_order_release);
        m_recording_filename = batch_mp4_filename(1);
        m_mp4_path = batch_mp4_path(1);
        m_speakers_json_path = m_active_settings.export_speaker_timeline ? batch_speakers_json_path(1) : String {};
    } else {
        m_session_dir = {};
        m_current_batch_index.store(0, AK::MemoryOrder::memory_order_release);
        m_recording_filename = MUST(String::formatted("{}.mp4", base_name));
        m_mp4_path = ak_string_from_qstring(QDir::toNativeSeparators(qbase_dir.filePath(qstring_from_ak_string(m_recording_filename))));
        if (m_active_settings.export_speaker_timeline)
            m_speakers_json_path = ak_string_from_qstring(QDir::toNativeSeparators(qbase_dir.filePath(qstring_from_ak_string(MUST(String::formatted("{}.speakers.json", base_name))))));
        else
            m_speakers_json_path = {};
    }

    // Switch view to CPU backing stores so visible_snapshot reads frames cleanly without occlusion.
    m_view.set_recording_active(true);

    QImage initial_frame = m_view.holds_displayed_frame() ? m_view.visible_snapshot() : QImage {};
    u32 raw_width = initial_frame.isNull()
        ? static_cast<u32>(max(320, static_cast<int>(m_view.width() * m_view.device_pixel_ratio())))
        : static_cast<u32>(initial_frame.width());
    u32 raw_height = initial_frame.isNull()
        ? static_cast<u32>(max(240, static_cast<int>(m_view.height() * m_view.device_pixel_ratio())))
        : static_cast<u32>(initial_frame.height());

    // Cap to 1920x1080 and enforce even dimensions required by H.264 NV12 encoding.
    if (raw_width > 1920 || raw_height > 1080) {
        double scale = min(1920.0 / static_cast<double>(raw_width), 1080.0 / static_cast<double>(raw_height));
        raw_width = static_cast<u32>(static_cast<double>(raw_width) * scale);
        raw_height = static_cast<u32>(static_cast<double>(raw_height) * scale);
    }
    u32 target_width = max<u32>(320, raw_width & ~1u);
    u32 target_height = max<u32>(240, raw_height & ~1u);

    bool want_audio = m_active_settings.record_microphone || m_active_settings.record_system_audio;

    {
        AK::MutexLocker tl_locker(m_timeline_mutex);
        if (m_active_settings.export_speaker_timeline)
            m_timeline = make<Media::SpeakerTimeline>();
        else
            m_timeline.clear();
        m_timeline_batch_start_offset = AK::Duration::zero();
    }
    {
        AK::MutexLocker rec_locker(m_recorder_mutex);
        m_recorder_batch_start_offset = AK::Duration::zero();
    }
    m_stop_requested = false;
    m_snapshot_in_flight = false;
    m_recorder_init_error.clear();
    m_recorder_finalize_error.clear();
    m_recorder_has_audio = false;
    m_audio_worker_stop.store(false, AK::MemoryOrder::memory_order_release);
    m_worker_failed.store(false, AK::MemoryOrder::memory_order_release);
    {
        AK::MutexLocker locker(m_video_queue_mutex);
        m_video_queue.clear();
    }

    m_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_video_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    m_recorder_init_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stop_event || !m_video_event || !m_recorder_init_event) {
        if (m_stop_event) {
            CloseHandle(static_cast<HANDLE>(m_stop_event));
            m_stop_event = nullptr;
        }
        if (m_video_event) {
            CloseHandle(static_cast<HANDLE>(m_video_event));
            m_video_event = nullptr;
        }
        if (m_recorder_init_event) {
            CloseHandle(static_cast<HANDLE>(m_recorder_init_event));
            m_recorder_init_event = nullptr;
        }
        {
            AK::MutexLocker tl_locker(m_timeline_mutex);
            m_timeline.clear();
        }
        m_current_batch_index.store(0, AK::MemoryOrder::memory_order_release);
        m_view.set_recording_active(false);
        return Error::from_windows_error();
    }

    m_start_time = MonotonicTime::now_coarse();
    m_is_recording = true;

    m_video_thread = Threading::Thread::construct("TabRecorder Video"sv, [this, target_width, target_height, target_fps, want_audio]() -> intptr_t {
        run_video_worker(target_width, target_height, target_fps, want_audio);
        return 0;
    });
    m_video_thread->start();

    DWORD init_wait_result = WaitForSingleObject(static_cast<HANDLE>(m_recorder_init_event), 10'000);
    if (init_wait_result != WAIT_OBJECT_0) {
        {
            AK::MutexLocker locker(m_video_queue_mutex);
            m_stop_requested = true;
        }
        m_audio_worker_stop.store(true, AK::MemoryOrder::memory_order_release);
        if (m_stop_event)
            SetEvent(static_cast<HANDLE>(m_stop_event));
        if (m_video_event)
            SetEvent(static_cast<HANDLE>(m_video_event));
        m_is_recording = false;
        {
            AK::MutexLocker tl_locker(m_timeline_mutex);
            m_timeline.clear();
        }
        m_current_batch_index.store(0, AK::MemoryOrder::memory_order_release);
        m_view.set_recording_active(false);
        return Error::from_string_literal("Timed out initializing Media Foundation recorder");
    }

    CloseHandle(static_cast<HANDLE>(m_recorder_init_event));
    m_recorder_init_event = nullptr;

    if (m_recorder_init_error.has_value()) {
        m_is_recording = false;
        (void)m_video_thread->join();
        m_video_thread = nullptr;
        CloseHandle(static_cast<HANDLE>(m_stop_event));
        m_stop_event = nullptr;
        CloseHandle(static_cast<HANDLE>(m_video_event));
        m_video_event = nullptr;
        {
            AK::MutexLocker tl_locker(m_timeline_mutex);
            m_timeline.clear();
        }
        m_current_batch_index.store(0, AK::MemoryOrder::memory_order_release);
        m_view.set_recording_active(false);
        return m_recorder_init_error.release_value();
    }

    if (is_batched())
        (void)write_session_manifest(false, AK::Duration::zero());

    if (m_recorder_has_audio) {
        m_audio_thread = Threading::Thread::construct("TabRecorder Audio"sv, [this]() -> intptr_t {
            run_audio_worker();
            return 0;
        });
        m_audio_thread->start();
    }

    capture_video_tick();
    m_frame_timer.start();
    emit recording_started(qstring_from_ak_string(m_mp4_path));
    return {};
#endif
}

ErrorOr<TabRecorder::RecordingResult> TabRecorder::stop()
{
    if (!m_is_recording)
        return Error::from_string_literal("No active recording");

#ifndef Q_OS_WIN
    return Error::from_string_literal("Native tab recording is currently only supported on Windows");
#else
    m_frame_timer.stop();
    m_view.set_recording_active(false);
    m_is_recording = false;
    m_snapshot_in_flight = false;

    auto total_duration = max(AK::Duration::from_milliseconds(1), MonotonicTime::now_coarse() - m_start_time);

    // Stop and join the audio worker first so all audio samples finish before video finalization.
    m_audio_worker_stop.store(true, AK::MemoryOrder::memory_order_release);
    if (m_stop_event)
        SetEvent(static_cast<HANDLE>(m_stop_event));
    if (m_audio_thread) {
        (void)m_audio_thread->join();
        m_audio_thread = nullptr;
    }

    {
        AK::MutexLocker locker(m_video_queue_mutex);
        m_stop_requested = true;
    }
    if (m_video_event)
        SetEvent(static_cast<HANDLE>(m_video_event));
    if (m_video_thread) {
        (void)m_video_thread->join();
        m_video_thread = nullptr;
    }

    if (m_stop_event) {
        CloseHandle(static_cast<HANDLE>(m_stop_event));
        m_stop_event = nullptr;
    }
    if (m_video_event) {
        CloseHandle(static_cast<HANDLE>(m_video_event));
        m_video_event = nullptr;
    }

    u32 final_part = m_current_batch_index.load(AK::MemoryOrder::memory_order_acquire);
    m_current_batch_index.store(0, AK::MemoryOrder::memory_order_release);

    OwnPtr<Media::SpeakerTimeline> final_timeline;
    AK::Duration final_session_start = AK::Duration::zero();
    {
        AK::MutexLocker tl_locker(m_timeline_mutex);
        final_timeline = move(m_timeline);
        final_session_start = m_timeline_batch_start_offset;
    }

    auto final_batch_duration = max(AK::Duration::from_milliseconds(1), total_duration - final_session_start);
    ErrorOr<void> json_result = {};
    String final_mp4_path = is_batched() && final_part > 0 ? batch_mp4_path(final_part) : m_mp4_path;
    String final_mp4_filename = is_batched() && final_part > 0 ? batch_mp4_filename(final_part) : m_recording_filename;
    String final_json_path = is_batched() && final_part > 0
        ? (m_active_settings.export_speaker_timeline ? batch_speakers_json_path(final_part) : String {})
        : m_speakers_json_path;

    if (final_timeline) {
        final_timeline->finalize(final_batch_duration);
        json_result = write_speakers_json_file(*final_timeline, final_mp4_filename, final_json_path, final_batch_duration);
        final_timeline.clear();
    }

    if (is_batched() && final_part > 0 && !m_recorder_finalize_error.has_value()) {
        m_completed_batches.append(CompletedBatch {
            .part_index = final_part,
            .mp4_filename = final_mp4_filename,
            .speakers_json_filename = !json_result.is_error() && !final_json_path.is_empty() ? batch_speakers_json_filename(final_part) : String {},
            .session_start = final_session_start,
            .duration = final_batch_duration,
        });
        (void)write_session_manifest(true, total_duration);
    }

    if (m_recorder_finalize_error.has_value()) {
        auto err = m_recorder_finalize_error.release_value();
        auto err_msg = qstring_from_ak_string(MUST(String::formatted("{}", err)));
        emit recording_failed(err_msg);
        return err;
    }
    if (json_result.is_error()) {
        auto err_msg = qstring_from_ak_string(MUST(String::formatted("{}", json_result.error())));
        emit recording_failed(err_msg);
        return json_result.release_error();
    }

    RecordingResult result {
        .mp4_path = final_mp4_path,
        .speakers_json_path = final_json_path,
        .duration = total_duration,
    };
    emit recording_stopped(qstring_from_ak_string(result.mp4_path), qstring_from_ak_string(result.speakers_json_path));
    return result;
#endif
}

void TabRecorder::notify_webrtc_speaker_activity(u64 speaker_key, bool is_local, bool is_speaking, float level)
{
    if (!m_is_recording)
        return;
    AK::MutexLocker tl_locker(m_timeline_mutex);
    if (!m_timeline)
        return;
    auto session_ts = max(AK::Duration::zero(), MonotonicTime::now_coarse() - m_start_time);
    auto batch_ts = max(AK::Duration::zero(), session_ts - m_timeline_batch_start_offset);
    m_timeline->record_webrtc_activity(speaker_key, is_local, is_speaking, level, batch_ts);
}

void TabRecorder::capture_video_tick()
{
#ifndef Q_OS_WIN
    return;
#else
    if (!m_is_recording)
        return;

    if (m_view.holds_displayed_frame()) {
        enqueue_video_frame(m_view.visible_snapshot());
        return;
    }

    if (m_snapshot_in_flight)
        return;
    m_snapshot_in_flight = true;
    m_view.take_visible_snapshot([self = QPointer { this }](QImage snapshot) {
        if (!self)
            return;
        self->m_snapshot_in_flight = false;
        if (self->m_is_recording)
            self->enqueue_video_frame(move(snapshot));
    });
#endif
}

void TabRecorder::enqueue_video_frame(QImage snapshot)
{
#ifndef Q_OS_WIN
    (void)snapshot;
    return;
#else
    if (!m_is_recording || snapshot.isNull() || snapshot.width() <= 0 || snapshot.height() <= 0)
        return;

    if (snapshot.format() != QImage::Format_RGB32 && snapshot.format() != QImage::Format_ARGB32 && snapshot.format() != QImage::Format_ARGB32_Premultiplied)
        snapshot = snapshot.convertToFormat(QImage::Format_RGB32);

    if (m_active_settings.show_cursor && m_view.width() > 0 && m_view.height() > 0) {
        QPoint local_pos = m_view.mapFromGlobal(QCursor::pos());
        if (m_view.rect().contains(local_pos)) {
            qreal cx = (static_cast<qreal>(local_pos.x()) * snapshot.width()) / static_cast<qreal>(m_view.width());
            qreal cy = (static_cast<qreal>(local_pos.y()) * snapshot.height()) / static_cast<qreal>(m_view.height());

            QPainter painter(&snapshot);
            painter.setRenderHint(QPainter::Antialiasing, true);

            if (QApplication::mouseButtons() != Qt::NoButton) {
                painter.setPen(QPen(QColor(229, 72, 77, 220), 2.0));
                painter.setBrush(QColor(229, 72, 77, 110));
                painter.drawEllipse(QPointF(cx, cy), 14.0, 14.0);
            }

            QPolygonF arrow;
            arrow << QPointF(cx, cy)
                  << QPointF(cx, cy + 19.0)
                  << QPointF(cx + 4.5, cy + 14.5)
                  << QPointF(cx + 8.0, cy + 22.0)
                  << QPointF(cx + 10.5, cy + 20.5)
                  << QPointF(cx + 7.0, cy + 13.5)
                  << QPointF(cx + 13.5, cy + 13.5);
            painter.setPen(QPen(QColor(24, 24, 28), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::white);
            painter.drawPolygon(arrow);
        }
    }

    auto timestamp = max(AK::Duration::zero(), MonotonicTime::now_coarse() - m_start_time);
    {
        AK::MutexLocker locker(m_video_queue_mutex);
        constexpr size_t MAX_QUEUED_FRAMES = 4;
        if (m_video_queue.size() >= MAX_QUEUED_FRAMES)
            m_video_queue.take_first();
        m_video_queue.append(QueuedVideoFrame {
            .image = move(snapshot),
            .timestamp = timestamp,
        });
    }
    if (m_video_event)
        SetEvent(static_cast<HANDLE>(m_video_event));
#endif
}

void TabRecorder::run_video_worker([[maybe_unused]] u32 width, [[maybe_unused]] u32 height, [[maybe_unused]] u32 fps, [[maybe_unused]] bool record_audio)
{
#ifdef Q_OS_WIN
    ScopedComInitializer com(COINIT_MULTITHREADED);
    if (!com.initialized()) {
        m_recorder_init_error = Error::from_string_literal("Failed to initialize COM MTA on recorder thread");
        if (m_recorder_init_event)
            SetEvent(static_cast<HANDLE>(m_recorder_init_event));
        return;
    }

    Media::WindowsMediaFoundation::Mp4RecorderConfig config;
    config.width = width;
    config.height = height;
    config.fps = fps;
    config.video_bitrate_bps = 4'000'000;
    config.record_audio = record_audio;
    config.audio_sample_rate = TARGET_AUDIO_SAMPLE_RATE;
    config.audio_channels = TARGET_AUDIO_CHANNELS;
    config.audio_bitrate_bps = 192'000;

    auto recorder_or_error = Media::WindowsMediaFoundation::WindowsMediaFoundationMp4Recorder::create(m_mp4_path, config);
    if (recorder_or_error.is_error()) {
        m_recorder_init_error = recorder_or_error.release_error();
        if (m_recorder_init_event)
            SetEvent(static_cast<HANDLE>(m_recorder_init_event));
        return;
    }

    {
        AK::MutexLocker rec_locker(m_recorder_mutex);
        m_recorder = recorder_or_error.release_value();
        m_recorder_has_audio = m_recorder->has_audio_stream();
    }
    if (m_recorder_init_event)
        SetEvent(static_cast<HANDLE>(m_recorder_init_event));

    {
        AK::MutexLocker locker(m_video_queue_mutex);
        if (m_stop_requested) {
            AK::MutexLocker rec_locker(m_recorder_mutex);
            m_recorder.clear();
            return;
        }
    }

    AK::Duration last_written_ts = AK::Duration::from_milliseconds(-1);
    u32 safe_fps = clamp<u32>(fps, 10, 60);
    auto frame_duration = AK::Duration::from_milliseconds(max<i64>(16, 1000 / safe_fps));
    u32 active_part = m_active_settings.batch_duration_seconds > 0 ? 1 : 0;
    AK::Duration batch_start_offset = AK::Duration::zero();
    auto batch_limit = AK::Duration::from_seconds(static_cast<i64>(m_active_settings.batch_duration_seconds));
    size_t consecutive_write_errors = 0;
    constexpr size_t max_consecutive_write_errors = 10;

    for (;;) {
        Optional<QueuedVideoFrame> next_frame;
        bool should_exit = false;
        {
            AK::MutexLocker locker(m_video_queue_mutex);
            if (!m_video_queue.is_empty()) {
                next_frame = m_video_queue.take_first();
            } else if (m_stop_requested) {
                should_exit = true;
            }
        }

        if (should_exit)
            break;

        if (!next_frame.has_value()) {
            WaitForSingleObject(static_cast<HANDLE>(m_video_event), 25);
            continue;
        }

        auto frame_session_ts = next_frame->timestamp;
        if (active_part > 0 && batch_limit > AK::Duration::zero() && (frame_session_ts - batch_start_offset) >= batch_limit) {
            auto split_offset = frame_session_ts;
            auto finished_part = active_part;
            auto next_part = active_part + 1;

            // Open the next batch recorder before swapping m_recorder so the audio worker never drops chunks.
            auto next_recorder_or_err = Media::WindowsMediaFoundation::WindowsMediaFoundationMp4Recorder::create(batch_mp4_path(next_part), config);

            OwnPtr<Media::WindowsMediaFoundation::WindowsMediaFoundationMp4Recorder> outgoing_recorder;
            {
                AK::MutexLocker rec_locker(m_recorder_mutex);
                outgoing_recorder = move(m_recorder);
                if (!next_recorder_or_err.is_error()) {
                    m_recorder = next_recorder_or_err.release_value();
                    m_recorder_batch_start_offset = split_offset;
                }
            }

            if (outgoing_recorder) {
                if (auto rc = outgoing_recorder->finalize(); rc.is_error()) {
                    outgoing_recorder.clear();
                    handle_worker_failure(rc.release_error());
                    break;
                }
                outgoing_recorder.clear();
            }

            if (next_recorder_or_err.is_error()) {
                handle_worker_failure(next_recorder_or_err.release_error());
                break;
            }

            OwnPtr<Media::SpeakerTimeline> outgoing_timeline;
            {
                AK::MutexLocker tl_locker(m_timeline_mutex);
                if (m_timeline) {
                    outgoing_timeline = move(m_timeline);
                    m_timeline = outgoing_timeline->create_continuation();
                }
                m_timeline_batch_start_offset = split_offset;
            }

            auto batch_duration = max(AK::Duration::from_milliseconds(1), split_offset - batch_start_offset);
            String completed_json_name;
            if (outgoing_timeline) {
                outgoing_timeline->finalize(batch_duration);
                auto json_filename = batch_speakers_json_filename(finished_part);
                auto json_path = batch_speakers_json_path(finished_part);
                if (auto rc = write_speakers_json_file(*outgoing_timeline, batch_mp4_filename(finished_part), json_path, batch_duration); rc.is_error())
                    dbgln("TabRecorder: write batch speakers json failed: {}", rc.error());
                else
                    completed_json_name = move(json_filename);
            }

            m_completed_batches.append(CompletedBatch {
                .part_index = finished_part,
                .mp4_filename = batch_mp4_filename(finished_part),
                .speakers_json_filename = move(completed_json_name),
                .session_start = batch_start_offset,
                .duration = batch_duration,
            });
            if (auto rc = write_session_manifest(false, split_offset); rc.is_error())
                dbgln("TabRecorder: write_session_manifest failed: {}", rc.error());

            active_part = next_part;
            batch_start_offset = split_offset;
            last_written_ts = AK::Duration::from_milliseconds(-1);
            m_current_batch_index.store(active_part, AK::MemoryOrder::memory_order_release);
        }

        auto ts = max(AK::Duration::zero(), frame_session_ts - batch_start_offset);
        if (last_written_ts >= AK::Duration::zero() && ts <= last_written_ts)
            ts = last_written_ts + AK::Duration::from_milliseconds(1);
        last_written_ts = ts;

        auto const& img = next_frame->image;
        ReadonlyBytes pixels { img.constBits(), static_cast<size_t>(img.bytesPerLine()) * static_cast<size_t>(img.height()) };

        Optional<Error> write_error;
        {
            AK::MutexLocker rec_locker(m_recorder_mutex);
            if (m_recorder) {
                if (auto rc = m_recorder->write_video_frame(pixels, static_cast<u32>(img.width()), static_cast<u32>(img.height()), static_cast<size_t>(img.bytesPerLine()), false, ts, frame_duration); rc.is_error())
                    write_error = rc.release_error();
            }
        }
        if (write_error.has_value()) {
            dbgln("TabRecorder: write_video_frame failed: {}", *write_error);
            if (++consecutive_write_errors >= max_consecutive_write_errors) {
                handle_worker_failure(write_error.release_value());
                break;
            }
        } else {
            consecutive_write_errors = 0;
        }
    }

    {
        AK::MutexLocker rec_locker(m_recorder_mutex);
        if (m_recorder) {
            if (auto rc = m_recorder->finalize(); rc.is_error())
                m_recorder_finalize_error = rc.release_error();
            m_recorder.clear();
        }
    }
#endif
}

void TabRecorder::run_audio_worker()
{
#ifdef Q_OS_WIN
    ScopedComInitializer com(COINIT_MULTITHREADED);

    WasapiCaptureSource loopback;
    WasapiCaptureSource mic;
    if (m_active_settings.record_system_audio)
        (void)open_wasapi_source(loopback, true);
    if (m_active_settings.record_microphone)
        (void)open_wasapi_source(mic, false, m_active_settings.microphone_device_id);

    Vector<float> loopback_chunk;
    Vector<float> mic_chunk;
    Vector<i16> mixed_pcm;
    loopback_chunk.resize(AUDIO_CHUNK_FRAMES * TARGET_AUDIO_CHANNELS);
    mic_chunk.resize(AUDIO_CHUNK_FRAMES * TARGET_AUDIO_CHANNELS);
    mixed_pcm.resize(AUDIO_CHUNK_FRAMES * TARGET_AUDIO_CHANNELS);

    bool loopback_speaking = false;
    bool mic_speaking = false;
    Optional<MonotonicTime> loopback_last_speech;
    Optional<MonotonicTime> mic_last_speech;

    constexpr auto chunk_duration = AK::Duration::from_milliseconds(20);
    u64 chunk_index = 0;

    while (!m_audio_worker_stop.load(AK::MemoryOrder::memory_order_acquire)) {
        auto next_deadline = m_start_time + AK::Duration::from_milliseconds(static_cast<i64>((chunk_index + 1) * 20));
        auto now = MonotonicTime::now_coarse();
        if (next_deadline > now) {
            auto remaining_us = (next_deadline - now).to_microseconds();
            DWORD wait_ms = static_cast<DWORD>(clamp<i64>((remaining_us + 999) / 1000, 1, 20));
            WaitForSingleObject(static_cast<HANDLE>(m_stop_event), wait_ms);
            if (m_audio_worker_stop.load(AK::MemoryOrder::memory_order_acquire))
                break;
            continue;
        }

        float loopback_rms = 0.0f;
        float mic_rms = 0.0f;
        if (loopback.initialized) {
            drain_wasapi_source(loopback);
            loopback_rms = pop_chunk_and_compute_rms(loopback, loopback_chunk.span());
        }
        if (mic.initialized) {
            drain_wasapi_source(mic);
            mic_rms = pop_chunk_and_compute_rms(mic, mic_chunk.span());
        }

        auto chunk_ts = AK::Duration::from_milliseconds(static_cast<i64>(chunk_index * 20));
        auto vad_now = MonotonicTime::now_coarse();

        if (loopback.initialized) {
            if (loopback_rms >= FALLBACK_VAD_RMS_THRESHOLD)
                loopback_last_speech = vad_now;
            bool next_loopback_speaking = loopback_rms >= FALLBACK_VAD_RMS_THRESHOLD
                || (loopback_last_speech.has_value() && (vad_now - *loopback_last_speech) <= FALLBACK_VAD_HOLDOVER);
            if (next_loopback_speaking != loopback_speaking) {
                loopback_speaking = next_loopback_speaking;
                AK::MutexLocker tl_locker(m_timeline_mutex);
                if (m_timeline) {
                    auto batch_ts = max(AK::Duration::zero(), chunk_ts - m_timeline_batch_start_offset);
                    m_timeline->record_fallback_audio_activity(false, loopback_speaking, loopback_rms, batch_ts);
                }
            }
        }

        if (mic.initialized) {
            if (mic_rms >= FALLBACK_VAD_RMS_THRESHOLD)
                mic_last_speech = vad_now;
            bool next_mic_speaking = mic_rms >= FALLBACK_VAD_RMS_THRESHOLD
                || (mic_last_speech.has_value() && (vad_now - *mic_last_speech) <= FALLBACK_VAD_HOLDOVER);
            if (next_mic_speaking != mic_speaking) {
                mic_speaking = next_mic_speaking;
                AK::MutexLocker tl_locker(m_timeline_mutex);
                if (m_timeline) {
                    auto batch_ts = max(AK::Duration::zero(), chunk_ts - m_timeline_batch_start_offset);
                    m_timeline->record_fallback_audio_activity(true, mic_speaking, mic_rms, batch_ts);
                }
            }
        }

        for (size_t i = 0; i < mixed_pcm.size(); ++i) {
            float sum = clamp(loopback_chunk[i] + mic_chunk[i], -1.0f, 1.0f);
            mixed_pcm[i] = static_cast<i16>(sum * 32767.0f);
        }

        ReadonlyBytes pcm_bytes { reinterpret_cast<u8 const*>(mixed_pcm.data()), mixed_pcm.size() * sizeof(i16) };
        {
            AK::MutexLocker rec_locker(m_recorder_mutex);
            if (m_recorder) {
                auto audio_batch_ts = max(AK::Duration::zero(), chunk_ts - m_recorder_batch_start_offset);
                if (auto rc = m_recorder->write_audio_pcm_s16le(pcm_bytes, audio_batch_ts, chunk_duration); rc.is_error()) {
                    dbgln("TabRecorder: write_audio_pcm_s16le failed: {}", rc.error());
                }
            }
        }

        ++chunk_index;
    }
#endif
}

String TabRecorder::batch_mp4_filename(u32 part_index) const
{
    return MUST(String::formatted("part_{:03}.mp4", part_index));
}

String TabRecorder::batch_mp4_path(u32 part_index) const
{
    auto filename = batch_mp4_filename(part_index);
    return ak_string_from_qstring(QDir::toNativeSeparators(QDir(qstring_from_ak_string(m_session_dir)).filePath(qstring_from_ak_string(filename))));
}

String TabRecorder::batch_speakers_json_filename(u32 part_index) const
{
    return MUST(String::formatted("part_{:03}.speakers.json", part_index));
}

String TabRecorder::batch_speakers_json_path(u32 part_index) const
{
    auto filename = batch_speakers_json_filename(part_index);
    return ak_string_from_qstring(QDir::toNativeSeparators(QDir(qstring_from_ak_string(m_session_dir)).filePath(qstring_from_ak_string(filename))));
}

ErrorOr<void> TabRecorder::write_speakers_json_file(Media::SpeakerTimeline& timeline, StringView mp4_filename, StringView json_path, AK::Duration batch_duration)
{
    if (json_path.is_empty())
        return {};

    auto json = timeline.to_json(mp4_filename, batch_duration);
    auto file = TRY(Core::File::open(json_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(file->write_until_depleted(json.bytes()));
    return {};
}

ErrorOr<void> TabRecorder::write_session_manifest(bool completed, AK::Duration total_duration)
{
    if (m_session_dir.is_empty())
        return {};

    JsonObject root;
    root.set("session_id"sv, m_session_id);
    root.set("completed"sv, completed);
    root.set("batch_duration_seconds"sv, m_active_settings.batch_duration_seconds);
    root.set("total_duration_sec"sv, static_cast<double>(max<i64>(0, total_duration.to_milliseconds())) / 1000.0);

    JsonArray batches_array;
    StringBuilder concat_builder;
    for (auto const& batch : m_completed_batches) {
        JsonObject entry;
        entry.set("part_index"sv, batch.part_index);
        entry.set("mp4_file"sv, batch.mp4_filename);
        if (!batch.speakers_json_filename.is_empty())
            entry.set("speakers_json_file"sv, batch.speakers_json_filename);
        entry.set("start_sec"sv, static_cast<double>(max<i64>(0, batch.session_start.to_milliseconds())) / 1000.0);
        entry.set("start_time"sv, Media::SpeakerTimeline::format_timestamp_hms(batch.session_start));
        entry.set("duration_sec"sv, static_cast<double>(max<i64>(0, batch.duration.to_milliseconds())) / 1000.0);
        batches_array.must_append(move(entry));

        concat_builder.appendff("file '{}'\n", batch.mp4_filename);
    }
    root.set("batches"sv, move(batches_array));

    auto manifest_json = root.serialized();
    auto qdir = QDir(qstring_from_ak_string(m_session_dir));
    auto manifest_path = ak_string_from_qstring(QDir::toNativeSeparators(qdir.filePath("session.json")));
    auto manifest_file = TRY(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(manifest_file->write_until_depleted(manifest_json.bytes()));

    auto concat_path = ak_string_from_qstring(QDir::toNativeSeparators(qdir.filePath("concat.txt")));
    auto concat_file = TRY(Core::File::open(concat_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(concat_file->write_until_depleted(concat_builder.string_view().bytes()));

    return {};
}

}
