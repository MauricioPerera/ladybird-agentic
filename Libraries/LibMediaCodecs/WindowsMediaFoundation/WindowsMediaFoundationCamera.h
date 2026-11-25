/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibMediaCodecs/Export.h>

namespace Media::WindowsMediaFoundation {

struct VideoCaptureDeviceInfo {
    String device_id;
    String label;
    String group_id;
    bool is_default { false };
};

MEDIACODECS_API Vector<VideoCaptureDeviceInfo> enumerate_video_capture_devices();

class MEDIACODECS_API VideoCaptureStream final : public AtomicRefCounted<VideoCaptureStream> {
    AK_MAKE_NONCOPYABLE(VideoCaptureStream);
    AK_MAKE_NONMOVABLE(VideoCaptureStream);

public:
    AK_ALLOC_WITH_KMALLOC;

    using FrameCallback = Function<void(Gfx::DecodedImageFrame)>;
    using ErrorCallback = Function<void()>;

    static ErrorOr<NonnullRefPtr<VideoCaptureStream>> create(
        StringView device_id,
        u32 requested_width,
        u32 requested_height,
        u32 requested_fps,
        FrameCallback on_frame,
        ErrorCallback on_ended = {});

    ~VideoCaptureStream();

    void stop();

    u32 width() const;
    u32 height() const;
    double frame_rate() const;
    bool is_running() const;

private:
    struct WorkerState;
    struct Impl;

    explicit VideoCaptureStream(NonnullOwnPtr<Impl>);

    NonnullOwnPtr<Impl> m_impl;
};

}
