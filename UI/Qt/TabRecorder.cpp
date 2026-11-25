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
#include <AK/Math.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibMediaCodecs/SpeakerTimeline.h>
#include <LibThreading/Thread.h>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

#include <cstring>

#ifdef Q_OS_WIN
#    include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationMp4Recorder.h>
#    ifndef WIN32_LEAN_AND_MEAN
#        define WIN32_LEAN_AND_MEAN
#    endif
#    include <audioclient.h>
#    include <mmdeviceapi.h>
#    include <windows.h>
#    include <wrl/client.h>
#endif

namespace Ladybird {

namespace {

constexpr u32 TARGET_FPS = 20;
constexpr int FRAME_INTERVAL_MS = 1000 / TARGET_FPS;
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

#ifdef Q_OS_WIN

using Microsoft::WRL::ComPtr;

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

bool open_wasapi_source(WasapiCaptureSource& out, bool loopback)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr) || !enumerator)
        return false;

    ComPtr<IMMDevice> device;
    hr = enumerator->GetDefaultAudioEndpoint(loopback ? eRender : eCapture, eConsole, &device);
    if (FAILED(hr) || !device)
        return false;

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
    m_frame_timer.setInterval(FRAME_INTERVAL_MS);
    QObject::connect(&m_frame_timer, &QTimer::timeout, this, [this] { capture_video_tick(); });
}

TabRecorder::~TabRecorder()
{
    if (m_is_recording)
        (void)stop();
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
    auto dir = recordings_directory();
    auto timestamp_qstr = QDateTime::currentDateTime().toString("yyyy-MM-dd_HH-mm-ss");
    auto sanitized = sanitize_filename_part(page_title);
    String base_name;
    if (sanitized.is_empty())
        base_name = MUST(String::formatted("Ladybird_{}", ak_string_from_qstring(timestamp_qstr)));
    else
        base_name = MUST(String::formatted("Ladybird_{}_{}", ak_string_from_qstring(timestamp_qstr), sanitized));

    auto qdir = QDir(qstring_from_ak_string(dir));
    m_recording_filename = MUST(String::formatted("{}.mp4", base_name));
    m_mp4_path = ak_string_from_qstring(QDir::toNativeSeparators(qdir.filePath(qstring_from_ak_string(m_recording_filename))));
    m_speakers_json_path = ak_string_from_qstring(QDir::toNativeSeparators(qdir.filePath(qstring_from_ak_string(MUST(String::formatted("{}.speakers.json", base_name))))));

    // Switch view to CPU backing stores so visible_snapshot reads frames cleanly without occlusion.
    m_view.set_recording_active(true);

    QImage initial_frame = m_view.visible_snapshot();
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

    Media::WindowsMediaFoundation::Mp4RecorderConfig config;
    config.width = target_width;
    config.height = target_height;
    config.fps = TARGET_FPS;
    config.video_bitrate_bps = 4'000'000;
    config.record_audio = true;
    config.audio_sample_rate = TARGET_AUDIO_SAMPLE_RATE;
    config.audio_channels = TARGET_AUDIO_CHANNELS;
    config.audio_bitrate_bps = 192'000;

    auto recorder_or_error = Media::WindowsMediaFoundation::WindowsMediaFoundationMp4Recorder::create(m_mp4_path, config);
    if (recorder_or_error.is_error()) {
        m_view.set_recording_active(false);
        return recorder_or_error.release_error();
    }

    m_recorder = recorder_or_error.release_value();
    m_timeline = make<Media::SpeakerTimeline>();
    m_stop_requested = false;
    m_audio_worker_stop.store(false, AK::MemoryOrder::memory_order_release);
    {
        AK::MutexLocker locker(m_video_queue_mutex);
        m_video_queue.clear();
    }

    m_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    m_video_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_stop_event || !m_video_event) {
        if (m_stop_event) {
            CloseHandle(static_cast<HANDLE>(m_stop_event));
            m_stop_event = nullptr;
        }
        if (m_video_event) {
            CloseHandle(static_cast<HANDLE>(m_video_event));
            m_video_event = nullptr;
        }
        m_recorder.clear();
        m_timeline.clear();
        m_view.set_recording_active(false);
        return Error::from_windows_error();
    }

    m_start_time = MonotonicTime::now_coarse();
    m_is_recording = true;

    m_video_thread = Threading::Thread::construct("TabRecorder Video"sv, [this]() -> intptr_t {
        run_video_worker();
        return 0;
    });
    m_video_thread->start();

    if (m_recorder->has_audio_stream()) {
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

    auto total_duration = max(AK::Duration::from_milliseconds(1), MonotonicTime::now_coarse() - m_start_time);

    {
        AK::MutexLocker locker(m_video_queue_mutex);
        m_stop_requested = true;
    }
    m_audio_worker_stop.store(true, AK::MemoryOrder::memory_order_release);
    if (m_stop_event)
        SetEvent(static_cast<HANDLE>(m_stop_event));
    if (m_video_event)
        SetEvent(static_cast<HANDLE>(m_video_event));

    if (m_video_thread) {
        (void)m_video_thread->join();
        m_video_thread = nullptr;
    }
    if (m_audio_thread) {
        (void)m_audio_thread->join();
        m_audio_thread = nullptr;
    }

    if (m_stop_event) {
        CloseHandle(static_cast<HANDLE>(m_stop_event));
        m_stop_event = nullptr;
    }
    if (m_video_event) {
        CloseHandle(static_cast<HANDLE>(m_video_event));
        m_video_event = nullptr;
    }

    ErrorOr<void> finalize_result = {};
    {
        AK::MutexLocker locker(m_recorder_mutex);
        if (m_recorder) {
            finalize_result = m_recorder->finalize();
            m_recorder.clear();
        }
    }

    if (m_timeline)
        m_timeline->finalize(total_duration);
    auto json_result = write_speakers_json(total_duration);
    m_timeline.clear();

    if (finalize_result.is_error()) {
        auto err_msg = qstring_from_ak_string(finalize_result.error().string_literal());
        emit recording_failed(err_msg);
        return finalize_result.release_error();
    }
    if (json_result.is_error()) {
        auto err_msg = qstring_from_ak_string(json_result.error().string_literal());
        emit recording_failed(err_msg);
        return json_result.release_error();
    }

    RecordingResult result {
        .mp4_path = m_mp4_path,
        .speakers_json_path = m_speakers_json_path,
        .duration = total_duration,
    };
    emit recording_stopped(qstring_from_ak_string(result.mp4_path), qstring_from_ak_string(result.speakers_json_path));
    return result;
#endif
}

