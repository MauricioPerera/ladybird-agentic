/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <RequestServer/NetworkIntercept.h>

namespace RequestServer {

NetworkInterceptStore& NetworkInterceptStore::the()
{
    static NetworkInterceptStore s_store;
    return s_store;
}

void NetworkInterceptStore::clear()
{
    m_rules.clear();
}

static bool wildcard_match(StringView text, StringView pattern)
{
    size_t ti = 0;
    size_t pi = 0;
    Optional<size_t> star_pi;
    size_t match_ti = 0;

    while (ti < text.length()) {
        if (pi < pattern.length() && pattern[pi] == text[ti]) {
            ++ti;
            ++pi;
        } else if (pi < pattern.length() && pattern[pi] == '*') {
            star_pi = pi++;
            match_ti = ti;
        } else if (star_pi.has_value()) {
            pi = *star_pi + 1;
            ti = ++match_ti;
        } else {
            return false;
        }
    }

    while (pi < pattern.length() && pattern[pi] == '*')
        ++pi;

    return pi == pattern.length();
}

bool NetworkInterceptStore::url_matches_pattern(StringView url, StringView pattern)
{
    if (pattern.is_empty())
        return false;
    if (pattern == "*"sv)
        return true;
    if (pattern.contains('*'))
        return wildcard_match(url, pattern);
    return url.contains(pattern);
}

static bool method_matches(Vector<String> const& methods, StringView method)
{
    if (methods.is_empty())
        return true;
    for (auto const& allowed : methods) {
        if (allowed.bytes_as_string_view().equals_ignoring_ascii_case(method))
            return true;
    }
    return false;
}

ErrorOr<void> NetworkInterceptStore::set_rules_from_json(StringView json)
{
    if (json.trim_whitespace().is_empty()) {
        m_rules.clear();
        return {};
    }

    auto parsed = TRY(JsonValue::from_string(json));
    if (!parsed.is_object())
        return Error::from_string_literal("Network intercept rules payload must be a JSON object");

    auto rules_array = parsed.as_object().get_array("rules"sv);
    if (!rules_array.has_value()) {
        m_rules.clear();
        return {};
    }

    Vector<NetworkInterceptRule> next_rules;
    for (auto const& value : rules_array->values()) {
        if (next_rules.size() >= MAX_RULES)
            break;
        if (!value.is_object())
            continue;
        auto const& obj = value.as_object();

        auto id = obj.get_string("id"sv).value_or("rule"_string);
        auto url_pattern = obj.get_string("url_pattern"sv);
        auto action_str = obj.get_string("action"sv);
        if (!url_pattern.has_value() || url_pattern->is_empty() || !action_str.has_value())
            continue;

        NetworkInterceptRule rule;
        rule.id = move(id);
        rule.url_pattern = move(*url_pattern);

        if (*action_str == "block"sv) {
            rule.action = NetworkInterceptRule::Action::Block;
        } else if (*action_str == "fulfill"sv) {
            rule.action = NetworkInterceptRule::Action::Fulfill;
        } else if (*action_str == "modify_headers"sv) {
            rule.action = NetworkInterceptRule::Action::ModifyHeaders;
        } else {
            continue;
        }

        if (auto methods = obj.get_array("methods"sv); methods.has_value()) {
            for (auto const& m : methods->values()) {
                if (m.is_string() && !m.as_string().is_empty())
                    rule.methods.append(m.as_string());
            }
        }

        if (auto status = obj.get_u64("status_code"sv); status.has_value() && *status >= 100 && *status <= 599)
            rule.status_code = static_cast<u32>(*status);

        if (auto content_type = obj.get_string("content_type"sv); content_type.has_value() && !content_type->is_empty())
            rule.content_type = *content_type;

        if (auto body_base64 = obj.get_string("body_base64"sv); body_base64.has_value()) {
            auto decoded = decode_base64(*body_base64);
            if (!decoded.is_error() && decoded.value().size() <= MAX_BODY_BYTES)
                rule.body = decoded.release_value();
        } else if (auto body = obj.get_string("body"sv); body.has_value()) {
            if (body->bytes().size() <= MAX_BODY_BYTES)
                rule.body = TRY(ByteBuffer::copy(body->bytes()));
        }

        if (auto headers = obj.get_object("headers"sv); headers.has_value()) {
            headers->for_each_member([&](String const& name, JsonValue const& val) {
                if (!val.is_string())
                    return;
                auto name_trimmed = name.bytes_as_string_view().trim_whitespace();
                auto val_str = val.as_string();
                if (name_trimmed.is_empty() || name_trimmed.contains('\r') || name_trimmed.contains('\n'))
                    return;
                if (val_str.bytes_as_string_view().contains('\r') || val_str.bytes_as_string_view().contains('\n'))
                    return;
                rule.headers.append(HTTP::Header {
                    .name = ByteString { name_trimmed },
                    .value = val_str.to_byte_string(),
                });
            });
        }

        next_rules.append(move(rule));
    }

    m_rules = move(next_rules);
    return {};
}

Optional<NetworkInterceptRule> NetworkInterceptStore::match(URL::URL const& url, StringView method) const
{
    if (m_rules.is_empty())
        return {};

    auto serialized = url.serialize();
    for (auto const& rule : m_rules) {
        if (rule.action == NetworkInterceptRule::Action::ModifyHeaders)
            continue;
        if (!method_matches(rule.methods, method))
            continue;
        if (url_matches_pattern(serialized, rule.url_pattern))
            return rule;
    }
    return {};
}

void NetworkInterceptStore::apply_header_modifications(URL::URL const& url, StringView method, HTTP::HeaderList& headers) const
{
    if (m_rules.is_empty())
        return;

    auto serialized = url.serialize();
    for (auto const& rule : m_rules) {
        if (rule.action != NetworkInterceptRule::Action::ModifyHeaders)
            continue;
        if (!method_matches(rule.methods, method))
            continue;
        if (!url_matches_pattern(serialized, rule.url_pattern))
            continue;
        for (auto const& header : rule.headers)
            headers.set(header);
    }
}

}
