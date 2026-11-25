/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/NeverDestroyed.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <AudioServer/PlatformAudio.h>
#include <LibCore/EventLoop.h>
#include <mutex>

#include <AK/Windows.h>
#include <initguid.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <propsys.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

namespace Audio {

using Microsoft::WRL::ComPtr;

namespace {

class ScopedComInitializer {
    AK_MAKE_NONCOPYABLE(ScopedComInitializer);
    AK_MAKE_NONMOVABLE(ScopedComInitializer);

public:
    AK_ALLOC_WITH_KMALLOC;

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

ByteString wide_to_utf8(wchar_t const* value)
{
    if (!value || *value == L'\0')
        return {};
    int length = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1)
        return {};
    Vector<char> buffer;
    buffer.resize(static_cast<size_t>(length));
    int written = WideCharToMultiByte(CP_UTF8, 0, value, -1, buffer.data(), length, nullptr, nullptr);
    if (written <= 1)
        return {};
    return ByteString(buffer.data(), static_cast<size_t>(written - 1));
}

void read_device_format(IMMDevice* device, u32& sample_rate_hz, u32& channel_count)
{
    if (!device)
        return;

    ComPtr<IAudioClient> audio_client;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(audio_client.GetAddressOf())))) {
        WAVEFORMATEX* mix_format = nullptr;
        if (SUCCEEDED(audio_client->GetMixFormat(&mix_format)) && mix_format) {
            ScopeGuard free_format = [&] { CoTaskMemFree(mix_format); };
            sample_rate_hz = mix_format->nSamplesPerSec;
            channel_count = mix_format->nChannels;
            return;
        }
    }

    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties)) || !properties)
        return;

    PROPVARIANT value;
    PropVariantInit(&value);
    ScopeGuard clear_prop = [&] { PropVariantClear(&value); };
    if (SUCCEEDED(properties->GetValue(PKEY_AudioEngine_DeviceFormat, &value))
        && value.vt == VT_BLOB
        && value.blob.cbSize >= sizeof(WAVEFORMATEX)
        && value.blob.pBlobData) {
        auto const* format = reinterpret_cast<WAVEFORMATEX const*>(value.blob.pBlobData);
        sample_rate_hz = format->nSamplesPerSec;
        channel_count = format->nChannels;
    }
}

ByteString read_device_friendly_name(IMMDevice* device)
{
    if (!device)
        return {};

    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties)) || !properties)
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

ErrorOr<Vector<Audio::AudioDeviceInfo>> enumerate_flow(
    IMMDeviceEnumerator* enumerator,
    EDataFlow flow,
    StringView id_prefix,
    StringView fallback_label)
{
    Vector<Audio::AudioDeviceInfo> devices;

    ByteString default_raw_id;
    ComPtr<IMMDevice> default_device;
    HRESULT default_hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &default_device);
    if (SUCCEEDED(default_hr) && default_device) {
        LPWSTR default_id_wide = nullptr;
        if (SUCCEEDED(default_device->GetId(&default_id_wide)) && default_id_wide) {
            ScopeGuard free_default_id = [&] { CoTaskMemFree(default_id_wide); };
            default_raw_id = wide_to_utf8(default_id_wide);
        }
    } else if (default_hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        // Ignore missing default device errors when no hardware endpoint is connected;
        // surface unexpected COM failures if endpoint enumeration also fails below.
    }

    ComPtr<IMMDeviceCollection> collection;
    HRESULT enum_hr = enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(enum_hr))
        return Error::from_windows_error(static_cast<u32>(enum_hr));

    UINT count = 0;
    HRESULT count_hr = collection->GetCount(&count);
    if (FAILED(count_hr))
        return Error::from_windows_error(static_cast<u32>(count_hr));

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device)) || !device)
            continue;

        LPWSTR id_wide = nullptr;
        if (FAILED(device->GetId(&id_wide)) || !id_wide)
            continue;
        ScopeGuard free_id = [&] { CoTaskMemFree(id_wide); };

        auto raw_id = wide_to_utf8(id_wide);
        if (raw_id.is_empty())
            continue;

        auto label = read_device_friendly_name(device.Get());
        if (label.is_empty())
            label = fallback_label;

        u32 sample_rate_hz = 0;
        u32 channel_count = 0;
        read_device_format(device.Get(), sample_rate_hz, channel_count);

        devices.append({
            .dom_device_id = ByteString::formatted("{}{}", id_prefix, raw_id),
            .label = move(label),
            .group_id = raw_id,
            .sample_rate_hz = sample_rate_hz,
            .channel_count = channel_count,
            .is_default = !default_raw_id.is_empty() && raw_id == default_raw_id,
        });
    }

    return devices;
}

