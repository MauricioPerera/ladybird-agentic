/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Math.h>
#include <AK/QuickSort.h>
#include <LibMediaCodecs/SpeakerTimeline.h>

namespace Media {

namespace {

constexpr auto MERGE_GAP_THRESHOLD = AK::Duration::from_milliseconds(500);
constexpr auto MIN_SEGMENT_DURATION = AK::Duration::from_milliseconds(150);

double round_to_millis_seconds(AK::Duration d)
{
    i64 ms = max<i64>(0, d.to_milliseconds());
    return static_cast<double>(ms) / 1000.0;
}

}

String SpeakerTimeline::format_timestamp_hms(AK::Duration d)
{
    i64 total_ms = max<i64>(0, d.to_milliseconds());
    i64 hours = total_ms / 3'600'000;
    i64 minutes = (total_ms / 60'000) % 60;
    i64 seconds = (total_ms / 1'000) % 60;
    i64 millis = total_ms % 1'000;
    return MUST(String::formatted("{:02}:{:02}:{:02}.{:03}", hours, minutes, seconds, millis));
}

void SpeakerTimeline::close_turn_locked(InternalKey const& key, AK::Duration timestamp)
{
    for (size_t i = 0; i < m_active_keys.size(); ++i) {
        if (m_active_keys[i] != key)
            continue;
        auto turn = move(m_active_turns[i]);
        m_active_keys.remove(i);
        m_active_turns.remove(i);
        auto end = timestamp > turn.start ? timestamp : turn.start;
        if (end > turn.start) {
            m_raw_segments.append(SpeakerSegment {
                .start = turn.start,
                .end = end,
                .speaker = move(turn.label),
                .is_local = turn.is_local,
            });
        }
        return;
    }
}

void SpeakerTimeline::update_turn_locked(InternalKey const& key, String const& label, bool is_speaking, AK::Duration timestamp)
{
    for (size_t i = 0; i < m_active_keys.size(); ++i) {
        if (m_active_keys[i] == key) {
            if (!is_speaking)
                close_turn_locked(key, timestamp);
            return;
        }
    }

    if (!is_speaking)
        return;

    m_active_keys.append(key);
    m_active_turns.append(ActiveTurn {
        .label = label,
        .is_local = key.is_local,
        .start = max(AK::Duration::zero(), timestamp),
    });
}

void SpeakerTimeline::record_webrtc_activity(u64 speaker_key, bool is_local, bool is_speaking, float level, AK::Duration timestamp)
{
    (void)level;
    MutexLocker locker(m_mutex);
    if (m_finalized)
        return;

    if (is_local) {
        if (!m_saw_webrtc_local) {
            m_saw_webrtc_local = true;
            close_turn_locked(InternalKey { .is_local = true, .is_fallback = true, .id = 0 }, timestamp);
        }
        update_turn_locked(InternalKey { .is_local = true, .is_fallback = false, .id = 0 }, "Yo"_string, is_speaking, timestamp);
        return;
    }

    if (!m_saw_webrtc_remote) {
        m_saw_webrtc_remote = true;
        close_turn_locked(InternalKey { .is_local = false, .is_fallback = true, .id = 0 }, timestamp);
    }

    auto existing = m_remote_labels.get(speaker_key);
    if (!existing.has_value()) {
        if (!is_speaking)
            return;
        auto label = MUST(String::formatted("Asistente {}", m_next_remote_index++));
        m_remote_labels.set(speaker_key, label);
        update_turn_locked(InternalKey { .is_local = false, .is_fallback = false, .id = speaker_key }, label, true, timestamp);
        return;
    }

    update_turn_locked(InternalKey { .is_local = false, .is_fallback = false, .id = speaker_key }, *existing, is_speaking, timestamp);
}

void SpeakerTimeline::record_fallback_audio_activity(bool is_local, bool is_speaking, float level, AK::Duration timestamp)
{
    (void)level;
    MutexLocker locker(m_mutex);
    if (m_finalized)
        return;

    if (is_local) {
        if (m_saw_webrtc_local)
            return;
        update_turn_locked(InternalKey { .is_local = true, .is_fallback = true, .id = 0 }, "Yo"_string, is_speaking, timestamp);
        return;
    }

    if (m_saw_webrtc_remote)
        return;
    update_turn_locked(InternalKey { .is_local = false, .is_fallback = true, .id = 0 }, "Asistente 1"_string, is_speaking, timestamp);
}

void SpeakerTimeline::finalize(AK::Duration total_duration)
{
    MutexLocker locker(m_mutex);
    if (m_finalized)
        return;
    m_finalized = true;

    for (size_t i = 0; i < m_active_turns.size(); ++i) {
        auto const& turn = m_active_turns[i];
        auto end = total_duration > turn.start ? total_duration : turn.start;
        if (end > turn.start) {
            m_raw_segments.append(SpeakerSegment {
                .start = turn.start,
                .end = end,
                .speaker = turn.label,
                .is_local = turn.is_local,
            });
        }
    }
    m_active_turns.clear();
    m_active_keys.clear();

    m_segments.clear();
    m_speakers.clear();
    if (m_raw_segments.is_empty())
        return;

    quick_sort(m_raw_segments, [](SpeakerSegment const& a, SpeakerSegment const& b) {
        if (a.start != b.start)
            return a.start < b.start;
        return a.end < b.end;
    });

    // Filter out micro-blips (< 150 ms) before coalescing so isolated clicks don't pull neighboring turns earlier.
    Vector<SpeakerSegment> filtered_raw;
    for (auto const& seg : m_raw_segments) {
        if (seg.end - seg.start >= MIN_SEGMENT_DURATION)
            filtered_raw.append(seg);
    }
    auto const& source_segments = !filtered_raw.is_empty() ? filtered_raw : m_raw_segments;

    // Coalesce short pauses (< 500 ms) per speaker.
    Vector<SpeakerSegment> merged;
    for (auto const& seg : source_segments) {
        bool coalesced = false;
        for (size_t i = merged.size(); i > 0; --i) {
            auto& prev = merged[i - 1];
            if (prev.speaker == seg.speaker && prev.is_local == seg.is_local) {
                if (seg.start <= prev.end + MERGE_GAP_THRESHOLD) {
                    if (seg.end > prev.end)
                        prev.end = seg.end;
                    coalesced = true;
                }
                break;
            }
        }
        if (!coalesced)
            merged.append(seg);
    }

    m_segments = move(merged);

    quick_sort(m_segments, [](SpeakerSegment const& a, SpeakerSegment const& b) {
        if (a.start != b.start)
            return a.start < b.start;
        return a.end < b.end;
    });

    for (auto const& seg : m_segments) {
        double seg_sec = round_to_millis_seconds(seg.end - seg.start);
        bool updated = false;
        for (auto& summary : m_speakers) {
            if (summary.label == seg.speaker && summary.is_local == seg.is_local) {
                summary.total_speaking_sec += seg_sec;
                summary.segment_count += 1;
                updated = true;
                break;
            }
        }
        if (!updated) {
            m_speakers.append(SpeakerSummary {
                .label = seg.speaker,
                .is_local = seg.is_local,
                .total_speaking_sec = seg_sec,
                .segment_count = 1,
            });
        }
    }
}

String SpeakerTimeline::to_json(StringView recording_filename, AK::Duration total_duration) const
{
    MutexLocker locker(m_mutex);

    JsonObject root;
    root.set("recording"sv, recording_filename);
    root.set("duration_sec"sv, round_to_millis_seconds(total_duration));

    JsonArray speakers_array;
    for (auto const& speaker : m_speakers) {
        JsonObject entry;
        entry.set("label"sv, speaker.label);
        entry.set("is_local"sv, speaker.is_local);
        entry.set("total_speaking_sec"sv, AK::round(speaker.total_speaking_sec * 1000.0) / 1000.0);
        entry.set("segment_count"sv, static_cast<u64>(speaker.segment_count));
        speakers_array.must_append(move(entry));
    }
    root.set("speakers"sv, move(speakers_array));

    JsonArray segments_array;
    for (auto const& seg : m_segments) {
        JsonObject entry;
        entry.set("start_sec"sv, round_to_millis_seconds(seg.start));
        entry.set("end_sec"sv, round_to_millis_seconds(seg.end));
        entry.set("start_time"sv, format_timestamp_hms(seg.start));
        entry.set("end_time"sv, format_timestamp_hms(seg.end));
        entry.set("speaker"sv, seg.speaker);
        entry.set("is_local"sv, seg.is_local);
        segments_array.must_append(move(entry));
    }
    root.set("segments"sv, move(segments_array));

    return root.serialized();
}

}
