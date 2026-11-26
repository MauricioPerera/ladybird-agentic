/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Utf16StringBuilder.h>
#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Clipboard/Clipboard.h>
#include <LibWeb/CredentialManagement/CredentialsContainer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Geolocation/Geolocation.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/MediaCapture/MediaDevices.h>
#include <LibWeb/MediaSession/MediaSession.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PermissionsAPI/Permissions.h>
#include <LibWeb/ServiceWorker/ServiceWorkerContainer.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::HTML {

extern "C" {

// Called by Bindings/Rust/src/html/navigator.rs.
Window* web_html_navigator_window(JSObject* navigator_wrapper);
}

GC_DEFINE_ALLOCATOR(Navigator);

GC::Ref<Navigator> Navigator::create(Window& window)
{
    return GC::Heap::the().allocate<Navigator>(window);
}

Navigator::Navigator(Window& window)
    : m_window(window)
{
    NavigatorGamepadPartial::check_for_connected_gamepads();
}

Navigator::~Navigator() = default;

EnvironmentSettingsObject& Navigator::navigator_storage_settings_object() const
{
    return relevant_settings_object(*m_window);
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-pdfviewerenabled
bool Navigator::pdf_viewer_enabled() const
{
    // The NavigatorPlugins mixin's pdfViewerEnabled getter steps are to return the user agent's PDF viewer supported.
    // NOTE: The NavigatorPlugins mixin should only be exposed on the Window object.
    return m_window->page().pdf_viewer_supported();
}

// https://w3c.github.io/webdriver/#dfn-webdriver
bool Navigator::webdriver() const
{
    // Returns true if webdriver-active flag is set, false otherwise.

    // NOTE: The NavigatorAutomationInformation interface should not be exposed on WorkerNavigator.
    return m_window->page().is_webdriver_active();
}

void Navigator::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
    visitor.visit(m_battery_promise);
    visitor.visit(m_mime_type_array);
    visitor.visit(m_plugin_array);
    visitor.visit(m_clipboard);
    visitor.visit(m_geolocation);
    visitor.visit(m_user_activation);
    visitor.visit(m_service_worker_container);
    visitor.visit(m_media_capabilities);
    visitor.visit(m_media_session);
    visitor.visit(m_media_devices);
    visitor.visit(m_credentials);
    visitor.visit(m_permissions);
}

GC::Ref<MimeTypeArray> Navigator::mime_types()
{
    if (!m_mime_type_array)
        m_mime_type_array = MimeTypeArray::create(*m_window);
    return *m_mime_type_array;
}

GC::Ref<PluginArray> Navigator::plugins()
{
    if (!m_plugin_array)
        m_plugin_array = PluginArray::create(*m_window);
    return *m_plugin_array;
}

GC::Ref<Clipboard::Clipboard> Navigator::clipboard()
{
    if (!m_clipboard)
        m_clipboard = Clipboard::Clipboard::create(relevant_realm(*m_window));
    return *m_clipboard;
}

GC::Ref<Geolocation::Geolocation> Navigator::geolocation()
{
    if (!m_geolocation)
        m_geolocation = Geolocation::Geolocation::create(*m_window);
    return *m_geolocation;
}

GC::Ref<UserActivation> Navigator::user_activation()
{
    if (!m_user_activation)
        m_user_activation = UserActivation::create(*m_window);
    return *m_user_activation;
}

GC::Ref<CredentialManagement::CredentialsContainer> Navigator::credentials()
{
    if (!m_credentials)
        m_credentials = CredentialManagement::CredentialsContainer::create();
    return *m_credentials;
}

// https://w3c.github.io/pointerevents/#dom-navigator-maxtouchpoints
WebIDL::Long Navigator::max_touch_points()
{
    // FIXME: Implement this for touch-capable devices.
    return 0;
}

GC::Ref<ServiceWorker::ServiceWorkerContainer> Navigator::service_worker()
{
    if (!m_service_worker_container)
        m_service_worker_container = ServiceWorker::ServiceWorkerContainer::create(relevant_settings_object(*m_window));
    return *m_service_worker_container;
}

