/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibMediaCodecs/Export.h>

namespace Media {

class MEDIACODECS_API SpeakerTimeline {
public:
    AK_ALLOC_WITH_KMALLOC;

    struct SpeakerSummary {
        String label;
        bool is_local { false };
        double total_speaking_sec { 0.0 };
        size_t segment_count { 0 };
    };

    struct SpeakerSegment {
        AK::Duration start;
        AK::Duration end;
        String speaker;
        bool is_local { false };
    };

    void record_webrtc_activity(u64 speaker_key, bool is_local, bool is_speaking, float level, AK::Duration timestamp);
    void record_fallback_audio_activity(bool is_local, bool is_speaking, float level, AK::Duration timestamp);
    void finalize(AK::Duration total_duration);
    NonnullOwnPtr<SpeakerTimeline> create_continuation() const;

    Vector<SpeakerSegment> const& segments() const { return m_segments; }
    Vector<SpeakerSummary> const& speakers() const { return m_speakers; }

    String to_json(StringView recording_filename, AK::Duration total_duration) const;
    static String format_timestamp_hms(AK::Duration);

private:
    struct InternalKey {
        bool is_local { false };
        bool is_fallback { false };
        u64 id { 0 };

        bool operator==(InternalKey const& other) const = default;
    };

    struct ActiveTurn {
        String label;
        bool is_local { false };
        AK::Duration start;
    };

    void update_turn_locked(InternalKey const&, String const& label, bool is_speaking, AK::Duration timestamp);
    void close_turn_locked(InternalKey const&, AK::Duration timestamp);

    mutable Mutex m_mutex;
    bool m_finalized { false };
    bool m_saw_webrtc_local { false };
    bool m_saw_webrtc_remote { false };
    u32 m_next_remote_index { 1 };
    HashMap<u64, String> m_remote_labels;
    Vector<ActiveTurn> m_active_turns;
    Vector<InternalKey> m_active_keys;
    Vector<SpeakerSegment> m_raw_segments;
    Vector<SpeakerSegment> m_segments;
    Vector<SpeakerSummary> m_speakers;
};

}
