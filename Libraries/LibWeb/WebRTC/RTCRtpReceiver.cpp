/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibGC/Heap.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebRTC/RTCPeerConnection.h>
#include <LibWeb/WebRTC/RTCRtpReceiver.h>
#include <LibWeb/WebRTC/RTCRtpScriptTransform.h>
#include <LibWeb/WebRTC/RTCSFrameReceiverTransform.h>

namespace Web::WebRTC {

GC_DEFINE_ALLOCATOR(RTCRtpReceiver);

GC::Ref<RTCRtpReceiver> RTCRtpReceiver::create(GC::Ref<RTCPeerConnection> connection, Bindings::MediaStreamTrackKind kind)
{
    auto track = MediaCapture::MediaStreamTrack::create(kind);
    return GC::Heap::the().allocate<RTCRtpReceiver>(connection, track);
}

RTCRtpReceiver::RTCRtpReceiver(GC::Ref<RTCPeerConnection> connection, GC::Ref<MediaCapture::MediaStreamTrack> track)
    : m_connection(connection)
    , m_track(track)
{
}

RTCRtpReceiver::~RTCRtpReceiver() = default;

static RTCRtpCodec make_rtp_codec(Utf16String mime_type, u32 clock_rate, Optional<u16> channels = {}, Optional<Utf16String> sdp_fmtp_line = {})
{
    RTCRtpCodec codec;
    codec.channels = channels;
    codec.clock_rate = clock_rate;
    codec.mime_type = move(mime_type);
    codec.sdp_fmtp_line = move(sdp_fmtp_line);
    return codec;
}

static RTCRtpHeaderExtensionCapability make_header_extension_capability(Utf16String uri)
{
    RTCRtpHeaderExtensionCapability ext;
    ext.uri = move(uri);
    return ext;
}

// https://w3c.github.io/webrtc-pc/#dom-rtcrtpreceiver-getcapabilities
Optional<RTCRtpCapabilities> RTCRtpReceiver::get_capabilities(Utf16View kind)
{
    if (kind == "audio"sv) {
        RTCRtpCapabilities capabilities;
        capabilities.codecs.append(make_rtp_codec("audio/opus"_utf16, 48000, 2, "minptime=10;useinbandfec=1"_utf16));
        capabilities.codecs.append(make_rtp_codec("audio/G722"_utf16, 8000));
        capabilities.codecs.append(make_rtp_codec("audio/PCMU"_utf16, 8000));
        capabilities.codecs.append(make_rtp_codec("audio/PCMA"_utf16, 8000));
        capabilities.codecs.append(make_rtp_codec("audio/CN"_utf16, 8000));
        capabilities.codecs.append(make_rtp_codec("audio/telephone-event"_utf16, 48000));
        capabilities.codecs.append(make_rtp_codec("audio/telephone-event"_utf16, 8000));
        capabilities.header_extensions.append(make_header_extension_capability("urn:ietf:params:rtp-hdrext:sdes:mid"_utf16));
        capabilities.header_extensions.append(make_header_extension_capability("urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"_utf16));
        capabilities.header_extensions.append(make_header_extension_capability("urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id"_utf16));
        capabilities.header_extensions.append(make_header_extension_capability("urn:ietf:params:rtp-hdrext:ssrc-audio-level"_utf16));
        return capabilities;
    }

    if (kind == "video"sv) {
        RTCRtpCapabilities capabilities;
        capabilities.codecs.append(make_rtp_codec("video/VP8"_utf16, 90000));
        capabilities.codecs.append(make_rtp_codec("video/VP9"_utf16, 90000, {}, "profile-id=0"_utf16));
        capabilities.codecs.append(make_rtp_codec("video/H264"_utf16, 90000, {}, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f"_utf16));
        capabilities.codecs.append(make_rtp_codec("video/H264"_utf16, 90000, {}, "level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42e01f"_utf16));
        capabilities.codecs.append(make_rtp_codec("video/AV1"_utf16, 90000));
        capabilities.codecs.append(make_rtp_codec("video/rtx"_utf16, 90000));
        capabilities.codecs.append(make_rtp_codec("video/red"_utf16, 90000));
        capabilities.codecs.append(make_rtp_codec("video/ulpfec"_utf16, 90000));
        capabilities.header_extensions.append(make_header_extension_capability("urn:ietf:params:rtp-hdrext:sdes:mid"_utf16));
        capabilities.header_extensions.append(make_header_extension_capability("urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id"_utf16));
        capabilities.header_extensions.append(make_header_extension_capability("urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id"_utf16));
        return capabilities;
    }

    return {};
}

// https://w3c.github.io/webrtc-pc/#dom-rtcrtpreceiver-getparameters
RTCRtpReceiveParameters RTCRtpReceiver::get_parameters() const
{
    RTCRtpReceiveParameters result;
    if (m_track->track_kind() == Bindings::MediaStreamTrackKind::Audio) {
        RTCRtpCodecParameters opus;
        opus.channels = 2;
        opus.clock_rate = 48000;
        opus.mime_type = "audio/opus"_utf16;
        opus.sdp_fmtp_line = "minptime=10;useinbandfec=1"_utf16;
        opus.payload_type = 111;
        result.codecs.append(move(opus));
    }
    return result;
}

// https://w3c.github.io/webrtc-pc/#dom-rtcrtpreceiver-getstats
GC::Ref<WebIDL::Promise> RTCRtpReceiver::get_stats() const
{
    return m_connection->get_stats(nullptr);
}

GC::Ref<MediaCapture::MediaStreamTrack> RTCRtpReceiver::track() const { return m_track; }

void RTCRtpReceiver::set_associated_remote_streams(Vector<GC::Ref<MediaCapture::MediaStream>>&& streams)
{
    m_associated_remote_streams = move(streams);
}

void RTCRtpReceiver::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_connection);
    visitor.visit(m_track);
    visitor.visit(m_transform);
    for (auto& stream : m_associated_remote_streams)
        visitor.visit(stream);
}

RTCRtpReceiverTransform RTCRtpReceiver::transform() const
{
    if (!m_transform)
        return Empty {};
    if (auto* script_transform = as_if<RTCRtpScriptTransform>(*m_transform))
        return GC::Ref { *script_transform };
    if (auto* sframe_transform = as_if<RTCSFrameReceiverTransform>(*m_transform))
        return GC::Ref { *sframe_transform };
    return Empty {};
}

// https://w3c.github.io/webrtc-encoded-transform/#dom-rtcrtpreceiver-transform
WebIDL::ExceptionOr<void> RTCRtpReceiver::set_transform(RTCRtpReceiverTransform value)
{
    // NB: A script transform belongs to the first sender or receiver it's set on, even once it's replaced there.
    auto* new_script_transform = value.get_pointer<GC::Ref<RTCRtpScriptTransform>>();
    if (new_script_transform && (*new_script_transform)->has_owner())
        return WebIDL::InvalidStateError::create("The transform is already in use"_utf16);
    if (new_script_transform)
        (*new_script_transform)->set_has_owner();

    if (auto* old_script_transform = m_transform ? as_if<RTCRtpScriptTransform>(*m_transform) : nullptr)
        old_script_transform->detach();
    m_transform = value.visit(
        [](Empty) -> GC::Ptr<Bindings::Wrappable> { return nullptr; },
        [](GC::Ref<RTCSFrameReceiverTransform> const& t) -> GC::Ptr<Bindings::Wrappable> { return t.ptr(); },
        [](GC::Ref<RTCRtpScriptTransform> const& t) -> GC::Ptr<Bindings::Wrappable> { return t.ptr(); });
    dbgln_if(WEBRTC_DEBUG, "RTCRtpReceiver::set_transform: transform set={}", m_transform != nullptr);
    return {};
}

}