GC::Ref<MediaCapabilitiesAPI::MediaCapabilities> Navigator::media_capabilities()
{
    if (!m_media_capabilities)
        m_media_capabilities = MediaCapabilitiesAPI::MediaCapabilities::create();
    return *m_media_capabilities;
}

// https://w3c.github.io/mediasession/#dom-navigator-mediasession
GC::Ref<MediaSession::MediaSession> Navigator::media_session()
{
    // The mediaSession attribute MUST return the MediaSession instance associated with the Navigator object.
    if (!m_media_session)
        m_media_session = MediaSession::MediaSession::create(*m_window);
    return *m_media_session;
}

GC::Ref<MediaCapture::MediaDevices> Navigator::media_devices()
{
    if (!m_media_devices)
        m_media_devices = MediaCapture::MediaDevices::create(*m_window);
    return *m_media_devices;
}

// https://w3c.github.io/mediacapture-main/#dom-navigator-getusermedia
void Navigator::webkit_get_user_media(MediaCapture::MediaStreamConstraints const& constraints, GC::Ref<WebIDL::CallbackType> success_callback, GC::Ref<WebIDL::CallbackType> error_callback)
{
    // 1. Let constraints be the method's first argument.
    // 2. Let successCallback be the callback indicated by the method's second argument.
    // 3. Let errorCallback be the callback indicated by the method's third argument.

    // 4. Run the steps specified by the getUserMedia() algorithm with constraints as the argument, and let p be the
    //    resulting promise.
    auto promise = WebIDL::create_promise(relevant_realm(*m_window));
    media_devices()->get_user_media(constraints, promise);

    // 5. Upon fulfillment of p with value stream, run the following step:
    auto on_fulfilled = GC::create_function<WebIDL::ExceptionOr<JS::Value>(JS::Value)>(GC::Heap::the(), GC::captures(success_callback), [](WebIDL::CallbackType& success_callback, JS::Value stream) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Invoke successCallback with stream as the argument.
        (void)WebIDL::invoke_callback(success_callback, {}, WebIDL::ExceptionBehavior::Report, { { stream } });
        return JS::js_undefined();
    });

    // 6. Upon rejection of p with reason r, run the following step:
    auto on_rejected = GC::create_function<WebIDL::ExceptionOr<JS::Value>(JS::Value)>(GC::Heap::the(), GC::captures(error_callback), [](WebIDL::CallbackType& error_callback, JS::Value reason) -> WebIDL::ExceptionOr<JS::Value> {
        // 1. Invoke errorCallback with r as the argument.
        (void)WebIDL::invoke_callback(error_callback, {}, WebIDL::ExceptionBehavior::Report, { { reason } });
        return JS::js_undefined();
    });

    WebIDL::react_to_promise(promise, on_fulfilled, on_rejected);
}

// https://w3c.github.io/battery/#the-getbattery-method
GC::Ref<WebIDL::Promise> Navigator::get_battery()
{
    // 1. If this.[[BatteryPromise]] is null, then set it to a new promise in this's relevant realm.
    if (!m_battery_promise) {
        m_battery_promise = WebIDL::create_promise_for(*m_window);
        start_get_battery_steps(*m_battery_promise);
    }

    // 4. Return this.[[BatteryPromise]].
    return *m_battery_promise;
}

void Navigator::start_get_battery_steps(GC::Ref<WebIDL::Promise> promise)
{
    // 2. If this's relevant global object's associated Document is not allowed to use the "battery" policy-controlled
    //    feature, then reject this.[[BatteryPromise]] with a "NotAllowedError" DOMException.
    if (true) {
        WebIDL::reject_promise(promise, WebIDL::NotAllowedError::create("Battery Status API is not yet implemented"_utf16));
    }
    // 3. Otherwise:
    else {
        // FIXME: 1. If this.[[BatteryManager]] is null, then set it to the result of creating a new BatteryManager in this's
        //           relevant realm.
        // FIXME: 2. Resolve this.[[BatteryPromise]] with this.[[BatteryManager]].
    }
}

GC::Ref<PermissionsAPI::Permissions> Navigator::permissions()
{
    if (!m_permissions)
        m_permissions = PermissionsAPI::Permissions::create();
    return *m_permissions;
}