ErrorOr<Audio::AudioDeviceEnumeration> enumerate_wasapi_devices()
{
    ScopedComInitializer com;
    if (!com.initialized())
        return Error::from_windows_error(static_cast<u32>(com.hresult()));

    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr))
        return Error::from_windows_error(static_cast<u32>(hr));

    auto inputs = TRY(enumerate_flow(enumerator.Get(), eCapture, "wasapi:input:"sv, "Microphone"sv));
    auto outputs = TRY(enumerate_flow(enumerator.Get(), eRender, "wasapi:output:"sv, "Speaker"sv));
    return Audio::AudioDeviceEnumeration { move(inputs), move(outputs) };
}

class WasapiNotificationClient final : public IMMNotificationClient {
public:
    explicit WasapiNotificationClient(Function<void()> on_change)
        : m_on_change(move(on_change))
    {
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv_object) override
    {
        if (!ppv_object)
            return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
            *ppv_object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv_object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override
    {
        return ++m_ref_count;
    }

    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = --m_ref_count;
        if (count == 0)
            delete this;
        return count;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override
    {
        if (m_on_change)
            m_on_change();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override
    {
        if (m_on_change)
            m_on_change();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override
    {
        if (m_on_change)
            m_on_change();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override
    {
        if (m_on_change)
            m_on_change();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, PROPERTYKEY const) override
    {
        if (m_on_change)
            m_on_change();
        return S_OK;
    }

private:
    ~WasapiNotificationClient() = default;

    Atomic<ULONG> m_ref_count { 1 };
    Function<void()> m_on_change;
};

class AudioDeviceWatch {
public:
    void watch(AudioDeviceListCallback callback)
    {
        m_callback = move(callback);
        {
            std::lock_guard locker(m_mutex);
            m_event_loop_weak = Core::EventLoop::current_weak();
            m_has_callback = static_cast<bool>(m_callback);
            ensure_registered_locked();
        }

        auto initial = enumerate_wasapi_devices();
        if (m_callback)
            m_callback(move(initial));
    }

private:
    void ensure_registered_locked()
    {
        if (m_registered)
            return;

        if (!m_com)
            m_com = make<ScopedComInitializer>();
        if (!m_com->initialized())
            return;

        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))))
            return;

        ComPtr<WasapiNotificationClient> client;
        client.Attach(new WasapiNotificationClient([this] {
            schedule_refresh();
        }));

        if (FAILED(enumerator->RegisterEndpointNotificationCallback(client.Get())))
            return;

        m_enumerator = move(enumerator);
        m_client = move(client);
        m_registered = true;
    }

    void schedule_refresh()
    {
        if (m_refresh_pending.exchange(true))
            return;

        RefPtr<Core::WeakEventLoopReference> event_loop_weak;
        {
            std::lock_guard locker(m_mutex);
            if (!m_event_loop_weak || !m_has_callback) {
                m_refresh_pending.store(false);
                return;
            }
            event_loop_weak = m_event_loop_weak;
        }

        auto event_loop = event_loop_weak->take();
        if (!event_loop.is_alive()) {
            m_refresh_pending.store(false);
            return;
        }

        event_loop->deferred_invoke([this] {
            m_refresh_pending.store(false);
            auto devices = enumerate_wasapi_devices();
            if (!m_callback)
                return;
            m_callback(move(devices));
        });
    }

    std::mutex m_mutex;
    OwnPtr<ScopedComInitializer> m_com;
    ComPtr<IMMDeviceEnumerator> m_enumerator;
    ComPtr<WasapiNotificationClient> m_client;
    RefPtr<Core::WeakEventLoopReference> m_event_loop_weak;
    AudioDeviceListCallback m_callback;
    Atomic<bool> m_refresh_pending { false };
    bool m_has_callback { false };
    bool m_registered { false };
};

static NeverDestroyed<AudioDeviceWatch> s_device_watch;

}

void watch_platform_audio_devices(AudioDeviceListCallback callback)
{
    if (!callback)
        return;

    if (!Core::EventLoop::is_running()) {
        callback(enumerate_wasapi_devices());
        return;
    }

    s_device_watch->watch(move(callback));
}

}
