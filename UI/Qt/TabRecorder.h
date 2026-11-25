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
    static Vector<MicrophoneDeviceInfo> enumerate_microphones();

    bool is_recording() const { return m_is_recording; }
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
    void capture_video_tick();
    void enqueue_video_frame(QImage);
    void run_video_worker();
    void run_audio_worker();
    ErrorOr<void> write_speakers_json(AK::Duration total_duration);

    struct QueuedVideoFrame {
        QImage image;
        AK::Duration timestamp;
    };

    WebContentView& m_view;
    QTimer m_frame_timer;

    bool m_is_recording { false };
    bool m_snapshot_in_flight { false };
    TabRecorderSettings m_active_settings;
    MonotonicTime m_start_time { MonotonicTime::now_coarse() };
    String m_mp4_path;
    String m_speakers_json_path;
    String m_recording_filename;

    OwnPtr<Media::SpeakerTimeline> m_timeline;
#ifdef Q_OS_WIN
    OwnPtr<Media::WindowsMediaFoundation::WindowsMediaFoundationMp4Recorder> m_recorder;
    void* m_stop_event { nullptr };
    void* m_video_event { nullptr };
#endif

    AK::Mutex m_recorder_mutex;
    AK::Mutex m_video_queue_mutex;
    Vector<QueuedVideoFrame> m_video_queue;
    bool m_stop_requested { false };
    Atomic<bool> m_audio_worker_stop { false };

    RefPtr<Threading::Thread> m_video_thread;
    RefPtr<Threading::Thread> m_audio_thread;
};

}
