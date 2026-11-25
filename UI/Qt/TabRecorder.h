/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QTimer>

namespace Media {
class SpeakerTimeline;
}

#ifdef Q_OS_WIN
namespace Media::WindowsMediaFoundation {
class WindowsMediaFoundationMp4Recorder;
}
#endif

namespace Threading {
class Thread;
}

namespace Ladybird {

class WebContentView;

struct MicrophoneDeviceInfo {
    String device_id;
    String label;
    bool is_default { false };
};

struct TabRecorderSettings {
    bool record_microphone { true };
    bool record_system_audio { true };
    String microphone_device_id;
    bool show_cursor { true };
    bool export_speaker_timeline { true };
    u32 batch_duration_seconds { 300 };
    u32 fps { 30 };
};

class TabRecorder final : public QObject {
    Q_OBJECT

public:
    AK_ALLOC_WITH_KMALLOC;

    struct RecordingResult {
        String mp4_path;
        String speakers_json_path;
        AK::Duration duration;
    };

    explicit TabRecorder(WebContentView& view, QObject* parent = nullptr);
    virtual ~TabRecorder() override;

    static TabRecorderSettings& settings();
    static void load_settings();
    static void save_settings();
    static String settings_file_path();
    static Vector<MicrophoneDeviceInfo> enumerate_microphones();

    bool is_recording() const { return m_is_recording; }
    bool is_batched() const { return !m_session_dir.is_empty(); }
    u32 current_batch_index() const { return m_current_batch_index.load(AK::MemoryOrder::memory_order_acquire); }
    AK::Duration elapsed() const;
    static String recordings_directory();

    ErrorOr<void> start(StringView page_title = {});
    ErrorOr<RecordingResult> stop();

    void notify_webrtc_speaker_activity(u64 speaker_key, bool is_local, bool is_speaking, float level);

signals:
    void recording_started(QString mp4_path);
    void recording_stopped(QString mp4_path, QString speakers_json_path);
    void recording_failed(QString error_message);

private:
    struct QueuedVideoFrame {
        QImage image;
        AK::Duration timestamp;
    };

    struct CompletedBatch {
        u32 part_index { 0 };
        String mp4_filename;
        String speakers_json_filename;
        AK::Duration session_start;
        AK::Duration duration;
    };

    void capture_video_tick();
    void enqueue_video_frame(QImage);
    void run_video_worker(u32 width, u32 height, u32 fps, bool record_audio);
    void run_audio_worker();

    String batch_mp4_filename(u32 part_index) const;
    String batch_mp4_path(u32 part_index) const;
    String batch_speakers_json_filename(u32 part_index) const;
    String batch_speakers_json_path(u32 part_index) const;
    ErrorOr<void> write_speakers_json_file(Media::SpeakerTimeline&, StringView mp4_filename, StringView json_path, AK::Duration batch_duration);
    ErrorOr<void> write_session_manifest(bool completed, AK::Duration total_duration);

    WebContentView& m_view;
    QTimer m_frame_timer;

    bool m_is_recording { false };
    bool m_snapshot_in_flight { false };
    TabRecorderSettings m_active_settings;
    MonotonicTime m_start_time { MonotonicTime::now_coarse() };
    String m_session_dir;
    String m_session_id;
    String m_mp4_path;
    String m_speakers_json_path;
    String m_recording_filename;

    AK::Mutex m_timeline_mutex;
    OwnPtr<Media::SpeakerTimeline> m_timeline;
    AK::Duration m_timeline_batch_start_offset { AK::Duration::zero() };

#ifdef Q_OS_WIN
    OwnPtr<Media::WindowsMediaFoundation::WindowsMediaFoundationMp4Recorder> m_recorder;
    AK::Duration m_recorder_batch_start_offset { AK::Duration::zero() };
    Optional<Error> m_recorder_init_error;
    Optional<Error> m_recorder_finalize_error;
    void* m_stop_event { nullptr };
    void* m_video_event { nullptr };
    void* m_recorder_init_event { nullptr };
    bool m_recorder_has_audio { false };
#endif

    AK::Mutex m_recorder_mutex;
    AK::Mutex m_video_queue_mutex;
    Vector<QueuedVideoFrame> m_video_queue;
    bool m_stop_requested { false };
    Atomic<bool> m_audio_worker_stop { false };
    Atomic<u32> m_current_batch_index { 0 };
    Vector<CompletedBatch> m_completed_batches;

    RefPtr<Threading::Thread> m_video_thread;
    RefPtr<Threading::Thread> m_audio_thread;
};

}
