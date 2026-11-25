/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <LibAudio/AudioDevices.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibGC/RootVector.h>
#ifdef AK_OS_WINDOWS
#    include <LibMediaCodecs/WindowsMediaFoundation/WindowsMediaFoundationCamera.h>
#endif
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/MediaCapture/MediaDeviceInfo.h>
#include <LibWeb/MediaCapture/MediaDevices.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/PermissionsAPI/Permissions.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapture {

void MediaDevices::PendingEnumerateDevicesRequest::visit_edges(GC::Cell::Visitor& visitor) const
{
    visitor.visit(promise);
}

void MediaDevices::PendingGetUserMediaRequest::visit_edges(GC::Cell::Visitor& visitor) const
{
    visitor.visit(promise);
}

static Utf16FlyString const& AUDIO_INPUT_KIND = *new Utf16FlyString("audioinput"_utf16_fly_string);
static Utf16FlyString const& AUDIO_OUTPUT_KIND = *new Utf16FlyString("audiooutput"_utf16_fly_string);
static Utf16FlyString const& VIDEO_INPUT_KIND = *new Utf16FlyString("videoinput"_utf16_fly_string);

using ConstrainDOMString = Variant<Utf16String, Vector<Utf16String>, ConstrainDOMStringParameters>;

static Optional<DeviceIdConstraint> extract_device_id_constraint(Optional<ConstrainDOMString> const& device_id_value);

GC_DEFINE_ALLOCATOR(MediaDevices);

static void resolve_media_stream_promise(WebIDL::Promise& promise, GC::Ref<MediaStream> stream)
{
    auto& realm = HTML::relevant_realm(*promise.promise());
    WebIDL::resolve_promise(promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, stream));
}

static void resolve_media_device_info_list_promise(WebIDL::Promise& promise, GC::RootVector<GC::Ref<MediaDeviceInfo>> const& result_list)
{
    auto& realm = HTML::relevant_realm(*promise.promise());
    GC::Ref<JS::Array> array = MUST(JS::Array::create(realm, result_list.size()));
    for (size_t index = 0; index < result_list.size(); ++index) {
        JS::PropertyKey property_index { index };
        if (array->create_data_property(property_index, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, result_list[index])).is_error()) {
            WebIDL::reject_promise(promise, WebIDL::OperationError::create(realm, "Failed to build enumerateDevices result"_utf16));
            return;
        }
    }
    WebIDL::resolve_promise(promise, array);
}

GC::Ref<MediaDevices> MediaDevices::create(HTML::Window& window)
{
    auto media_devices = GC::Heap::the().allocate<MediaDevices>(window);
    media_devices->initialize_pending_request_state_change_listener();
    return media_devices;
}

MediaDevices::MediaDevices(HTML::Window& window)
    : DOM::EventTarget()
    , m_window(window)
{
    m_audio_device_cache_listener_id = Audio::AudioDevices::the().add_devices_changed_listener([this] {
        did_observe_audio_device_cache_update();
    });

    did_observe_audio_device_cache_update();
}

void MediaDevices::initialize_pending_request_state_change_listener()
{
    auto& realm = m_window->principal_realm();
    auto pending_request_state_change_callback_function = JS::NativeFunction::create(realm, GC::create_function(GC::Heap::the(), GC::captures(*this), [](auto& media_devices, JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
        media_devices.process_pending_enumerate_devices_requests();
        media_devices.process_pending_get_user_media_requests();
        return JS::js_undefined(); }), 0, Utf16FlyString {}, &realm);

    auto pending_request_state_change_callback = GC::Heap::the().allocate<WebIDL::CallbackType>(*pending_request_state_change_callback_function, realm);
    m_pending_request_state_change_listener = DOM::IDLEventListener::create(pending_request_state_change_callback);

    m_window->associated_document().add_event_listener_without_options(HTML::EventNames::visibilitychange, *m_pending_request_state_change_listener);
    m_window->add_event_listener_without_options(HTML::EventNames::focus, *m_pending_request_state_change_listener);
    m_window->add_event_listener_without_options(HTML::EventNames::blur, *m_pending_request_state_change_listener);
}

void MediaDevices::finalize()
{
    if (m_pending_request_state_change_listener) {
        auto& window = *m_window;
        window.associated_document().remove_event_listener_without_options(HTML::EventNames::visibilitychange, *m_pending_request_state_change_listener);
        window.remove_event_listener_without_options(HTML::EventNames::focus, *m_pending_request_state_change_listener);
        window.remove_event_listener_without_options(HTML::EventNames::blur, *m_pending_request_state_change_listener);
    }

    Base::finalize();
    if (m_audio_device_cache_listener_id.has_value())
        Audio::AudioDevices::the().remove_devices_changed_listener(m_audio_device_cache_listener_id.release_value());
}

void MediaDevices::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
    for (auto const& request : m_pending_enumerate_devices_requests)
        visitor.visit(request.promise);
    for (auto const& request : m_pending_get_user_media_requests)
        visitor.visit(request.promise);
    visitor.visit(m_pending_request_state_change_listener);
    visitor.visit(m_awaiting_fresh_audio_device_list);
}

MediaTrackSupportedConstraints MediaDevices::get_supported_constraints()
{
    // Returns a dictionary whose members are the constrainable properties known to the User Agent.
    return {};
}

// https://w3c.github.io/mediacapture-main/#device-information-exposure
bool MediaDevices::device_information_can_be_exposed()
{
    // 1. If camera information can be exposed on mediaDevices, return true.
    if (camera_information_can_be_exposed())
        return true;
    // 2. If microphone information can be exposed on mediaDevices, return true.
    if (microphone_information_can_be_exposed())
        return true;
    // 3. Return false.
    return false;
}

bool MediaDevices::microphone_information_can_be_exposed()
{
    // 1. If any of the local devices of kind "audioinput" are attached to a live MediaStreamTrack in
    // mediaDevices's relevant global object's associated Document, return true.
    if (has_live_device_of_kind(AUDIO_INPUT_KIND))
        return true;
    // 2. Return mediaDevices.[[canExposeMicrophoneInfo]].
    if (m_can_expose_microphone_info)
        return true;
    return PermissionsAPI::permission_state(Web::Permissions::PermissionName::Microphone, HTML::relevant_settings_object(*m_window)) == PermissionsAPI::PermissionState::Granted;
}

bool MediaDevices::camera_information_can_be_exposed() const
{
    if (has_live_device_of_kind(VIDEO_INPUT_KIND) || m_can_expose_camera_info)
        return true;
    return PermissionsAPI::permission_state(Web::Permissions::PermissionName::Camera, HTML::relevant_settings_object(*m_window)) == PermissionsAPI::PermissionState::Granted;
}

bool MediaDevices::can_use_microphone_feature() const
{
    auto const& document = m_window->associated_document();
    return document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Microphone);
}

bool MediaDevices::can_use_camera_feature() const
{
    auto const& document = m_window->associated_document();
    return document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Camera);
}

