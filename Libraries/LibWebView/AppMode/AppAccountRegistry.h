/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API AppAccountRecord {
    String id;
    String label;
    String profile_name;
    i64 created_at_ms { 0 };
    i64 last_used_at_ms { 0 };
};

class WEBVIEW_API AppAccountRegistry {
public:
    static String default_label_for_account(StringView account_id);
    static ErrorOr<Vector<AppAccountRecord>> list(ByteString const& registry_path);
    static ErrorOr<Optional<AppAccountRecord>> find(ByteString const& registry_path, StringView account_id);
    static ErrorOr<AppAccountRecord> upsert(
        ByteString const& registry_path,
        StringView app_id,
        StringView account_id,
        Optional<StringView> label = {},
        bool touch_last_used = true);
};

}
