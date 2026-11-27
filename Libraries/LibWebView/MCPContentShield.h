/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct MCPTextSanitizeResult {
    String text;
    size_t stripped_invisible_chars { 0 };
    size_t stripped_html_comments { 0 };
};

WEBVIEW_API MCPTextSanitizeResult sanitize_mcp_untrusted_text(StringView text, bool strip_html_comments = false);
WEBVIEW_API Vector<String> detect_mcp_prompt_injection_signals(StringView text, size_t stripped_invisible_chars = 0, size_t stripped_html_comments = 0);
WEBVIEW_API String generate_mcp_boundary_nonce();
WEBVIEW_API JsonObject build_mcp_security_envelope(StringView nonce, size_t stripped_invisible_chars, size_t stripped_html_comments, Vector<String> const& risk_signals);
WEBVIEW_API JsonValue sanitize_mcp_json_value(JsonValue const& value, size_t& stripped_invisible_chars, StringBuilder& signal_scan_corpus);

}