bool MediaDevices::device_enumeration_can_proceed()
{
    // 1. The User Agent MAY return true if device information can be exposed on mediaDevices.
    if (device_information_can_be_exposed())
        return true;

    // 2. Return the result of is in view with mediaDevices.
    return is_in_view();
}

// https://w3c.github.io/mediacapture-main/#dfn-has-system-focus
void MediaDevices::system_focus_changed()
{
    // 11.7 While hasSystemFocus is false, the User Agent MUST wait to proceed to the next step until a task queued to set
    //      hasSystemFocus to the result of the has system focus algorithm, would set hasSystemFocus to true.
    HTML::queue_global_task(HTML::Task::Source::UserInteraction, HTML::relevant_global_object(*m_window),
        GC::create_function(heap(), GC::captures(GC::Ref(*this)), [](auto& media_devices) {
            media_devices->process_pending_enumerate_devices_requests();
            media_devices->process_pending_get_user_media_requests();
        }));
}

bool MediaDevices::get_user_media_can_proceed() const
{
    return is_in_view() && m_window->associated_document().has_focus();
}

bool MediaDevices::is_in_view() const
{
    auto const& document = m_window->associated_document();
    return document.is_fully_active() && document.visibility_state() == HTML::VisibilityState::Visible;
}

bool MediaDevices::has_live_device_of_kind(Utf16FlyString const& kind) const
{
    for (auto const& device : m_stored_device_list) {
        if (device.kind != kind)
            continue;

        auto live = m_devices_live_map.get(device.dom_device_id);
        if (live.value_or(false))
            return true;
    }

    return false;
}

// https://w3c.github.io/mediacapture-main/#set-device-information-exposure-0
void MediaDevices::set_device_information_exposure(bool audio_requested, bool video_requested, bool value)
{
    auto& settings_object = HTML::relevant_settings_object(*m_window);

    // 1. If "video" is in requestedTypes, run the following sub-steps.
    if (video_requested) {
        // 1.1 Set mediaDevices.[[canExposeCameraInfo]] to value.
        m_can_expose_camera_info = value;

        // 1.2 If value is true and if device exposure can be extended with "microphone", set mediaDevices.[[canExposeMicrophoneInfo]] to true.
        if (value && PermissionsAPI::permission_state(Web::Permissions::PermissionName::Microphone, settings_object) == PermissionsAPI::PermissionState::Granted)
            m_can_expose_microphone_info = true;
    }

    // 2. If "audio" is in requestedTypes, run the following sub-steps.
    if (audio_requested) {
        // 2.1 Set mediaDevices.[[canExposeMicrophoneInfo]] to value.
        m_can_expose_microphone_info = value;

        // 2.2 If value is true and if device exposure can be extended with "camera", set mediaDevices.[[canExposeCameraInfo]] to true.
        if (value && PermissionsAPI::permission_state(Web::Permissions::PermissionName::Camera, settings_object) == PermissionsAPI::PermissionState::Granted)
            m_can_expose_camera_info = true;
    }
    process_pending_enumerate_devices_requests();
    process_pending_get_user_media_requests();
}