// https://html.spec.whatwg.org/multipage/system-state.html#normalize-protocol-handler-parameters
static WebIDL::ExceptionOr<void> normalize_protocol_handler_parameters(Utf16String const& scheme, Utf16String const& url, EnvironmentSettingsObject& environment)
{
    // 1. Set scheme to scheme converted to ASCII lowercase.
    auto normalized_scheme = scheme.to_ascii_lowercase();
    auto scheme_view = normalized_scheme.utf16_view();

    // 2. If scheme is neither a safelisted scheme nor a string starting with "web+" followed by one or more ASCII lower alphas,
    //    then throw a "SecurityError" DOMException.
    static constexpr Array safelisted_schemes = {
        u"bitcoin"sv, u"ftp"sv, u"ftps"sv, u"geo"sv, u"im"sv, u"irc"sv, u"ircs"sv, u"magnet"sv,
        u"mailto"sv, u"matrix"sv, u"mms"sv, u"news"sv, u"nntp"sv, u"openpgp4fpr"sv, u"sftp"sv,
        u"sip"sv, u"sms"sv, u"smsto"sv, u"ssh"sv, u"tel"sv, u"urn"sv, u"webcal"sv, u"wtai"sv, u"xmpp"sv
    };
    bool is_safelisted = false;
    for (auto const& candidate : safelisted_schemes) {
        if (scheme_view == candidate) {
            is_safelisted = true;
            break;
        }
    }
    if (!is_safelisted) {
        bool is_valid_custom_scheme = scheme_view.starts_with(u"web+"sv) && scheme_view.length_in_code_units() > 4;
        if (is_valid_custom_scheme) {
            for (size_t i = 4; i < scheme_view.length_in_code_units(); ++i) {
                if (!is_ascii_lower_alpha(scheme_view.code_unit_at(i))) {
                    is_valid_custom_scheme = false;
                    break;
                }
            }
        }
        if (!is_valid_custom_scheme)
            return WebIDL::SecurityError::create(Utf16String::formatted("The scheme '{}' is not permitted for protocol handlers", scheme));
    }

    // 3. If url does not contain "%s", then throw a "SyntaxError" DOMException.
    auto url_view = url.utf16_view();
    auto placeholder_offset = url_view.find_code_unit_offset(u"%s"sv);
    if (!placeholder_offset.has_value())
        return WebIDL::SyntaxError::create("The URL argument must contain '%s'"_utf16);

    // 4-6. Replace first "%s" with "dummy", encoding-parse relative to environment, and verify HTTP(S) + same origin.
    Utf16StringBuilder replaced_url_builder;
    replaced_url_builder.append(url_view.substring_view(0, *placeholder_offset));
    replaced_url_builder.append(u"dummy"sv);
    replaced_url_builder.append(url_view.substring_view(*placeholder_offset + 2));
    auto replaced_url = replaced_url_builder.to_string();

    auto parsed_url = environment.encoding_parse_url(replaced_url.utf16_view());
    if (!parsed_url.has_value())
        return WebIDL::SyntaxError::create(Utf16String::formatted("The URL '{}' is invalid", url));

    if ((parsed_url->scheme() != "http"sv && parsed_url->scheme() != "https"sv)
        || !parsed_url->origin().is_same_origin(environment.origin())) {
        return WebIDL::SecurityError::create("Protocol handler URL must be same-origin and use http or https"_utf16);
    }

    return {};
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-registerprotocolhandler
WebIDL::ExceptionOr<void> Navigator::register_protocol_handler(Utf16String const& scheme, Utf16String const& url)
{
    TRY(normalize_protocol_handler_parameters(scheme, url, relevant_settings_object(*m_window)));
    return {};
}

// https://html.spec.whatwg.org/multipage/system-state.html#dom-navigator-unregisterprotocolhandler
WebIDL::ExceptionOr<void> Navigator::unregister_protocol_handler(Utf16String const& scheme, Utf16String const& url)
{
    TRY(normalize_protocol_handler_parameters(scheme, url, relevant_settings_object(*m_window)));
    return {};
}

Window* web_html_navigator_window(JSObject* navigator_wrapper)
{
    return &Bindings::impl_from<Navigator>(JS::HostABI::object_from_abi(navigator_wrapper))->window();
}

}
