/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibHTTP/Header.h>
#include <LibHTTP/HeaderList.h>
#include <LibURL/URL.h>

namespace RequestServer {

struct NetworkInterceptRule {
    enum class Action : u8 {
        Block,
        Fulfill,
        ModifyHeaders,
    };

    String id;
    String url_pattern;
    Action action { Action::Block };
    Vector<String> methods {};
    u32 status_code { 200 };
    String content_type { "text/plain; charset=utf-8"_string };
    ByteBuffer body {};
    Vector<HTTP::Header> headers {};
};

class NetworkInterceptStore {
public:
    static constexpr size_t MAX_RULES = 128;
    static constexpr size_t MAX_BODY_BYTES = 2 * 1024 * 1024;

    static NetworkInterceptStore& the();

    ErrorOr<void> set_rules_from_json(StringView json);
    void clear();

    static bool url_matches_pattern(StringView url, StringView pattern);
    Optional<NetworkInterceptRule> match(URL::URL const& url, StringView method) const;
    void apply_header_modifications(URL::URL const& url, StringView method, HTTP::HeaderList& headers) const;

private:
    Vector<NetworkInterceptRule> m_rules;
};

}