void MediaDevices::enumerate_devices(GC::Ref<WebIDL::Promise> promise)
{
    // 2. Let proceed be the result of device enumeration can proceed with this.
    if (!device_enumeration_can_proceed()) {
        m_pending_enumerate_devices_requests.append({ promise });
        return;
    }

    if (!Audio::AudioDevices::the().has_device_list()) {
        m_pending_enumerate_devices_requests.append({ promise });
        return;
    }

    m_stored_device_list = current_device_snapshot();

    // 3. Let mediaDevices be this.
    // 4. Run the following steps in parallel.
    queue_enumerate_devices_task(promise);
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-enumeratedevices
GC::RootVector<GC::Ref<MediaDeviceInfo>> MediaDevices::create_list_of_device_info_objects(Vector<StoredDevice> const& device_list)
{
    // To perform creating a list of device info objects, given mediaDevices and deviceList, run the following steps:

    // 1. Let resultList be an empty list.
    GC::RootVector<GC::Ref<MediaDeviceInfo>> result_list;

    // 2. Let microphoneList, cameraList and otherDeviceList be empty lists.
    GC::RootVector<GC::Ref<MediaDeviceInfo>> microphone_list;
    GC::RootVector<GC::Ref<MediaDeviceInfo>> camera_list;
    GC::RootVector<GC::Ref<MediaDeviceInfo>> other_device_list;

    // 3. Let document be mediaDevices's relevant global object's associated Document.
    auto const& document = m_window->associated_document();
    (void)document;

    auto create_device_info_object = [&](StoredDevice const& device) -> Optional<GC::Ref<MediaDeviceInfo>> {
        Optional<MediaDeviceKind> kind;
        if (device.kind == AUDIO_INPUT_KIND)
            kind = MediaDeviceKind::Audioinput;
        else if (device.kind == AUDIO_OUTPUT_KIND)
            kind = MediaDeviceKind::Audiooutput;
        else if (device.kind == VIDEO_INPUT_KIND)
            kind = MediaDeviceKind::Videoinput;
        else
            return {};

        auto device_id = device.dom_device_id;
        auto label = device.label;
        auto group_id = device.group_id;

        if ((device.kind == AUDIO_INPUT_KIND && !microphone_information_can_be_exposed())
            || (device.kind == VIDEO_INPUT_KIND && !camera_information_can_be_exposed())) {
            device_id = {};
            label = {};
            group_id = {};
        }

        return MediaDeviceInfo::create(move(device_id), *kind, move(label), move(group_id));
    };

    // 4. Run the following sub steps for each discovered device in deviceList, device.
    for (auto const& device : device_list) {
        // 4.1 If device is not a microphone, or document is not allowed to use the feature identified by microphone, abort these sub steps and continue with the next device.
        if (device.kind != AUDIO_INPUT_KIND)
            continue;
        if (!can_use_microphone_feature())
            continue;

        // 4.2 Let deviceInfo be the result of creating a device info object to represent device, with mediaDevices.
        Optional<GC::Ref<MediaDeviceInfo>> device_info = create_device_info_object(device);
        if (!device_info.has_value())
            continue;

        // 4.3 If device is the system default microphone, prepend deviceInfo to microphoneList. Otherwise, append deviceInfo to microphoneList.
        if (device.is_default)
            microphone_list.insert(0, *device_info);
        else
            microphone_list.append(*device_info);
    }

    // 5. Run the following sub steps for each discovered device in deviceList, device.
    for (auto const& device : device_list) {
        // 5.1 If device is not a camera, or document is not allowed to use the feature identified by camera, abort these sub steps and continue with the next device.
        if (device.kind != VIDEO_INPUT_KIND)
            continue;
        if (!can_use_camera_feature())
            continue;

        // 5.2 Let deviceInfo be the result of creating a device info object to represent device, with mediaDevices.
        Optional<GC::Ref<MediaDeviceInfo>> device_info = create_device_info_object(device);
        if (!device_info.has_value())
            continue;

        // 5.3 If device is the system default camera, prepend deviceInfo to cameraList. Otherwise, append deviceInfo to cameraList.
        if (device.is_default)
            camera_list.insert(0, *device_info);
        else
            camera_list.append(*device_info);
    }

    // 6. If microphone information can be exposed on mediaDevices is false, truncate microphoneList to its first item.
    if (!microphone_information_can_be_exposed()) {
        while (microphone_list.size() > 1)
            microphone_list.take_last();
    }

    // 7. If camera information can be exposed on mediaDevices is false, truncate cameraList to its first item.
    if (!camera_information_can_be_exposed()) {
        while (camera_list.size() > 1)
            camera_list.take_last();
    }

    // 8. Run the following sub steps for each discovered device in deviceList, device.
    for (auto const& device : device_list) {
        // 8.1 If device is a microphone or device is a camera, abort these sub steps and continue with the next device.
        if (device.kind == AUDIO_INPUT_KIND || device.kind == VIDEO_INPUT_KIND)
            continue;

        // 8.2 Run the exposure decision algorithm for devices other than camera and microphone.
        bool expose_other_device = device_information_can_be_exposed();
        // FIXME: Implement the exact exposure decision algorithm from the relevant output-device spec.
        if (!expose_other_device)
            continue;

        // 8.3 Let deviceInfo be the result of creating a device info object to represent device, with mediaDevices.
        Optional<GC::Ref<MediaDeviceInfo>> device_info = create_device_info_object(device);
        if (!device_info.has_value())
            continue;

        // 8.4 Append deviceInfo to otherDeviceList.
        // 8.5 If device is the system default audio output, prepend it instead.
        if (device.kind == AUDIO_OUTPUT_KIND && device.is_default) {
            // AD-HOC: 8.5.x Label & id in the LibMedia audio device cache are only mutated in response
            // to OS notifications. The device manager updates the DOM id & label in the event of changes,
            // and these will be broadcast to the LibMedia cache. We don't set that state here.
            other_device_list.insert(0, *device_info);
        } else {
            other_device_list.append(*device_info);
        }
    }

    // 9. Append to resultList all devices of microphoneList in order.
    result_list.extend(microphone_list);

    // 10. Append to resultList all devices of cameraList in order.
    result_list.extend(camera_list);

    // 11. Append to resultList all devices of otherDeviceList in order.
    result_list.extend(other_device_list);

    // 12. Return resultList.
    return result_list;
}

static GC::Ref<WebIDL::DOMException> device_access_failure(Error const& error, bool is_video = false)
{
    // AD-HOC: The Browser refuses a capture only once the permission, or the system's consent for the browser, is gone,
    //         which is a failure of permission rather than of the device.
    if (error.is_errno() && error.code() == EPERM)
        return WebIDL::NotAllowedError::create("Permission denied"_utf16);
    if (error.is_errno() && error.code() == EIO)
        return WebIDL::NotReadableError::create(is_video ? "The video input device could not be opened"_utf16 : "The audio input device could not be opened"_utf16);
    return WebIDL::AbortError::create(is_video ? "The video input device could not be accessed"_utf16 : "The audio input device could not be accessed"_utf16);
}

static Vector<VideoCameraCandidate> enumerate_available_cameras()
{
    Vector<VideoCameraCandidate> cameras;
#ifdef AK_OS_WINDOWS
    for (auto& device : Media::WindowsMediaFoundation::enumerate_video_capture_devices()) {
        cameras.append(VideoCameraCandidate {
            .device_id = move(device.device_id),
            .label = move(device.label),
            .group_id = move(device.group_id),
            .is_default = device.is_default,
        });
    }
#endif
    return cameras;
}

static void extract_ulong_constraint_preference(Optional<Variant<u32, Bindings::ConstrainULongRange>> const& value, u32& target)
{
    if (!value.has_value())
        return;
    if (value->has<u32>()) {
        auto v = value->get<u32>();
        if (v > 0)
            target = v;
        return;
    }
    auto const& range = value->get<Bindings::ConstrainULongRange>();
    if (range.exact.has_value() && *range.exact > 0)
        target = *range.exact;
    else if (range.ideal.has_value() && *range.ideal > 0)
        target = *range.ideal;
    else if (range.max.has_value() && *range.max > 0 && target > *range.max)
        target = *range.max;
}

static void extract_double_constraint_preference(Optional<Variant<double, Bindings::ConstrainDoubleRange>> const& value, u32& target)
{
    if (!value.has_value())
        return;
    double preferred = 0.0;
    if (value->has<double>()) {
        preferred = value->get<double>();
    } else {
        auto const& range = value->get<Bindings::ConstrainDoubleRange>();
        if (range.exact.has_value())
            preferred = *range.exact;
        else if (range.ideal.has_value())
            preferred = *range.ideal;
        else if (range.max.has_value() && target > *range.max)
            preferred = *range.max;
    }
    if (preferred >= 1.0 && preferred <= 120.0)
        target = static_cast<u32>(preferred);
}

template<typename Device, typename GetId, typename IsDefault>
static Vector<Device> filter_devices_by_id_constraint(
    Vector<Device> const& devices,
    DeviceIdConstraint const& constraint,
    GetId&& get_id,
    IsDefault&& is_default)
{
    bool wants_default = constraint.device_ids.contains_slow("default"_utf16);
    bool has_explicit_default = any_of(devices, [&](auto const& device) { return is_default(device); });

    Vector<Device> filtered;
    for (size_t index = 0; index < devices.size(); ++index) {
        auto const& device = devices[index];
        bool matches_id = constraint.device_ids.contains_slow(get_id(device));
        bool matches_default = wants_default && (is_default(device) || (!has_explicit_default && index == 0));
        if (matches_id || matches_default)
            filtered.append(device);
    }
    return filtered;
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-getusermedia
void MediaDevices::queue_get_user_media_task(GC::Ref<WebIDL::Promise> promise, GetUserMediaOptions options)
{
    GC::Ref<MediaDevices> media_devices = *this;

    auto audio_input_devices = options.audio_requested ? Audio::AudioDevices::the().input_devices() : Vector<Audio::AudioDeviceInfo> {};
    auto video_input_devices = options.video_requested ? enumerate_available_cameras() : Vector<VideoCameraCandidate> {};

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(GC::Root(promise), media_devices, move(options), move(audio_input_devices), move(video_input_devices)), [](auto& promise, auto& media_devices, auto& options, auto& audio_input_devices, auto& video_input_devices) {
        auto& realm = WebIDL::promise_realm(*promise);
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        auto reject_not_found = [&](Utf16String const& message) {
            WebIDL::reject_promise(*promise, WebIDL::NotFoundError::create(message));
        };

        auto reject_permission_failure = [&] {
            WebIDL::reject_promise(*promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
        };

        if (options.audio_requested) {
            Vector<Audio::AudioDeviceInfo> audio_candidates = audio_input_devices;
            if (audio_candidates.is_empty()) {
                dbgln("MediaDevices::getUserMedia rejected (NotFoundError): no audio input devices available");
                reject_not_found("No audio input devices available"_utf16);
                return;
            }

            auto device_ids_are_known = any_of(audio_candidates, [](auto const& device) { return !device.dom_device_id.is_empty(); });
            if (options.audio_device_id_constraint.has_value() && options.audio_device_id_constraint->is_required && device_ids_are_known) {
                audio_candidates = filter_devices_by_id_constraint(
                    audio_candidates,
                    *options.audio_device_id_constraint,
                    [](auto const& device) { return Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()); },
                    [](auto const& device) { return device.is_default; });
                if (audio_candidates.is_empty()) {
                    dbgln("MediaDevices::getUserMedia rejected (NotFoundError): requested audio input device not found");
                    reject_not_found("Requested audio input device not found"_utf16);
                    return;
                }
            }

            if (PermissionsAPI::permission_state(Web::Permissions::PermissionName::Microphone) == PermissionsAPI::PermissionState::Denied) {
                dbgln("MediaDevices::getUserMedia rejected (NotAllowedError): microphone permission denied");
                reject_permission_failure();
                return;
            }
        }

        if (options.video_requested) {
            Vector<VideoCameraCandidate> video_candidates = video_input_devices;
            if (video_candidates.is_empty()) {
                dbgln("MediaDevices::getUserMedia rejected (NotFoundError): no video input devices available");
                reject_not_found("No video input devices available"_utf16);
                return;
            }

            if (options.video_device_id_constraint.has_value() && options.video_device_id_constraint->is_required) {
                video_candidates = filter_devices_by_id_constraint(
                    video_candidates,
                    *options.video_device_id_constraint,
                    [](auto const& device) { return Utf16String::from_utf8(device.device_id); },
                    [](auto const& device) { return device.is_default; });
                if (video_candidates.is_empty()) {
                    dbgln("MediaDevices::getUserMedia rejected (NotFoundError): requested video input device not found");
                    reject_not_found("Requested video input device not found"_utf16);
                    return;
                }
            }

            if (PermissionsAPI::permission_state(Web::Permissions::PermissionName::Camera) == PermissionsAPI::PermissionState::Denied) {
                dbgln("MediaDevices::getUserMedia rejected (NotAllowedError): camera permission denied");
                reject_permission_failure();
                return;
            }
        }

        GC::Ref<MediaStream> stream = MediaStream::create();
        auto& settings_object = HTML::relevant_settings_object(*media_devices->m_window);

        auto on_all_permissions_granted = GC::create_function(GC::Heap::the(), GC::captures(promise, media_devices, options, stream), [](auto& promise, auto& media_devices, auto& options, auto& stream) {
            auto start_tracks = GC::create_function(GC::Heap::the(), GC::captures(promise, media_devices, options, stream), [](auto& promise, auto& media_devices, auto& options, auto& stream) {
                auto& realm = WebIDL::promise_realm(*promise);
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                auto& document = media_devices->m_window->associated_document();
                if (!document.is_fully_active()) {
                    dbgln("MediaDevices::getUserMedia rejected (InvalidStateError): document is not fully active");
                    WebIDL::reject_promise(*promise, WebIDL::InvalidStateError::create("Document is not fully active"_utf16));
                    return;
                }
                if (options.audio_requested
                    && PermissionsAPI::permission_state(Web::Permissions::PermissionName::Microphone, HTML::relevant_settings_object(*media_devices->m_window)) != PermissionsAPI::PermissionState::Granted) {
                    dbgln("MediaDevices::getUserMedia rejected (NotAllowedError): microphone permission not granted");
                    WebIDL::reject_promise(*promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
                    return;
                }
                if (options.video_requested
                    && PermissionsAPI::permission_state(Web::Permissions::PermissionName::Camera, HTML::relevant_settings_object(*media_devices->m_window)) != PermissionsAPI::PermissionState::Granted) {
                    dbgln("MediaDevices::getUserMedia rejected (NotAllowedError): camera permission not granted");
                    WebIDL::reject_promise(*promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
                    return;
                }

                Optional<Audio::AudioDeviceInfo> chosen_audio;
                if (options.audio_requested) {
                    auto final_audio_set = Audio::AudioDevices::the().input_devices();
                    if (options.audio_device_id_constraint.has_value()) {
                        auto matching_devices = filter_devices_by_id_constraint(
                            final_audio_set,
                            *options.audio_device_id_constraint,
                            [](auto const& device) { return Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()); },
                            [](auto const& device) { return device.is_default; });
                        if (!matching_devices.is_empty()) {
                            final_audio_set = move(matching_devices);
                        } else if (options.audio_device_id_constraint->is_required) {
                            dbgln("MediaDevices::getUserMedia rejected (NotFoundError): requested final audio input device not found");
                            WebIDL::reject_promise(*promise, WebIDL::NotFoundError::create("Requested audio input device not found"_utf16));
                            return;
                        }
                    }

                    for (auto const& device : final_audio_set) {
                        if (!chosen_audio.has_value())
                            chosen_audio = device;
                        if (!device.is_default)
                            continue;
                        chosen_audio = device;
                        break;
                    }
                    if (!chosen_audio.has_value()) {
                        dbgln("MediaDevices::getUserMedia rejected (NotReadableError): no readable audio input devices available");
                        WebIDL::reject_promise(*promise, WebIDL::NotReadableError::create("No readable audio input devices available"_utf16));
                        return;
                    }
                }

                Optional<VideoCameraCandidate> chosen_video;
                if (options.video_requested) {
                    auto final_video_set = enumerate_available_cameras();
                    if (options.video_device_id_constraint.has_value()) {
                        auto matching_devices = filter_devices_by_id_constraint(
                            final_video_set,
                            *options.video_device_id_constraint,
                            [](auto const& device) { return Utf16String::from_utf8(device.device_id); },
                            [](auto const& device) { return device.is_default; });
                        if (!matching_devices.is_empty()) {
                            final_video_set = move(matching_devices);
                        } else if (options.video_device_id_constraint->is_required) {
                            dbgln("MediaDevices::getUserMedia rejected (NotFoundError): requested final video input device not found");
                            WebIDL::reject_promise(*promise, WebIDL::NotFoundError::create("Requested video input device not found"_utf16));
                            return;
                        }
                    }

                    for (auto const& device : final_video_set) {
                        if (!chosen_video.has_value())
                            chosen_video = device;
                        if (!device.is_default)
                            continue;
                        chosen_video = device;
                        break;
                    }
                    if (!chosen_video.has_value()) {
                        dbgln("MediaDevices::getUserMedia rejected (NotReadableError): no readable video input devices available");
                        WebIDL::reject_promise(*promise, WebIDL::NotReadableError::create("No readable video input devices available"_utf16));
                        return;
                    }
                }

                media_devices->m_stored_device_list = current_device_snapshot();
                media_devices->set_device_information_exposure(options.audio_requested, options.video_requested, true);

                auto open_video_and_finish = GC::create_function(GC::Heap::the(), GC::captures(promise, media_devices, options, stream, chosen_video), [](auto& promise, auto& media_devices, auto& options, auto& stream, auto& chosen_video, GC::Ptr<MediaStreamTrack> audio_track) {
                    if (chosen_video.has_value()) {
                        auto granted_video_id = Utf16String::from_utf8(chosen_video->device_id);
                        GC::Ref<MediaStreamTrack> video_track = MediaStreamTrack::create(
                            MediaStreamTrackKind::Video,
                            Utf16String::from_utf8(chosen_video->label));
                        MediaTrackSettings video_settings;
                        video_settings.device_id = granted_video_id;
                        video_settings.group_id = Utf16String::from_utf8(chosen_video->group_id);
                        video_track->set_settings(move(video_settings));
                        video_track->set_media_devices({}, *media_devices);

                        if (auto video_start = video_track->start_video_capture(chosen_video->device_id, options.requested_video_width, options.requested_video_height, options.requested_video_fps); video_start.is_error()) {
                            if (audio_track)
                                audio_track->stop();
                            dbgln("MediaDevices::getUserMedia rejected (video start failed): {}", video_start.error());
                            WebIDL::reject_promise(*promise, device_access_failure(video_start.error(), true));
                            return;
                        }

                        media_devices->m_devices_live_map.set(granted_video_id, true);
                        media_devices->m_devices_accessible_map.set(granted_video_id, true);
                        stream->add_track(video_track);
                        media_devices->m_media_stream_track_sources.append(video_track);
                    }

                    dbgln("MediaDevices::getUserMedia resolved: audio={} ('{}'), video={} ('{}')",
                        audio_track ? 1 : 0,
                        audio_track ? audio_track->label() : Utf16String {},
                        chosen_video.has_value() ? 1 : 0,
                        chosen_video.has_value() ? chosen_video->label : String {});
                    resolve_media_stream_promise(*promise, stream);
                });

                if (!chosen_audio.has_value()) {
                    open_video_and_finish->function()(nullptr);
                    return;
                }

                Audio::AudioDeviceInfo const& granted_audio = chosen_audio.value();
                auto granted_audio_id = Utf16String::from_utf8_with_replacement_character(granted_audio.dom_device_id.view());
                GC::Ref<MediaStreamTrack> audio_track = MediaStreamTrack::create(
                    MediaStreamTrackKind::Audio,
                    Utf16String::from_utf8_with_replacement_character(granted_audio.label.view()));

                MediaTrackSettings audio_settings;
                audio_settings.device_id = granted_audio_id;
                audio_settings.group_id = Utf16String::from_utf8_with_replacement_character(granted_audio.group_id.view());
                audio_settings.sample_rate = granted_audio.sample_rate_hz;
                audio_settings.channel_count = granted_audio.channel_count;
                audio_track->set_settings(move(audio_settings));
                audio_track->set_media_devices({}, *media_devices);

                audio_track->start_audio_capture(GC::create_function(GC::Heap::the(), GC::captures(promise, media_devices, options, stream, audio_track, move(granted_audio_id), open_video_and_finish), [](auto& promise, auto& media_devices, auto& options, auto& stream, auto& audio_track, auto& granted_audio_id, auto& open_video_and_finish, ErrorOr<void> access) {
                    auto& realm = WebIDL::promise_realm(*promise);
                    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                    if (access.is_error()) {
                        dbgln("MediaDevices::getUserMedia rejected (audio start failed): {}", access.error());
                        WebIDL::reject_promise(*promise, device_access_failure(access.error(), false));
                        return;
                    }

                    if (!media_devices->m_window->associated_document().is_fully_active()) {
                        audio_track->stop();
                        dbgln("MediaDevices::getUserMedia rejected (InvalidStateError): document became inactive while starting audio");
                        WebIDL::reject_promise(*promise, WebIDL::InvalidStateError::create("Document is not fully active"_utf16));
                        return;
                    }

                    if (PermissionsAPI::permission_state(Web::Permissions::PermissionName::Microphone, HTML::relevant_settings_object(*media_devices->m_window)) != PermissionsAPI::PermissionState::Granted
                        || (options.video_requested && PermissionsAPI::permission_state(Web::Permissions::PermissionName::Camera, HTML::relevant_settings_object(*media_devices->m_window)) != PermissionsAPI::PermissionState::Granted)) {
                        audio_track->stop();
                        dbgln("MediaDevices::getUserMedia rejected (NotAllowedError): permission revoked while starting audio");
                        WebIDL::reject_promise(*promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
                        return;
                    }

                    media_devices->m_devices_live_map.set(granted_audio_id, true);
                    media_devices->m_devices_accessible_map.set(granted_audio_id, true);
                    stream->add_track(audio_track);
                    media_devices->m_media_stream_track_sources.append(audio_track);

                    open_video_and_finish->function()(audio_track.ptr());
                }));
            });

            if (options.audio_requested)
                media_devices->run_with_fresh_audio_device_list(start_tracks);
            else
                start_tracks->function()();
        });

        auto check_camera_then_continue = GC::create_function(GC::Heap::the(), GC::captures(promise, media_devices, options, on_all_permissions_granted), [](auto& promise, auto& media_devices, auto& options, auto& on_all_permissions_granted) {
            if (!options.video_requested) {
                on_all_permissions_granted->function()();
                return;
            }
            auto& settings_object = HTML::relevant_settings_object(*media_devices->m_window);
            PermissionsAPI::request_permission_with_user_prompt(Web::Permissions::PermissionName::Camera, settings_object, GC::create_function(GC::Heap::the(), GC::captures(promise, media_devices, on_all_permissions_granted), [](auto& promise, auto& media_devices, auto& on_all_permissions_granted, PermissionsAPI::PermissionState camera_permission_result) {
                auto& realm = WebIDL::promise_realm(*promise);
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                if (!media_devices->m_window->associated_document().is_fully_active()) {
                    dbgln("MediaDevices::getUserMedia rejected (InvalidStateError): document became inactive during camera prompt");
                    WebIDL::reject_promise(*promise, WebIDL::InvalidStateError::create("Document is not fully active"_utf16));
                    return;
                }
                if (camera_permission_result == PermissionsAPI::PermissionState::Denied) {
                    dbgln("MediaDevices::getUserMedia rejected (NotAllowedError): user denied camera prompt");
                    WebIDL::reject_promise(*promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
                    return;
                }
                on_all_permissions_granted->function()();
            }));
        });

        if (options.audio_requested) {
            PermissionsAPI::request_permission_with_user_prompt(Web::Permissions::PermissionName::Microphone, settings_object, GC::create_function(GC::Heap::the(), GC::captures(promise, media_devices, check_camera_then_continue), [](auto& promise, auto& media_devices, auto& check_camera_then_continue, PermissionsAPI::PermissionState microphone_permission_result) {
                auto& realm = WebIDL::promise_realm(*promise);
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                if (!media_devices->m_window->associated_document().is_fully_active()) {
                    dbgln("MediaDevices::getUserMedia rejected (InvalidStateError): document became inactive during microphone prompt");
                    WebIDL::reject_promise(*promise, WebIDL::InvalidStateError::create("Document is not fully active"_utf16));
                    return;
                }
                if (microphone_permission_result == PermissionsAPI::PermissionState::Denied) {
                    dbgln("MediaDevices::getUserMedia rejected (NotAllowedError): user denied microphone prompt");
                    WebIDL::reject_promise(*promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
                    return;
                }
                check_camera_then_continue->function()();
            }));
        } else {
            check_camera_then_continue->function()();
        }
    }));
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-enumeratedevices
void MediaDevices::queue_enumerate_devices_task(GC::Ref<WebIDL::Promise> promise)
{
    GC::Ref<MediaDevices> media_devices = *this;

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(GC::Root(promise), media_devices), [](auto& promise, auto& media_devices) {
        auto& realm = WebIDL::promise_realm(*promise);
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        auto result_list = media_devices->create_list_of_device_info_objects(media_devices->m_stored_device_list);
        size_t audio_in = 0;
        size_t video_in = 0;
        size_t audio_out = 0;
        for (auto const& info : result_list) {
            switch (info->kind()) {
            case Bindings::MediaDeviceKind::Audioinput:
                ++audio_in;
                break;
            case Bindings::MediaDeviceKind::Videoinput:
                ++video_in;
                break;
            case Bindings::MediaDeviceKind::Audiooutput:
                ++audio_out;
                break;
            }
        }
        EnumerateLogSummary summary {
            .total = result_list.size(),
            .audio_in = audio_in,
            .video_in = video_in,
            .audio_out = audio_out,
            .exposed_mic = media_devices->microphone_information_can_be_exposed(),
            .exposed_cam = media_devices->camera_information_can_be_exposed(),
        };
        if (media_devices->m_last_logged_enumerate_summary != summary) {
            media_devices->m_last_logged_enumerate_summary = summary;
            dbgln("MediaDevices::enumerateDevices -> total={} (audioin={}, videoin={}, audioout={}, exposed_mic={}, exposed_cam={})",
                summary.total, summary.audio_in, summary.video_in, summary.audio_out,
                summary.exposed_mic, summary.exposed_cam);
        }
        resolve_media_device_info_list_promise(*promise, result_list);
    }));
}

void MediaDevices::process_pending_enumerate_devices_requests()
{
    if (!device_enumeration_can_proceed())
        return;
    if (!Audio::AudioDevices::the().has_device_list())
        return;

    run_device_change_notification_steps(current_device_snapshot());

    GC::RootVector pending_requests { move(m_pending_enumerate_devices_requests) };
    for (auto& request : pending_requests)
        queue_enumerate_devices_task(request.promise);
}

void MediaDevices::process_pending_get_user_media_requests()
{
    if (!get_user_media_can_proceed())
        return;

    bool has_audio_device_list = Audio::AudioDevices::the().has_device_list();
    Vector<PendingGetUserMediaRequest> remaining;
    GC::RootVector<PendingGetUserMediaRequest> ready;
    for (auto& request : m_pending_get_user_media_requests) {
        if (request.options.audio_requested && !has_audio_device_list)
            remaining.append(move(request));
        else
            ready.append(move(request));
    }
    m_pending_get_user_media_requests = move(remaining);
    for (auto& request : ready)
        queue_get_user_media_task(request.promise, move(request.options));
}

// https://w3c.github.io/mediacapture-main/#dfn-device-change-notification-steps
void MediaDevices::run_device_change_notification_steps(Vector<StoredDevice> const& device_list)
{
    // When new media input and/or output devices are made available to the User Agent, or any available
    // input and/or output device becomes unavailable, or the system default for input and/or output
    // devices of a MediaDeviceKind changed, the User Agent MUST run the following device change
    // notification steps for each MediaDevices object, mediaDevices, for which device enumeration can
    // proceed is true, but for no other MediaDevices object:

    if (!device_enumeration_can_proceed())
        return;

    // 1. Let lastExposedDevices be the result of creating a list of device info objects with mediaDevices and mediaDevices.[[storedDeviceList]].
    GC::RootVector<GC::Ref<MediaDeviceInfo>> last_exposed_devices = create_list_of_device_info_objects(m_stored_device_list);

    // 2. Let deviceList be the list of all media input and/or output devices available to the User Agent.
    // device_list is the caller-provided snapshot.

    // 3. Let newExposedDevices be the result of creating a list of device info objects with mediaDevices and deviceList.
    GC::RootVector<GC::Ref<MediaDeviceInfo>> new_exposed_devices = create_list_of_device_info_objects(device_list);

    // 4. If the MediaDeviceInfo objects in newExposedDevices match those in lastExposedDevices and have
    //    the same order, then abort these steps.
    if (new_exposed_devices.size() == last_exposed_devices.size()) {
        bool exposed_devices_match = true;
        for (size_t i = 0; i < new_exposed_devices.size(); ++i) {
            if (new_exposed_devices[i]->kind() != last_exposed_devices[i]->kind()
                || new_exposed_devices[i]->device_id() != last_exposed_devices[i]->device_id()
                || new_exposed_devices[i]->label() != last_exposed_devices[i]->label()
                || new_exposed_devices[i]->group_id() != last_exposed_devices[i]->group_id()) {
                exposed_devices_match = false;
                break;
            }
        }
        if (exposed_devices_match)
            return;
    }

    // 5. Set mediaDevices.[[storedDeviceList]] to deviceList.
    m_stored_device_list = device_list;

    // 6. Queue a task that fires an event named devicechange, using the DeviceChangeEvent constructor
    //    with devices initialized to newExposedDevices, at mediaDevices.
    // FIXME: Dispatch DeviceChangeEvent with devices once DeviceChangeEvent is implemented in LibWeb.
    auto& relevant_global_object = HTML::relevant_global_object(*m_window);
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation,
        relevant_global_object,
        GC::create_function(GC::Heap::the(), GC::captures(GC::Ref(*this), GC::Ref(relevant_global_object)), [](auto& media_devices, auto& relevant_global_object) {
            media_devices->dispatch_event(DOM::Event::create(HTML::EventNames::devicechange,
                HighResolutionTime::current_high_resolution_time(*relevant_global_object)));
        }));
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-getusermedia
void MediaDevices::get_user_media(Optional<MediaStreamConstraints> const& constraints, GC::Ref<WebIDL::Promise> promise)
{
    GetUserMediaOptions options;

    // 1. Let constraints be the method's first argument.
    if (!constraints.has_value()) {
        auto& realm = WebIDL::promise_realm(promise);
        WebIDL::reject_promise(promise, JS::TypeError::create(realm, "getUserMedia requires constraints"sv));
        return;
    }

    // 2. Let requestedMediaTypes be the set of media types in constraints with either a dictionary value or a value of true.
    auto const& constraints_value = constraints.value();
    auto const& audio_value = constraints_value.audio;
    if (audio_value.has<bool>()) {
        options.audio_requested = audio_value.get<bool>();
    } else {
        options.audio_requested = true;
        auto const& audio_constraints = audio_value.get<MediaTrackConstraints>();
        options.audio_device_id_constraint = extract_device_id_constraint(audio_constraints.device_id);
    }

    auto const& video_value = constraints_value.video;
    if (video_value.has<bool>()) {
        options.video_requested = video_value.get<bool>();
    } else {
        options.video_requested = true;
        auto const& video_constraints = video_value.get<MediaTrackConstraints>();
        options.video_device_id_constraint = extract_device_id_constraint(video_constraints.device_id);
        extract_ulong_constraint_preference(video_constraints.width, options.requested_video_width);
        extract_ulong_constraint_preference(video_constraints.height, options.requested_video_height);
        extract_double_constraint_preference(video_constraints.frame_rate, options.requested_video_fps);
    }

    // 3. If requestedMediaTypes is the empty set, return a promise rejected with a TypeError.
    if (!options.audio_requested && !options.video_requested) {
        auto& realm = WebIDL::promise_realm(promise);
        WebIDL::reject_promise(promise, JS::TypeError::create(realm, "No media types requested"sv));
        return;
    }

    // 4. Let document be the relevant global object's associated Document.
    auto const& document = m_window->associated_document();

    // 5. If document is NOT fully active, return a promise rejected with an InvalidStateError.
    if (!document.is_fully_active()) {
        WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create("Document is not fully active"_utf16));
        return;
    }

    // 6. If requestedMediaTypes contains "audio" and document is not allowed to use the feature identified by the "microphone" permission name, jump to Permission Failure.
    if (options.audio_requested && !can_use_microphone_feature()) {
        WebIDL::reject_promise(promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
        return;
    }

    // 7. If requestedMediaTypes contains "video" and document is not allowed to use the feature identified by the "camera" permission name, jump to Permission Failure.
    if (options.video_requested && !can_use_camera_feature()) {
        WebIDL::reject_promise(promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
        return;
    }

    // 8. Let mediaDevices be this.
    // 9. Let isInView be the result of the is in view algorithm.

    // 10. Let p be a new promise.
#ifndef AK_OS_WINDOWS
    if (options.video_requested) {
        WebIDL::reject_promise(promise, WebIDL::NotSupportedError::create("Video capture is not supported"_utf16));
        return;
    }
#endif

    // 11. Run the following steps in parallel.
    // AD-HOC: Keep the request queued until isInView and hasSystemFocus allow the deferred task
    // to continue without spinning in the event loop.
    if (!get_user_media_can_proceed()) {
        dbgln("MediaDevices::getUserMedia queued (waiting on view/focus): audio={}, video={}", options.audio_requested, options.video_requested);
        m_pending_get_user_media_requests.append({ .promise = promise, .options = move(options) });
        return;
    }

    if (options.audio_requested && !Audio::AudioDevices::the().has_device_list()) {
        dbgln("MediaDevices::getUserMedia queued (waiting on audio device list): audio={}, video={}", options.audio_requested, options.video_requested);
        m_pending_get_user_media_requests.append({ .promise = promise, .options = move(options) });
        return;
    }

    queue_get_user_media_task(promise, move(options));
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-ondevicechange
WebIDL::CallbackType* MediaDevices::ondevicechange()
{
    return event_handler_attribute(HTML::EventNames::devicechange);
}

void MediaDevices::set_ondevicechange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::devicechange, event_handler);
}

Vector<MediaDevices::StoredDevice> MediaDevices::current_device_snapshot()
{
    Vector<StoredDevice> stored_devices;
    auto input_devices = Audio::AudioDevices::the().input_devices();
    auto output_devices = Audio::AudioDevices::the().output_devices();
    auto video_devices = enumerate_available_cameras();
    stored_devices.ensure_capacity(input_devices.size() + output_devices.size() + video_devices.size());

    for (auto const& device : input_devices) {
        stored_devices.append(StoredDevice {
            .dom_device_id = Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()),
            .kind = AUDIO_INPUT_KIND,
            .label = Utf16String::from_utf8_with_replacement_character(device.label.view()),
            .group_id = Utf16String::from_utf8_with_replacement_character(device.group_id.view()),
            .is_default = device.is_default,
        });
    }

    for (auto const& device : video_devices) {
        stored_devices.append(StoredDevice {
            .dom_device_id = Utf16String::from_utf8(device.device_id),
            .kind = VIDEO_INPUT_KIND,
            .label = Utf16String::from_utf8(device.label),
            .group_id = Utf16String::from_utf8(device.group_id),
            .is_default = device.is_default,
        });
    }

    for (auto const& device : output_devices) {
        stored_devices.append(StoredDevice {
            .dom_device_id = Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()),
            .kind = AUDIO_OUTPUT_KIND,
            .label = Utf16String::from_utf8_with_replacement_character(device.label.view()),
            .group_id = Utf16String::from_utf8_with_replacement_character(device.group_id.view()),
            .is_default = device.is_default,
        });
    }

    return stored_devices;
}

void MediaDevices::did_observe_audio_device_cache_update()
{
    end_tracks_of_removed_devices();
    process_pending_enumerate_devices_requests();
    process_pending_get_user_media_requests();

    GC::RootVector awaiting { move(m_awaiting_fresh_audio_device_list) };
    for (auto const& callback : awaiting)
        callback->function()();
}

void MediaDevices::run_with_fresh_audio_device_list(GC::Ref<GC::Function<void()>> callback)
{
    if (!Audio::AudioDevices::the().request_fresh_device_list()) {
        callback->function()();
        return;
    }
    m_awaiting_fresh_audio_device_list.append(callback);
}

// https://w3c.github.io/mediacapture-main/#track-ended
void MediaDevices::end_tracks_of_removed_devices()
{
    if (m_media_stream_track_sources.is_empty())
        return;

    bool has_live_audio_track = false;
    bool has_live_video_track = false;
    for (auto const& weak_track : m_media_stream_track_sources) {
        auto track = weak_track.ptr();
        if (!track || track->track_ready_state() == MediaStreamTrackState::Ended)
            continue;
        if (track->is_video())
            has_live_video_track = true;
        else
            has_live_audio_track = true;
    }
    if (!has_live_audio_track && !has_live_video_track) {
        m_media_stream_track_sources.clear();
        return;
    }

    Vector<Utf16String> present_audio_ids;
    if (has_live_audio_track) {
        for (auto const& device : Audio::AudioDevices::the().input_devices())
            present_audio_ids.append(Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()));
    }

    Vector<Utf16String> present_video_ids;
    if (has_live_video_track) {
        for (auto const& device : enumerate_available_cameras())
            present_video_ids.append(Utf16String::from_utf8(device.device_id));
    }

    GC::RootVector<GC::Ref<MediaStreamTrack>> tracks_to_end;
    m_media_stream_track_sources.remove_all_matching([&](GC::Weak<MediaStreamTrack> const& weak_track) {
        auto track = weak_track.ptr();
        if (!track || track->track_ready_state() == MediaStreamTrackState::Ended)
            return true;
        auto device_id = track->device_id();
        auto const& present_ids = track->is_video() ? present_video_ids : present_audio_ids;
        if (!device_id.has_value() || present_ids.contains_slow(device_id.value()))
            return false;
        tracks_to_end.append(*track);
        return true;
    });
    end_tracks(move(tracks_to_end));
}

