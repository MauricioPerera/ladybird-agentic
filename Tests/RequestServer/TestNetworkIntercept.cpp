/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibHTTP/HeaderList.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <RequestServer/NetworkIntercept.h>

static URL::URL test_url()
{
    return URL::Parser::basic_parse("https://sensitive.example/app.js"sv).release_value();
}

TEST_CASE(network_intercept_rejects_unscoped_rules)
{
    auto& store = RequestServer::NetworkInterceptStore::the();
    store.clear();
    ScopeGuard clear_store = [&] { store.clear(); };

    MUST(store.set_rules_from_json(R"({"rules":[{"id":"unscoped","url_pattern":"*","action":"fulfill","body":"unsafe"}]})"sv));

    EXPECT(!store.match(42, test_url(), "GET"sv).has_value());
}

TEST_CASE(network_intercept_matches_only_authorized_page)
{
    auto& store = RequestServer::NetworkInterceptStore::the();
    store.clear();
    ScopeGuard clear_store = [&] { store.clear(); };

    MUST(store.set_rules_from_json(R"({"rules":[{"id":"scoped","page_ids":[42],"url_pattern":"https://sensitive.example/*","action":"fulfill","body":"controlled"}]})"sv));

    EXPECT(store.match(42, test_url(), "GET"sv).has_value());
    EXPECT(!store.match(43, test_url(), "GET"sv).has_value());
    EXPECT(!store.match(0, test_url(), "GET"sv).has_value());
}

TEST_CASE(network_header_modification_matches_only_authorized_page)
{
    auto& store = RequestServer::NetworkInterceptStore::the();
    store.clear();
    ScopeGuard clear_store = [&] { store.clear(); };

    MUST(store.set_rules_from_json(R"({"rules":[{"id":"headers","page_ids":[42],"url_pattern":"*","action":"modify_headers","headers":{"X-MCP-Scope":"authorized"}}]})"sv));

    auto unrelated_headers = HTTP::HeaderList::create();
    store.apply_header_modifications(43, test_url(), "GET"sv, *unrelated_headers);
    EXPECT(!unrelated_headers->contains("X-MCP-Scope"sv));

    auto authorized_headers = HTTP::HeaderList::create();
    store.apply_header_modifications(42, test_url(), "GET"sv, *authorized_headers);
    EXPECT_EQ(authorized_headers->get("X-MCP-Scope"sv).value(), "authorized"sv);
}
