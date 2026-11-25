/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Math.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <AudioServer/PlatformAudio.h>
#include <LibAudio/ChannelMap.h>
#include <LibAudio/RecordStream.h>
#include <LibAudio/SampleSpecification.h>
#include <LibCore/EventLoop.h>
#include <LibThreading/Thread.h>
#include <string.h>

#include <AK/Windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

namespace Audio {

using Microsoft::WRL::ComPtr;

namespace {

class ScopedComInitializer {
    AK_MAKE_NONCOPYABLE(ScopedComInitializer);
    AK_MAKE_NONMOVABLE(ScopedComInitializer);

public:
    ScopedComInitializer()
    {
        m_hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }

    ~ScopedComInitializer()
    {
        if (m_hr == S_OK || m_hr == S_FALSE)
            CoUninitialize();
    }

    [[nodiscard]] bool initialized() const
    {
        return SUCCEEDED(m_hr) || m_hr == RPC_E_CHANGED_MODE;
    }

    [[nodiscard]] HRESULT hresult() const { return m_hr; }

private:
    HRESULT m_hr { E_FAIL };
};

enum class NativeSampleEncoding {
    Float32,
    Int16,
    Int24,
    Int32,
};

Optional<NativeSampleEncoding> detect_native_sample_encoding(WAVEFORMATEX const* format)
{
    if (!format)
        return {};

    u16 const bits = format->wBitsPerSample;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        if (bits == 32)
            return NativeSampleEncoding::Float32;
        return {};
    }

    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        if (bits == 16)
            return NativeSampleEncoding::Int16;
        if (bits == 24)
            return NativeSampleEncoding::Int24;
        if (bits == 32)
            return NativeSampleEncoding::Int32;
        return {};
    }

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE
        && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        auto const* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE const*>(format);
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            if (bits == 32)
                return NativeSampleEncoding::Float32;
            return {};
        }
        if (IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            if (bits == 16)
                return NativeSampleEncoding::Int16;
            if (bits == 24)
                return NativeSampleEncoding::Int24;
            if (bits == 32)
                return NativeSampleEncoding::Int32;
            return {};
        }
    }

    return {};
}

Vector<wchar_t> utf8_to_wide(StringView value)
{
    if (value.is_empty())
        return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, value.characters_without_null_termination(), static_cast<int>(value.length()), nullptr, 0);
    if (length <= 0)
        return {};
    Vector<wchar_t> result;
    result.resize(static_cast<size_t>(length) + 1);
    int written = MultiByteToWideChar(CP_UTF8, 0, value.characters_without_null_termination(), static_cast<int>(value.length()), result.data(), length);
    if (written <= 0)
        return {};
    result[static_cast<size_t>(written)] = L'\0';
    return result;
}

float read_native_sample_as_float(u8 const* frame_ptr, u32 channel_index, u32 bytes_per_sample, NativeSampleEncoding encoding)
{
    u8 const* sample_ptr = frame_ptr + static_cast<size_t>(channel_index) * bytes_per_sample;
    switch (encoding) {
    case NativeSampleEncoding::Float32: {
        float value = 0.0f;
        memcpy(&value, sample_ptr, sizeof(value));
        return value;
    }
    case NativeSampleEncoding::Int16: {
        i16 value = 0;
        memcpy(&value, sample_ptr, sizeof(value));
        return static_cast<float>(value) / 32768.0f;
    }
    case NativeSampleEncoding::Int24: {
        i32 raw = static_cast<i32>(sample_ptr[0] | (sample_ptr[1] << 8) | (sample_ptr[2] << 16));
        if (raw & 0x800000)
            raw |= ~0xFFFFFF;
        return static_cast<float>(raw) / 8388608.0f;
    }
    case NativeSampleEncoding::Int32: {
        i32 value = 0;
        memcpy(&value, sample_ptr, sizeof(value));
        return static_cast<float>(static_cast<double>(value) / 2147483648.0);
    }
    }
    return 0.0f;
}

}

class RecordStreamWasapi final : public Audio::RecordStream {
public:
    struct StreamState final : public AtomicRefCounted<StreamState> {
        Atomic<bool> alive { true };
        RecordStreamWasapi* stream { nullptr };
    };