// https://w3c.github.io/mediacapture-main/#dfn-device-permission-revocation-algorithm
void MediaDevices::end_tracks_without_permission()
{
    auto& settings = HTML::relevant_settings_object(*m_window);
    bool can_mic = PermissionsAPI::permission_state(Web::Permissions::PermissionName::Microphone, settings) == PermissionsAPI::PermissionState::Granted;
    bool can_cam = PermissionsAPI::permission_state(Web::Permissions::PermissionName::Camera, settings) == PermissionsAPI::PermissionState::Granted;
    if (can_mic && can_cam)
        return;

    GC::RootVector<GC::Ref<MediaStreamTrack>> tracks_to_end;
    m_media_stream_track_sources.remove_all_matching([&](GC::Weak<MediaStreamTrack> const& weak_track) {
        auto track = weak_track.ptr();
        if (!track || track->track_ready_state() == MediaStreamTrackState::Ended)
            return true;
        bool permitted = track->is_video() ? can_cam : can_mic;
        if (permitted)
            return false;
        tracks_to_end.append(*track);
        return true;
    });
    end_tracks(move(tracks_to_end));
}

// A clone of a capture track captures from the same device too, so it ends with the permission like its original. A
// clone made once the permission is gone (from an ended listener of another track, e.g.) ends at once.
void MediaDevices::track_was_cloned(Badge<MediaStreamTrack>, MediaStreamTrack& clone)
{
    auto& settings = HTML::relevant_settings_object(*m_window);
    auto permission = clone.is_video() ? Web::Permissions::PermissionName::Camera : Web::Permissions::PermissionName::Microphone;
    if (PermissionsAPI::permission_state(permission, settings) != PermissionsAPI::PermissionState::Granted) {
        GC::RootVector<GC::Ref<MediaStreamTrack>> tracks_to_end;
        tracks_to_end.append(clone);
        end_tracks(move(tracks_to_end));
        return;
    }
    m_media_stream_track_sources.append(clone);
}