void TabRecorder::notify_webrtc_speaker_activity(u64 speaker_key, bool is_local, bool is_speaking, float level)
{
    if (!m_is_recording || !m_timeline)
        return;
    auto timestamp = max(AK::Duration::zero(), MonotonicTime::now_coarse() - m_start_time);
    m_timeline->record_webrtc_activity(speaker_key, is_local, is_speaking, level, timestamp);
}

void TabRecorder::capture_video_tick()
{
#ifndef Q_OS_WIN
    return;
#else
    if (!m_is_recording)
        return;

    QImage snapshot = m_view.visible_snapshot();
    if (snapshot.isNull())
        return;

    if (snapshot.format() != QImage::Format_RGB32 && snapshot.format() != QImage::Format_ARGB32 && snapshot.format() != QImage::Format_ARGB32_Premultiplied)
        snapshot = snapshot.convertToFormat(QImage::Format_RGB32);

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

void TabRecorder::run_video_worker()
{
#ifdef Q_OS_WIN
    ScopedComInitializer com(COINIT_MULTITHREADED);
    AK::Duration last_written_ts = AK::Duration::from_milliseconds(-1);
    constexpr auto frame_duration = AK::Duration::from_milliseconds(FRAME_INTERVAL_MS);

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

        auto ts = next_frame->timestamp;
        if (last_written_ts >= AK::Duration::zero() && ts <= last_written_ts)
            ts = last_written_ts + AK::Duration::from_milliseconds(1);
        last_written_ts = ts;

        auto const& img = next_frame->image;
        ReadonlyBytes pixels { img.constBits(), static_cast<size_t>(img.bytesPerLine()) * static_cast<size_t>(img.height()) };

        AK::MutexLocker rec_locker(m_recorder_mutex);
        if (m_recorder) {
            if (auto rc = m_recorder->write_video_frame(pixels, static_cast<u32>(img.width()), static_cast<u32>(img.height()), static_cast<size_t>(img.bytesPerLine()), false, ts, frame_duration); rc.is_error()) {
                dbgln("TabRecorder: write_video_frame failed: {}", rc.error());
            }
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
    (void)open_wasapi_source(loopback, true);
    (void)open_wasapi_source(mic, false);

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
            DWORD wait_ms = static_cast<DWORD>(min<i64>(20, (next_deadline - now).to_milliseconds()));
            if (wait_ms > 0)
                WaitForSingleObject(static_cast<HANDLE>(m_stop_event), wait_ms);
            if (m_audio_worker_stop.load(AK::MemoryOrder::memory_order_acquire))
                break;
            continue;
        }

        drain_wasapi_source(loopback);
        drain_wasapi_source(mic);

        float loopback_rms = pop_chunk_and_compute_rms(loopback, loopback_chunk.span());
        float mic_rms = pop_chunk_and_compute_rms(mic, mic_chunk.span());

        auto chunk_ts = AK::Duration::from_milliseconds(static_cast<i64>(chunk_index * 20));
        auto vad_now = MonotonicTime::now_coarse();

        if (loopback_rms >= FALLBACK_VAD_RMS_THRESHOLD)
            loopback_last_speech = vad_now;
        bool next_loopback_speaking = loopback_rms >= FALLBACK_VAD_RMS_THRESHOLD
            || (loopback_last_speech.has_value() && (vad_now - *loopback_last_speech) <= FALLBACK_VAD_HOLDOVER);
        if (next_loopback_speaking != loopback_speaking && m_timeline) {
            loopback_speaking = next_loopback_speaking;
            m_timeline->record_fallback_audio_activity(false, loopback_speaking, loopback_rms, chunk_ts);
        }

        if (mic_rms >= FALLBACK_VAD_RMS_THRESHOLD)
            mic_last_speech = vad_now;
        bool next_mic_speaking = mic_rms >= FALLBACK_VAD_RMS_THRESHOLD
            || (mic_last_speech.has_value() && (vad_now - *mic_last_speech) <= FALLBACK_VAD_HOLDOVER);
        if (next_mic_speaking != mic_speaking && m_timeline) {
            mic_speaking = next_mic_speaking;
            m_timeline->record_fallback_audio_activity(true, mic_speaking, mic_rms, chunk_ts);
        }

        for (size_t i = 0; i < mixed_pcm.size(); ++i) {
            float sum = clamp(loopback_chunk[i] + mic_chunk[i], -1.0f, 1.0f);
            mixed_pcm[i] = static_cast<i16>(sum * 32767.0f);
        }

        ReadonlyBytes pcm_bytes { reinterpret_cast<u8 const*>(mixed_pcm.data()), mixed_pcm.size() * sizeof(i16) };
        {
            AK::MutexLocker rec_locker(m_recorder_mutex);
            if (m_recorder) {
                if (auto rc = m_recorder->write_audio_pcm_s16le(pcm_bytes, chunk_ts, chunk_duration); rc.is_error()) {
                    dbgln("TabRecorder: write_audio_pcm_s16le failed: {}", rc.error());
                }
            }
        }

        ++chunk_index;
    }
#endif
}

ErrorOr<void> TabRecorder::write_speakers_json(AK::Duration total_duration)
{
    if (!m_timeline)
        return {};

    auto json = m_timeline->to_json(m_recording_filename, total_duration);
    auto file = TRY(Core::File::open(m_speakers_json_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(file->write_until_depleted(json.bytes()));
    return {};
}

}
