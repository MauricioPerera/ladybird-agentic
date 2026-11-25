/*
 * Copyright (c) 2025, Ryszard Goc <ryszardgoc@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/Atomic.h>
#include <AK/AtomicRefCounted.h>
#include <AK/Error.h>
#include <AK/FixedArray.h>
#include <AK/Format.h>
#include <AK/Math.h>
#include <AK/Mutex.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Platform.h>
#include <AK/Queue.h>
#include <AK/RefPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <AudioServer/PlatformAudio.h>
#include <AudioServer/PlaybackStreamWasapi.h>
#include <LibAudio/ChannelMap.h>
#include <LibAudio/SampleSpecification.h>
#include <LibAudio/WindowsChannelMask.h>
#include <LibCore/System.h>
#include <LibCore/ThreadedPromise.h>
#include <LibThreading/Thread.h>

#include <AK/Windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <timeapi.h>

// NOTE: Not using the newer winrt that supersedes wrl as that uses exceptions for error handling
#include <wrl/client.h>

namespace Audio {

using namespace Microsoft::WRL;

#define MUST_HR(expression)                                                                \
    ({                                                                                     \
        AK_IGNORE_DIAGNOSTIC("-Wshadow", HRESULT&& _temporary_hr = (expression));          \
        if (FAILED(_temporary_hr)) [[unlikely]] {                                          \
            dbgln("Expression failed with: {}", Error::from_windows_error(_temporary_hr)); \
            VERIFY_NOT_REACHED();                                                          \
        }                                                                                  \
    })

// GUID for the playback session. That way all render streams have a single volume slider in the OS interface
constexpr GUID PlaybackSessionGUID = { // 22f2ca89-210a-492c-a0aa-f25b1d2f33a1
    0x22f2ca89,
    0x210a,
    0x492c,
    { 0xa0, 0xaa, 0xf2, 0x5b, 0x1d, 0x2f, 0x33, 0xa1 }
};

struct TaskPlay {
    NonnullRefPtr<Core::ThreadedPromise<void>> promise;
};

struct TaskDrainAndSuspend {
    NonnullRefPtr<Core::ThreadedPromise<void>> promise;
};

struct TaskDiscardAndSuspend {
    NonnullRefPtr<Core::ThreadedPromise<void>> promise;
};

struct TaskResumeFromUnderrun { };

class ComUninitializer {
public:
    ~ComUninitializer()
    {
        if (initialized)
            CoUninitialize();
    }
    bool initialized = false;
};

static thread_local ComUninitializer s_com_uninitializer {};

struct PlaybackStreamWASAPI::AudioState : public AtomicRefCounted<PlaybackStreamWASAPI::AudioState> {
    AudioState();
    ~AudioState();

    Mutex stream_volume_mutex;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> audio_client;
    ComPtr<IAudioRenderClient> render_client;
    ComPtr<IAudioStreamVolume> audio_stream_volume;

    WAVEFORMATEXTENSIBLE wave_format;
    UINT32 buffer_frame_count;
    REFERENCE_TIME stream_latency = 0;
    HANDLE buffer_event = 0;

    PlaybackStreamWASAPI::AudioDataRequestCallback data_request_callback;

    Mutex task_queue_mutex;
    Queue<Variant<TaskPlay, TaskDrainAndSuspend, TaskDiscardAndSuspend, TaskResumeFromUnderrun>> task_queue;
    // FIXME: Create a owning handle type to be shared in the codebase
    HANDLE task_event = 0;

    enum class Paused : u8 {
        No,
        Explicit,
        Underrun,
    };
    Atomic<Paused> paused = Paused::Explicit;
    bool drain_and_suspend = false;
    Atomic<bool> device_invalidated = false;
    Atomic<bool> exit_requested = false;

    static bool reopen_default_render_device(AudioState& state);
    static int render_thread_loop(AudioState& state);
    static void drain_buffer_and_stop(AudioState& state);
    RefPtr<Core::ThreadedPromise<void>> resume_promise;
    RefPtr<Core::ThreadedPromise<void>> suspend_promise;

    Vector<float, ChannelMap::capacity()> channel_volumes;
};

PlaybackStreamWASAPI::AudioState::AudioState()
{
    task_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    VERIFY(task_event);
}

PlaybackStreamWASAPI::AudioState::~AudioState()
{
    if (buffer_event)
        CloseHandle(buffer_event);
    if (task_event)
        CloseHandle(task_event);
}

PlaybackStreamWASAPI::PlaybackStreamWASAPI(NonnullRefPtr<AudioState> state)
    : m_state(move(state))
{
}

PlaybackStreamWASAPI::~PlaybackStreamWASAPI()
{
    m_state->exit_requested.store(true, MemoryOrder::memory_order_release);
    // Poke the event to wake the thread up from wait
    VERIFY(m_state->buffer_event != NULL);
    SetEvent(m_state->buffer_event);
}

NonnullRefPtr<PlaybackStream::CreatePromise> create_platform_playback_stream(OutputState initial_output_state, u32 target_latency_ms, PlaybackStream::AudioDataRequestCallback data_callback)
{
    return PlaybackStreamWASAPI::create(initial_output_state, target_latency_ms, move(data_callback));
}

static void print_audio_format(WAVEFORMATEXTENSIBLE& format)
{
    VERIFY(format.Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE);
    dbgln("wFormatTag: {:x}\n"
          "nChannels: {}\n"
          "nSamplesPerSec: {}\n"
          "nAvgBytesPerSec: {}\n"
          "nBlockAlign: {}\n"
          "wBitsPerSample: {}\n"
          "cbSize: {}\n"
          "Samples.wValidBitsPerSample: {}\n"
          "dwChannelMask: {:b}\n"
          "SubFormat: {}",
        format.Format.wFormatTag,
        format.Format.nChannels,
        format.Format.nSamplesPerSec,
        format.Format.nAvgBytesPerSec,
        format.Format.nBlockAlign,
        format.Format.wBitsPerSample,
        format.Format.cbSize,
        format.Samples.wValidBitsPerSample,
        format.dwChannelMask,
        Span<u8> { reinterpret_cast<char*>(&format.SubFormat), 16 });
}

NonnullRefPtr<PlaybackStream::CreatePromise> PlaybackStreamWASAPI::create(OutputState initial_output_state, u32, AudioDataRequestCallback&& data_request_callback)
{
    auto promise = CreatePromise::construct();

#define TRY_HR(expression)                                                        \
    ({                                                                            \
        AK_IGNORE_DIAGNOSTIC("-Wshadow", HRESULT&& _temporary_hr = (expression)); \
        if (FAILED(_temporary_hr)) [[unlikely]] {                                 \
            promise->reject(Error::from_windows_error(_temporary_hr));            \
            return promise;                                                       \
        }                                                                         \
    })

    HRESULT hr;
    if (!s_com_uninitializer.initialized) {
        TRY_HR(CoInitializeEx(NULL, COINIT_MULTITHREADED));
        s_com_uninitializer.initialized = true;
    }

    auto state = make_ref_counted<PlaybackStreamWASAPI::AudioState>();

    TRY_HR(CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, IID_PPV_ARGS(&state->enumerator)));
    TRY_HR(state->enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &state->device));
    TRY_HR(state->device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &state->audio_client));

    state->data_request_callback = move(data_request_callback);

    WAVEFORMATEXTENSIBLE* device_format;
    state->audio_client->GetMixFormat(reinterpret_cast<WAVEFORMATEX**>(&device_format));
    ScopeGuard free_mix_format = [&device_format] { CoTaskMemFree(device_format); };

    dbgln_if(AUDIO_DEBUG, "PlaybackStreamWASAPI: Mixing engine audio format:\n");
    if (AUDIO_DEBUG)
        print_audio_format(*device_format);

    VERIFY(device_format->Format.wFormatTag == WAVE_FORMAT_EXTENSIBLE);
    VERIFY(device_format->Format.nChannels <= ChannelMap::capacity());
    VERIFY(popcount(device_format->dwChannelMask) == device_format->Format.nChannels);
    auto channels = device_format->Format.nChannels;

    // Set up a 32bit float pcm stream with whatever sample rate and channels we were given.

    auto block_align = channels * sizeof(float);
    state->wave_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    state->wave_format.Format.nChannels = channels;
    state->wave_format.Format.nSamplesPerSec = device_format->Format.nSamplesPerSec;
    state->wave_format.Format.nAvgBytesPerSec = device_format->Format.nSamplesPerSec * block_align;
    state->wave_format.Format.nBlockAlign = block_align;
    state->wave_format.Format.wBitsPerSample = 32;
    state->wave_format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    state->wave_format.Samples.wValidBitsPerSample = 32;
    state->wave_format.dwChannelMask = device_format->dwChannelMask;
    state->wave_format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    state->channel_volumes.resize(channels);

    WAVEFORMATEXTENSIBLE* closest_match;
    hr = state->audio_client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &state->wave_format.Format, reinterpret_cast<WAVEFORMATEX**>(&closest_match));
    if (FAILED(hr)) {
        promise->reject(Error::from_windows_error(hr));
        return promise;
    }
    if (hr == S_FALSE) {
        dbgln("Audio format not supported. Current format:\n");
        print_audio_format(state->wave_format);
        dbgln("Closest supported audio format:\n");
        print_audio_format(*closest_match);
        CoTaskMemFree(closest_match);
        VERIFY_NOT_REACHED();
    }
    // TODO: check the actual format of the engine and use it if possible to reduce overhead
    DWORD stream_flags
        = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST;
    // For event driven buffering we can't specify the buffer duration.
    TRY_HR(state->audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0, &state->wave_format.Format, &PlaybackSessionGUID));
    TRY_HR(state->audio_client->GetBufferSize(&state->buffer_frame_count));
    TRY_HR(state->audio_client->GetStreamLatency(&state->stream_latency));
    TRY_HR(state->audio_client->GetService(IID_PPV_ARGS(&state->render_client)));
    TRY_HR(state->audio_client->GetService(IID_PPV_ARGS(&state->audio_stream_volume)));

    state->buffer_event = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (!state->buffer_event) {
        promise->reject(Error::from_windows_error(hr));
        return promise;
    }

    TRY_HR(state->audio_client->SetEventHandle(state->buffer_event));

    if (initial_output_state == OutputState::Playing)
        state->paused = AudioState::Paused::No;

    auto audio_thread = Threading::Thread::construct("Audio Render"sv, [state] {
        return AudioState::render_thread_loop(*state);
    });

    if (initial_output_state == OutputState::Playing)
        TRY_HR(state->audio_client->Start());

    audio_thread->start();
    audio_thread->detach();

    auto stream = adopt_ref(*new PlaybackStreamWASAPI(move(state)));
    promise->resolve(stream);
    return promise;

#undef TRY_HR
}

SampleSpecification PlaybackStreamWASAPI::sample_specification() const
{
    auto channel_map = MUST(windows_channel_mask_to_channel_map(m_state->wave_format.dwChannelMask));
    return SampleSpecification { m_state->wave_format.Format.nSamplesPerSec, channel_map };
}

bool PlaybackStreamWASAPI::AudioState::reopen_default_render_device(PlaybackStreamWASAPI::AudioState& state)
{
    state.render_client.Reset();
    {
        MutexLocker volume_locker(state.stream_volume_mutex);
        state.audio_stream_volume.Reset();
    }
    state.audio_client.Reset();
    state.device.Reset();

    if (!state.enumerator || !state.buffer_event) {
        state.device_invalidated.store(true, MemoryOrder::memory_order_release);
        state.paused = Paused::Explicit;
        return false;
    }

    ComPtr<IMMDevice> new_device;
    HRESULT hr = state.enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &new_device);
    if (FAILED(hr) || !new_device) {
        dbgln("PlaybackStreamWASAPI: GetDefaultAudioEndpoint failed during reopen (0x{:08x})", static_cast<u32>(hr));
        state.device_invalidated.store(true, MemoryOrder::memory_order_release);
        state.paused = Paused::Explicit;
        return false;
    }

    ComPtr<IAudioClient> new_client;
    hr = new_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &new_client);
    if (FAILED(hr) || !new_client) {
        dbgln("PlaybackStreamWASAPI: Activate IAudioClient failed during reopen (0x{:08x})", static_cast<u32>(hr));
        state.device_invalidated.store(true, MemoryOrder::memory_order_release);
        state.paused = Paused::Explicit;
        return false;
    }

    DWORD stream_flags
        = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST;
    hr = new_client->Initialize(AUDCLNT_SHAREMODE_SHARED, stream_flags, 0, 0, &state.wave_format.Format, &PlaybackSessionGUID);
    if (FAILED(hr)) {
        dbgln("PlaybackStreamWASAPI: IAudioClient::Initialize failed during reopen (0x{:08x})", static_cast<u32>(hr));
        state.device_invalidated.store(true, MemoryOrder::memory_order_release);
        state.paused = Paused::Explicit;
        return false;
    }

    UINT32 new_buffer_frame_count = 0;
    hr = new_client->GetBufferSize(&new_buffer_frame_count);
    if (FAILED(hr) || new_buffer_frame_count == 0) {
        dbgln("PlaybackStreamWASAPI: GetBufferSize failed during reopen (0x{:08x})", static_cast<u32>(hr));
        state.device_invalidated.store(true, MemoryOrder::memory_order_release);
        state.paused = Paused::Explicit;
        return false;
    }

    REFERENCE_TIME new_stream_latency = 0;
    if (FAILED(new_client->GetStreamLatency(&new_stream_latency)))
        new_stream_latency = state.stream_latency;

    ComPtr<IAudioRenderClient> new_render_client;
    hr = new_client->GetService(IID_PPV_ARGS(&new_render_client));
    if (FAILED(hr) || !new_render_client) {
        dbgln("PlaybackStreamWASAPI: GetService(IAudioRenderClient) failed during reopen (0x{:08x})", static_cast<u32>(hr));
        state.device_invalidated.store(true, MemoryOrder::memory_order_release);
        state.paused = Paused::Explicit;
        return false;
    }

    ComPtr<IAudioStreamVolume> new_stream_volume;
    if (SUCCEEDED(new_client->GetService(IID_PPV_ARGS(&new_stream_volume))) && new_stream_volume) {
        if (!state.channel_volumes.is_empty())
            (void)new_stream_volume->SetAllVolumes(state.channel_volumes.size(), state.channel_volumes.data());
    }

    ResetEvent(state.buffer_event);
    hr = new_client->SetEventHandle(state.buffer_event);
    if (FAILED(hr)) {
        dbgln("PlaybackStreamWASAPI: SetEventHandle failed during reopen (0x{:08x})", static_cast<u32>(hr));
        state.device_invalidated.store(true, MemoryOrder::memory_order_release);
        state.paused = Paused::Explicit;
        return false;
    }

    state.device = move(new_device);
    state.audio_client = move(new_client);
    state.render_client = move(new_render_client);
    {
        MutexLocker volume_locker(state.stream_volume_mutex);
        state.audio_stream_volume = move(new_stream_volume);
    }
    state.buffer_frame_count = new_buffer_frame_count;
    state.stream_latency = new_stream_latency;
    state.device_invalidated.store(false, MemoryOrder::memory_order_release);

    if (state.paused == Paused::No) {
        hr = state.audio_client->Start();
        if (FAILED(hr) && hr != AUDCLNT_E_NOT_STOPPED) {
            dbgln("PlaybackStreamWASAPI: Start failed during reopen (0x{:08x})", static_cast<u32>(hr));
            state.device_invalidated.store(true, MemoryOrder::memory_order_release);
            state.paused = Paused::Explicit;
            return false;
        }
    }

    dbgln("PlaybackStreamWASAPI: reopened default audio render endpoint");
    return true;
}

void PlaybackStreamWASAPI::AudioState::drain_buffer_and_stop(PlaybackStreamWASAPI::AudioState& state)
{
    if (!state.audio_client)
        return;

    u32 padding = 0;
    HRESULT hr = state.audio_client->GetCurrentPadding(&padding);
    if (FAILED(hr))
        return;

    if (padding > 0) {
        u32 ms_to_sleep = padding * 1'000ull / state.wave_format.Format.nSamplesPerSec;
        if (ms_to_sleep > 0) {
            Sleep(min(ms_to_sleep - 1, 500u));
            if (FAILED(state.audio_client->GetCurrentPadding(&padding)))
                return;
        }
        if (padding == 0)
            dbgln_if(AUDIO_DEBUG, "------- PlaybackStreamWASAPI: overslept draining buffer --------");
        auto deadline = MonotonicTime::now() + AK::Duration::from_milliseconds(500);
        while (padding > 0 && MonotonicTime::now() < deadline) {
            AK::atomic_pause();
            if (FAILED(state.audio_client->GetCurrentPadding(&padding)))
                return;
        }
    }

    (void)state.audio_client->Stop();
}

int PlaybackStreamWASAPI::AudioState::render_thread_loop(PlaybackStreamWASAPI::AudioState& state)
{
    MUST_HR(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    WORD block_align = state.wave_format.Format.nBlockAlign;

    ScopeGuard uninitialize_com = [] { CoUninitialize(); };

    VERIFY(timeBeginPeriod(1) == TIMERR_NOERROR);
    DWORD task_index = 0;
    HANDLE task_handle = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_index);
    ScopeGuard revert_thread_priority = [&task_handle] { AvRevertMmThreadCharacteristics(task_handle); };

    while (!state.exit_requested.load(MemoryOrder::memory_order_acquire)) {
        Array handles = { state.task_event, state.buffer_event };
        DWORD result = WaitForMultipleObjects(handles.size(), handles.data(), FALSE, INFINITE);
        switch (result) {
        case WAIT_OBJECT_0: {
            state.task_queue_mutex.lock();
            while (!state.task_queue.is_empty()) {
                auto task = state.task_queue.dequeue();
                task.visit(
                    [&state](TaskPlay const& task) {
                        if (state.device_invalidated.load(MemoryOrder::memory_order_acquire) || !state.audio_client)
                            (void)reopen_default_render_device(state);
                        if (state.audio_client) {
                            HRESULT hr = state.audio_client->Start();
                            if (FAILED(hr) && hr != AUDCLNT_E_NOT_STOPPED) {
                                dbgln("PlaybackStreamWASAPI: Start failed on TaskPlay (0x{:08x}), attempting reopen", static_cast<u32>(hr));
                                if (reopen_default_render_device(state) && state.audio_client)
                                    hr = state.audio_client->Start();
                            }
                            if (SUCCEEDED(hr) || hr == AUDCLNT_E_NOT_STOPPED)
                                state.paused = Paused::No;
                            else
                                state.paused = Paused::Explicit;
                        }
                        task.promise->resolve();
                    },
                    [&state](TaskDrainAndSuspend const& task) {
                        state.paused = Paused::Explicit;
                        drain_buffer_and_stop(state);
                        task.promise->resolve();
                    },
                    [&state](TaskDiscardAndSuspend const& task) {
                        state.paused = Paused::Explicit;
                        if (state.audio_client) {
                            (void)state.audio_client->Stop();
                            (void)state.audio_client->Reset();
                        }
                        task.promise->resolve();
                    },
                    [&state](TaskResumeFromUnderrun const&) {
                        if (state.paused != Paused::Underrun)
                            return;
                        if (state.device_invalidated.load(MemoryOrder::memory_order_acquire) || !state.audio_client) {
                            if (!reopen_default_render_device(state)) {
                                state.paused = Paused::Explicit;
                                return;
                            }
                        }
                        if (!state.audio_client) {
                            state.paused = Paused::Explicit;
                            return;
                        }
                        HRESULT hr = state.audio_client->Start();
                        if (FAILED(hr) && hr != AUDCLNT_E_NOT_STOPPED) {
                            dbgln("PlaybackStreamWASAPI: Start failed on TaskResumeFromUnderrun (0x{:08x}), attempting reopen", static_cast<u32>(hr));
                            if (reopen_default_render_device(state) && state.audio_client)
                                hr = state.audio_client->Start();
                        }
                        if (SUCCEEDED(hr) || hr == AUDCLNT_E_NOT_STOPPED)
                            state.paused.store(Paused::No);
                        else
                            state.paused.store(Paused::Explicit);
                    });
            }
            state.task_queue_mutex.unlock();
            DWORD res = WaitForSingleObject(handles[1], 0);
            // Both the task event and buffer event were signaled
            if (res == WAIT_OBJECT_0)
                break;
            // The buffer event wasn't signaled, so we skip the iteration
            if (res == WAIT_TIMEOUT)
                continue;
            // Unless the wait errors we should never hit this
            VERIFY_NOT_REACHED();
        }
        case WAIT_OBJECT_0 + 1:
            break;
        default:
            VERIFY_NOT_REACHED();
        }

        // We check that after the wait we weren't asked to exit.
        if (state.exit_requested.load(MemoryOrder::memory_order_acquire)) [[unlikely]]
            break;

        if (state.paused != Paused::No)
            continue;

        if (!state.audio_client || !state.render_client) {
            if (!reopen_default_render_device(state))
                state.paused = Paused::Explicit;
            continue;
        }

        u32 padding = 0;
        HRESULT hr = state.audio_client->GetCurrentPadding(&padding);
        if (FAILED(hr)) {
            dbgln("PlaybackStreamWASAPI: WASAPI call failed (0x{:08x}), attempting device recovery", static_cast<u32>(hr));
            if (!reopen_default_render_device(state))
                state.paused = Paused::Explicit;
            continue;
        }

        u32 frames_available = state.buffer_frame_count - padding;
        if (frames_available == 0) [[unlikely]]
            continue;
        auto buffer_starts_playing_at = MonotonicTime::now() + AK::Duration::from_time_units(padding, 1, state.wave_format.Format.nSamplesPerSec) + AK::Duration::from_nanoseconds(static_cast<i64>(state.stream_latency) * 100);

        BYTE* buffer = nullptr;
        hr = state.render_client->GetBuffer(frames_available, &buffer);
        if (FAILED(hr) || !buffer) {
            dbgln("PlaybackStreamWASAPI: WASAPI call failed (0x{:08x}), attempting device recovery", static_cast<u32>(hr));
            if (!reopen_default_render_device(state))
                state.paused = Paused::Explicit;
            continue;
        }

        u32 buffer_size = frames_available * block_align;
        auto output_buffer = Bytes(buffer, buffer_size).reinterpret<float>();
        auto floats_written = state.data_request_callback(output_buffer, buffer_starts_playing_at);
        if (floats_written.is_empty()) [[unlikely]] {
            hr = state.render_client->ReleaseBuffer(0, AUDCLNT_BUFFERFLAGS_SILENT);
            if (FAILED(hr)) {
                dbgln("PlaybackStreamWASAPI: WASAPI call failed (0x{:08x}), attempting device recovery", static_cast<u32>(hr));
                if (!reopen_default_render_device(state))
                    state.paused = Paused::Explicit;
                continue;
            }
            state.paused = Paused::Underrun;
            drain_buffer_and_stop(state);
        } else {
            frames_available = floats_written.size() / state.wave_format.Format.nChannels;
            hr = state.render_client->ReleaseBuffer(frames_available, 0);
            if (FAILED(hr)) {
                dbgln("PlaybackStreamWASAPI: WASAPI call failed (0x{:08x}), attempting device recovery", static_cast<u32>(hr));
                if (!reopen_default_render_device(state))
                    state.paused = Paused::Explicit;
                continue;
            }
        }
    }

    VERIFY(timeEndPeriod(1) == TIMERR_NOERROR);

    return 0;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamWASAPI::resume()
{
    auto promise = Core::ThreadedPromise<void>::create();
    TaskPlay task = { .promise = promise };

    m_state->task_queue_mutex.lock();
    m_state->task_queue.enqueue(move(task));
    SetEvent(m_state->task_event);
    m_state->task_queue_mutex.unlock();

    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamWASAPI::drain_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    TaskDrainAndSuspend task = { .promise = promise };

    m_state->task_queue_mutex.lock();
    m_state->task_queue.enqueue(move(task));
    SetEvent(m_state->task_event);
    m_state->task_queue_mutex.unlock();

    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamWASAPI::discard_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    TaskDiscardAndSuspend task = { .promise = promise };

    m_state->task_queue_mutex.lock();
    m_state->task_queue.enqueue(move(task));
    SetEvent(m_state->task_event);
    m_state->task_queue_mutex.unlock();

    return promise;
}

void PlaybackStreamWASAPI::notify_data_available()
{
    if (m_state->paused != AudioState::Paused::Underrun)
        return;

    m_state->task_queue_mutex.lock();
    m_state->task_queue.enqueue(TaskResumeFromUnderrun {});
    SetEvent(m_state->task_event);
    m_state->task_queue_mutex.unlock();
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamWASAPI::set_volume(double volume)
{
    HRESULT hr;
    auto promise = Core::ThreadedPromise<void>::create();
    if (!s_com_uninitializer.initialized) [[unlikely]] {
        hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) [[unlikely]] {
            promise->reject(Error::from_windows_error(hr));
            return promise;
        }
        s_com_uninitializer.initialized = true;
    }

    float clamped_volume = static_cast<float>(clamp(volume, 0.0, 1.0));

    m_state->channel_volumes.fill(clamped_volume);

    ComPtr<IAudioStreamVolume> stream_volume;
    {
        MutexLocker volume_locker(m_state->stream_volume_mutex);
        stream_volume = m_state->audio_stream_volume;
    }
    if (!stream_volume) {
        promise->resolve();
        return promise;
    }

    hr = stream_volume->SetAllVolumes(m_state->channel_volumes.size(), m_state->channel_volumes.data());
    if (FAILED(hr)) [[unlikely]] {
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
            promise->resolve();
        else
            promise->reject(Error::from_windows_error(hr));
    } else [[likely]] {
        promise->resolve();
    }
    return promise;
}

}