void MediaDevices::track_source_ended(Badge<MediaStreamTrack>, MediaStreamTrack& track)
{
    GC::RootVector<GC::Ref<MediaStreamTrack>> tracks_to_end;
    tracks_to_end.append(track);
    end_tracks(move(tracks_to_end));
}

void MediaDevices::end_tracks(GC::RootVector<GC::Ref<MediaStreamTrack>> tracks_to_end)
{
    if (tracks_to_end.is_empty())
        return;

    // When a MediaStreamTrack track ends for any reason other than the stop() method being invoked, the User Agent
    // MUST queue a task that runs the following steps:
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, HTML::relevant_global_object(*m_window),
        GC::create_function(heap(), GC::captures(move(tracks_to_end), m_window), [](auto& tracks, auto& window) {
            HTML::TemporaryExecutionContext execution_context { window->principal_realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            // 1. If the track's [[ReadyState]] slot has the value "ended", abort these steps.
            // 2. Set track's [[ReadyState]] slot to "ended".
            // 3. Notify track's source that track is ended so that the source may be stopped, unless other MediaStreamTrack objects depend on it.
            // 4. Fire an event named ended at the object.
            for (auto const& track : tracks)
                track->end();
        }));
}

// https://w3c.github.io/mediacapture-main/#dfn-stop-all-sources
void MediaDevices::stop_all_sources()
{
    // 1. For each MediaStreamTrack object track whose relevant global object is globalObject, set track's
    //    [[ReadyState]] to "ended".
    // 2. If globalObject is a Window, then for each source in globalObject's associated
    //    MediaDevices.[[mediaStreamTrackSources]], stop source.
    // NB: The tracks stand in for their sources, and stopping a track also ends it.
    // FIXME: End this global object's tracks that no MediaDevices made too.
    for (auto const& weak_track : exchange(m_media_stream_track_sources, {})) {
        if (auto track = weak_track.ptr())
            track->stop();
    }
}

