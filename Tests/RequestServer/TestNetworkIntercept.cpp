/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <RequestServer/NetworkIntercept.h>

TEST_CASE(network_intercept_rules_require_exact_request_scope)
{
    auto& store = RequestServer::NetworkInterceptStore::the();
    store.clear();
    ScopeGuard clear_store([&] { store.clear(); });

    MUST(store.set_rules_from_json(R"JSON({
        "rules": [{
            "id": "authorized-rule",
            "url_pattern": "https://victim.example/*",
            "action": "fulfill",
            "methods": ["GET"],
            "body": "approved response",
            "scopes": [{ "client_id": 17, "page_id": 29 }]
        }]
    })JSON"sv));

    auto url = MUST(URL::Parser::basic_parse("https://victim.example/account"sv));
    auto authorized_match = store.match(url, "GET"sv, 17, 29);
    EXPECT(authorized_match.has_value());
    EXPECT(authorized_match->action == RequestServer::NetworkInterceptRule::Action::Fulfill);

    EXPECT(!store.match(url, "GET"sv, 18, 29).has_value());
    EXPECT(!store.match(url, "GET"sv, 17, 30).has_value());
    EXPECT(!store.match(url, "GET"sv, 17, 0).has_value());
    EXPECT(!store.match(url, "POST"sv, 17, 29).has_value());
}

TEST_CASE(network_intercept_rules_reject_unscoped_payloads)
{
    auto& store = RequestServer::NetworkInterceptStore::the();
    store.clear();
    ScopeGuard clear_store([&] { store.clear(); });

    MUST(store.set_rules_from_json(R"JSON({
        "rules": [{
            "id": "legacy-global-rule",
            "url_pattern": "*",
            "action": "fulfill",
            "body": "global response"
        }]
    })JSON"sv));

    auto url = MUST(URL::Parser::basic_parse("https://victim.example/account"sv));
    EXPECT(!store.match(url, "GET"sv, 17, 29).has_value());
}