    struct InitResult final : public AtomicRefCounted<InitResult> {
        Optional<Error> error;
    };

    static NonnullRefPtr<Audio::RecordStream::CreatePromise> create(
        Audio::SampleSpecification const& sample_spec,
        u32 fragment_size_bytes,
        StringView device_id,
        RecordCallback callback)
    {
        auto promise = CreatePromise::construct();
        bool const has_event_loop = Core::EventLoop::is_running();
        RefPtr<Core::WeakEventLoopReference> caller_loop_weak;
        if (has_event_loop)
            caller_loop_weak = Core::EventLoop::current_weak();

        HANDLE stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_event) {
            promise->reject(Error::from_windows_error());
            return promise;
        }

        HANDLE init_done_event = nullptr;
        if (!has_event_loop) {
            init_done_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!init_done_event) {
                DWORD err = GetLastError();
                CloseHandle(stop_event);
                promise->reject(Error::from_windows_error(err));
                return promise;
            }
        }

        auto state = make_ref_counted<StreamState>();
        auto init_result = make_ref_counted<InitResult>();
        auto stream = adopt_ref(*new RecordStreamWasapi(
            sample_spec,
            fragment_size_bytes,
            ByteString(device_id),
            move(callback),
            stop_event,
            caller_loop_weak,
            state));
        state->stream = stream.ptr();

        stream->start_capture_thread(promise, init_result, init_done_event);

        if (!has_event_loop) {
            WaitForSingleObject(init_done_event, INFINITE);
            CloseHandle(init_done_event);
            if (init_result->error.has_value())
                promise->reject(init_result->error.release_value());
            else
                promise->resolve(stream);
        }

        return promise;
    }

    ~RecordStreamWasapi() override
    {
        m_state->alive.store(false);
        if (m_stop_event)
            SetEvent(m_stop_event);
        if (m_capture_thread) {
            if (GetCurrentThreadId() == m_capture_thread_id.load())
                m_capture_thread->detach();
            else if (m_capture_thread->needs_to_be_joined())
                (void)m_capture_thread->join();
        }
        if (m_stop_event)
            CloseHandle(m_stop_event);
    }

    Audio::SampleSpecification const& sample_specification() const override
    {
        return m_sample_specification;
    }