static Optional<Vector<Utf16String>> dom_string_values_from_variant(Variant<Utf16String, Vector<Utf16String>> const& value)
{
    Vector<Utf16String> values;
    if (value.has<Utf16String>()) {
        auto const& string_value = value.get<Utf16String>();
        if (!string_value.is_empty())
            values.append(string_value);
    } else {
        for (auto const& entry : value.get<Vector<Utf16String>>()) {
            if (!entry.is_empty())
                values.append(entry);
        }
    }
    return values.is_empty() ? Optional<Vector<Utf16String>> {} : Optional<Vector<Utf16String>> { move(values) };
}

static Optional<Vector<Utf16String>> extract_dom_string_constraint_values(ConstrainDOMString const& constraint)
{
    // https://w3c.github.io/mediacapture-main/#dom-constraindomstring
    // ConstrainDOMString is (DOMString or sequence<DOMString> or ConstrainDOMStringParameters).
    // https://w3c.github.io/mediacapture-main/#dom-constraindomstringparameters
    // ConstrainDOMStringParameters members:
    // - exact: (DOMString or sequence<DOMString>) The exact required value for this property.
    // - ideal: (DOMString or sequence<DOMString>) The ideal (target) value for this property.
    // https://w3c.github.io/mediacapture-main/#constraint-types
    // List values MUST be interpreted as disjunctions.
    if (constraint.has<ConstrainDOMStringParameters>()) {
        auto const& parameters = constraint.get<ConstrainDOMStringParameters>();
        if (parameters.exact.has_value())
            return dom_string_values_from_variant(*parameters.exact);
        if (parameters.ideal.has_value())
            return dom_string_values_from_variant(*parameters.ideal);
        return {};
    }

    if (constraint.has<Utf16String>())
        return dom_string_values_from_variant(Variant<Utf16String, Vector<Utf16String>> { constraint.get<Utf16String>() });

    return dom_string_values_from_variant(Variant<Utf16String, Vector<Utf16String>> { constraint.get<Vector<Utf16String>>() });
}

static Optional<DeviceIdConstraint> extract_device_id_constraint(Optional<ConstrainDOMString> const& device_id_value)
{
    if (!device_id_value.has_value())
        return {};

    // https://w3c.github.io/mediacapture-main/#dfn-required-constraints
    // A bare value is an ideal one, so only exact makes the device a requirement.
    auto is_required = device_id_value->has<ConstrainDOMStringParameters>() && device_id_value->get<ConstrainDOMStringParameters>().exact.has_value();
    auto device_ids = extract_dom_string_constraint_values(*device_id_value).value_or({});

    // NB: Device ids are never empty, so an exact constraint without any other id is one that no device satisfies, such
    //     as the empty ids that enumerateDevices() exposes before the page may capture.
    if (device_ids.is_empty() && !is_required)
        return {};
    return DeviceIdConstraint { .device_ids = move(device_ids), .is_required = is_required };
}

}