private:
    RecordStreamWasapi(
        Audio::SampleSpecification const& requested_spec,
        u32 fragment_size_bytes,
        ByteString device_id,
        RecordCallback callback,
        HANDLE stop_event,
        RefPtr<Core::WeakEventLoopReference> caller_loop_weak,
        NonnullRefPtr<StreamState> state)
        : m_requested_spec(requested_spec)
        , m_fragment_size_bytes(fragment_size_bytes)
        , m_device_id(move(device_id))
        , m_callback(move(callback))
        , m_stop_event(stop_event)
        , m_caller_loop_weak(move(caller_loop_weak))
        , m_state(move(state))
    {
    }

    void start_capture_thread(
        NonnullRefPtr<CreatePromise> promise,
        NonnullRefPtr<InitResult> init_result,
        HANDLE init_done_event)
    {
        // Hold a strong reference only until asynchronous initialization settles on the caller loop
        // so the capture loop itself never keeps the stream alive after the caller releases it.
        RefPtr<RecordStreamWasapi> init_hold = init_done_event ? nullptr : RefPtr<RecordStreamWasapi>(*this);
        RecordStreamWasapi* raw_self = this;
        m_capture_thread = Threading::Thread::construct("WASAPI Capture"sv, [raw_self, promise = move(promise), init_result = move(init_result), init_done_event, init_hold = move(init_hold)]() mutable -> intptr_t {
            raw_self->run_capture_loop(move(promise), move(init_result), init_done_event, move(init_hold));
            return 0;
        });
        m_capture_thread->start();
    }

    void settle_init_failure(
        NonnullRefPtr<CreatePromise> promise,
        NonnullRefPtr<InitResult> init_result,
        HANDLE init_done_event,
        RefPtr<RecordStreamWasapi> init_hold,
        Error error)
    {
        if (init_done_event) {
            init_result->error = move(error);
            SetEvent(init_done_event);
            return;
        }
        if (!m_caller_loop_weak)
            return;
        auto loop = m_caller_loop_weak->take();
        if (!loop.is_alive())
            return;
        loop->deferred_invoke([promise = move(promise), error = move(error), hold = move(init_hold)]() mutable {
            promise->reject(move(error));
        });
    }

    [[nodiscard]] bool settle_init_success(
        NonnullRefPtr<CreatePromise> promise,
        HANDLE init_done_event,
        RefPtr<RecordStreamWasapi> init_hold)
    {
        if (init_done_event) {
            SetEvent(init_done_event);
            return true;
        }
        if (!m_caller_loop_weak)
            return false;
        auto loop = m_caller_loop_weak->take();
        if (!loop.is_alive())
            return false;
        loop->deferred_invoke([promise = move(promise), hold = move(init_hold)]() mutable {
            promise->resolve(hold.release_nonnull());
        });
        return true;
    }

    void notify_capture_lost()
    {
        if (!m_caller_loop_weak || !m_state->alive.load())
            return;
        auto loop = m_caller_loop_weak->take();
        if (!loop.is_alive())
            return;
        loop->deferred_invoke([state = m_state] {
            if (!state->alive.load())
                return;
            if (state->stream && state->stream->on_capture_lost)
                state->stream->on_capture_lost();
        });
    }

    void run_capture_loop(
        NonnullRefPtr<CreatePromise> promise,
        NonnullRefPtr<InitResult> init_result,
        HANDLE init_done_event,
        RefPtr<RecordStreamWasapi> init_hold)
    {
        m_capture_thread_id.store(GetCurrentThreadId());
        auto fail_init = [&](Error error) {
            settle_init_failure(move(promise), move(init_result), init_done_event, move(init_hold), move(error));
        };

        ScopedComInitializer com;
        if (!com.initialized()) {
            fail_init(Error::from_windows_error(static_cast<u32>(com.hresult())));
            return;
        }

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            fail_init(Error::from_windows_error(static_cast<u32>(hr)));
            return;
        }

        ComPtr<IMMDevice> device;
        static constexpr auto wasapi_input_prefix = "wasapi:input:"sv;
        if (m_device_id.starts_with(wasapi_input_prefix) && m_device_id.length() > wasapi_input_prefix.length()) {
            auto raw_id = m_device_id.substring_view(wasapi_input_prefix.length());
            auto wide_id = utf8_to_wide(raw_id);
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
            hr = enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device);
            if (FAILED(hr) || !device) {
                fail_init(Error::from_windows_error(static_cast<u32>(hr)));
                return;
            }
        }

        ComPtr<IAudioClient> audio_client;
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audio_client.GetAddressOf()));
        if (FAILED(hr) || !audio_client) {
            fail_init(Error::from_windows_error(static_cast<u32>(hr)));
            return;
        }

        WAVEFORMATEX* mix_format = nullptr;
        hr = audio_client->GetMixFormat(&mix_format);
        if (FAILED(hr) || !mix_format) {
            fail_init(Error::from_windows_error(static_cast<u32>(hr)));
            return;
        }
        ScopeGuard free_mix_format = [&] { CoTaskMemFree(mix_format); };

        u32 const device_sample_rate = mix_format->nSamplesPerSec > 0 ? mix_format->nSamplesPerSec : 48000;
        u32 const device_channels = mix_format->nChannels > 0 ? mix_format->nChannels : 1;
        u32 const requested_sample_rate = m_requested_spec.is_valid() && m_requested_spec.sample_rate() > 0
            ? m_requested_spec.sample_rate()
            : device_sample_rate;
        u32 const requested_channels = m_requested_spec.is_valid() && m_requested_spec.channel_count() > 0
            ? m_requested_spec.channel_count()
            : 1;
        u32 const clamped_channels = min(max(requested_channels, 1u), min(max(device_channels, 1u), 2u));

        WAVEFORMATEXTENSIBLE desired_format {};
        desired_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        desired_format.Format.nChannels = static_cast<WORD>(clamped_channels);
        desired_format.Format.nSamplesPerSec = requested_sample_rate;
        desired_format.Format.wBitsPerSample = 32;
        desired_format.Format.nBlockAlign = static_cast<WORD>(clamped_channels * sizeof(float));
        desired_format.Format.nAvgBytesPerSec = requested_sample_rate * desired_format.Format.nBlockAlign;
        desired_format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        desired_format.Samples.wValidBitsPerSample = 32;
        desired_format.dwChannelMask = clamped_channels == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
        desired_format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        u64 const bytes_per_second = static_cast<u64>(requested_sample_rate) * clamped_channels * sizeof(float);
        REFERENCE_TIME buffer_duration = 400'000; // 40 ms default
        if (m_fragment_size_bytes > 0 && bytes_per_second > 0) {
            u64 fragment_duration_100ns = (static_cast<u64>(m_fragment_size_bytes) * 10'000'000ULL) / bytes_per_second;
            buffer_duration = static_cast<REFERENCE_TIME>(min(max(fragment_duration_100ns * 2, 200'000ULL), 2'000'000ULL));
        }

        bool using_native_fallback = false;
        u32 output_sample_rate = requested_sample_rate;
        u32 output_channels = clamped_channels;
        u32 native_channels = clamped_channels;
        u32 native_bytes_per_sample = sizeof(float);
        NativeSampleEncoding native_encoding = NativeSampleEncoding::Float32;

        DWORD const autoconvert_flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK
            | AUDCLNT_STREAMFLAGS_NOPERSIST
            | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
            | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        hr = audio_client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            autoconvert_flags,
            buffer_duration,
            0,
            reinterpret_cast<WAVEFORMATEX const*>(&desired_format),
            nullptr);

        if (FAILED(hr)) {
            audio_client.Reset();
            hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audio_client.GetAddressOf()));
            if (FAILED(hr) || !audio_client) {
                fail_init(Error::from_windows_error(static_cast<u32>(hr)));
                return;
            }

            hr = audio_client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_NOPERSIST,
                buffer_duration,
                0,
                mix_format,
                nullptr);
            if (FAILED(hr)) {
                fail_init(Error::from_windows_error(static_cast<u32>(hr)));
                return;
            }

            auto detected_encoding = detect_native_sample_encoding(mix_format);
            if (!detected_encoding.has_value() || mix_format->nChannels == 0 || mix_format->nSamplesPerSec == 0) {
                fail_init(Error::from_string_literal("Unsupported WASAPI native capture format"));
                return;
            }

            using_native_fallback = true;
            native_encoding = *detected_encoding;
            native_channels = mix_format->nChannels;
            native_bytes_per_sample = mix_format->wBitsPerSample / 8;
            output_sample_rate = mix_format->nSamplesPerSec;
            output_channels = min(max(requested_channels, 1u), min(max(native_channels, 1u), 2u));
        }

        HANDLE sample_ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!sample_ready_event) {
            fail_init(Error::from_windows_error());
            return;
        }
        ScopeGuard close_sample_ready_event = [&] { CloseHandle(sample_ready_event); };

        hr = audio_client->SetEventHandle(sample_ready_event);
        if (FAILED(hr)) {
            fail_init(Error::from_windows_error(static_cast<u32>(hr)));
            return;
        }

        ComPtr<IAudioCaptureClient> capture_client;
        hr = audio_client->GetService(IID_PPV_ARGS(&capture_client));
        if (FAILED(hr) || !capture_client) {
            fail_init(Error::from_windows_error(static_cast<u32>(hr)));
            return;
        }

        m_sample_specification = Audio::SampleSpecification(
            output_sample_rate,
            output_channels == 1 ? Audio::ChannelMap::mono() : Audio::ChannelMap::stereo());

        hr = audio_client->Start();
        if (FAILED(hr)) {
            fail_init(Error::from_windows_error(static_cast<u32>(hr)));
            return;
        }
        ScopeGuard stop_audio_client = [&] { (void)audio_client->Stop(); };

        DWORD task_index = 0;
        HANDLE avrt_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
        ScopeGuard revert_avrt = [&] {
            if (avrt_handle)
                AvRevertMmThreadCharacteristics(avrt_handle);
        };

        if (!settle_init_success(move(promise), init_done_event, move(init_hold)))
            return;

        Vector<float> scratch_samples;
        HANDLE wait_handles[2] = { m_stop_event, sample_ready_event };
        bool capture_failed = false;

        while (m_state->alive.load() && !capture_failed) {
            DWORD wait_result = WaitForMultipleObjects(2, wait_handles, FALSE, 200);
            if (wait_result == WAIT_OBJECT_0)
                break;
            if (wait_result != WAIT_OBJECT_0 + 1 && wait_result != WAIT_TIMEOUT) {
                capture_failed = true;
                break;
            }

            while (m_state->alive.load()) {
                UINT32 packet_frames = 0;
                HRESULT next_hr = capture_client->GetNextPacketSize(&packet_frames);
                if (FAILED(next_hr)) {
                    capture_failed = true;
                    break;
                }
                if (packet_frames == 0)
                    break;

                BYTE* data = nullptr;
                UINT32 num_frames = 0;
                DWORD flags = 0;
                HRESULT buffer_hr = capture_client->GetBuffer(&data, &num_frames, &flags, nullptr, nullptr);
                if (FAILED(buffer_hr)) {
                    capture_failed = true;
                    break;
                }

                if (num_frames > 0 && m_callback) {
                    if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) {
                        size_t sample_count = static_cast<size_t>(num_frames) * output_channels;
                        scratch_samples.resize(sample_count);
                        scratch_samples.span().fill(0.0f);
                        m_callback(ReadonlyBytes { scratch_samples.data(), sample_count * sizeof(float) }, m_sample_specification);
                    } else if (!using_native_fallback) {
                        size_t byte_count = static_cast<size_t>(num_frames) * output_channels * sizeof(float);
                        m_callback(ReadonlyBytes { data, byte_count }, m_sample_specification);
                    } else {
                        size_t sample_count = static_cast<size_t>(num_frames) * output_channels;
                        scratch_samples.resize(sample_count);
                        u32 const native_stride = native_channels * native_bytes_per_sample;
                        for (UINT32 frame = 0; frame < num_frames; ++frame) {
                            u8 const* frame_ptr = data + static_cast<size_t>(frame) * native_stride;
                            float* out_ptr = scratch_samples.data() + static_cast<size_t>(frame) * output_channels;
                            if (output_channels == 1 && native_channels > 1) {
                                float sum = 0.0f;
                                for (u32 ch = 0; ch < native_channels; ++ch)
                                    sum += read_native_sample_as_float(frame_ptr, ch, native_bytes_per_sample, native_encoding);
                                out_ptr[0] = sum / static_cast<float>(native_channels);
                            } else {
                                for (u32 ch = 0; ch < output_channels; ++ch) {
                                    u32 source_ch = min(ch, native_channels - 1);
                                    out_ptr[ch] = read_native_sample_as_float(frame_ptr, source_ch, native_bytes_per_sample, native_encoding);
                                }
                            }
                        }
                        m_callback(ReadonlyBytes { scratch_samples.data(), sample_count * sizeof(float) }, m_sample_specification);
                    }
                }

                HRESULT release_hr = capture_client->ReleaseBuffer(num_frames);
                if (FAILED(release_hr)) {
                    capture_failed = true;
                    break;
                }
            }
        }

        if (capture_failed && m_state->alive.load())
            notify_capture_lost();
    }

    Audio::SampleSpecification m_requested_spec;
    Audio::SampleSpecification m_sample_specification;
    u32 m_fragment_size_bytes { 0 };
    ByteString m_device_id;
    RecordCallback m_callback;
    HANDLE m_stop_event { nullptr };
    RefPtr<Core::WeakEventLoopReference> m_caller_loop_weak;
    NonnullRefPtr<StreamState> m_state;
    RefPtr<Threading::Thread> m_capture_thread;
    Atomic<DWORD> m_capture_thread_id { 0 };
};

NonnullRefPtr<Audio::RecordStream::CreatePromise> create_platform_record_stream(
    Audio::SampleSpecification const& sample_spec,
    u32 fragment_size_bytes,
    StringView device_id,
    Audio::RecordStream::RecordCallback callback)
{
    return RecordStreamWasapi::create(sample_spec, fragment_size_bytes, device_id, move(callback));
}

}
