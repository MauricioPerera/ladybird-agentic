/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibDatabase/Database.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/ContainerStore.h>
#include <LibWebView/CookieJar.h>

namespace {

class CountingContainerObserver final : public WebView::ContainerStoreObserver {
public:
    virtual void containers_changed() override { ++changes; }

    size_t changes { 0 };
};

URL::URL parse_url(StringView url)
{
    auto parsed = URL::Parser::basic_parse(url);
    VERIFY(parsed.has_value());
    return parsed.release_value();
}

}

TEST_CASE(container_store_defaults_and_persistence)
{
    auto temp_dir = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("ladybird-container-test-{}", get_random<u64>())).string();
    MUST(Core::Directory::create(temp_dir, Core::Directory::CreateDirectories::Yes));
    ScopeGuard cleanup_guard { [&] { (void)FileSystem::remove(temp_dir, FileSystem::RecursionMode::Allowed); } };

    auto persistence_path = LexicalPath::join(temp_dir, "Containers.json"sv).string();

    {
        WebView::ContainerStore store(persistence_path);
        CountingContainerObserver observer;
        store.add_observer(observer);
        ScopeGuard observer_guard { [&] { store.remove_observer(observer); } };

        auto defaults = store.list_containers();
        EXPECT_EQ(defaults.size(), 4uz);
        EXPECT(store.get_container("personal"sv).has_value());
        EXPECT(store.get_container("work"sv).has_value());
        EXPECT(store.get_container("banking"sv).has_value());
        EXPECT(store.get_container("shopping"sv).has_value());

        EXPECT(store.create_container("   "_string, "#22cc88"_string).is_error());
        EXPECT(store.create_container("Bad Color"_string, "red"_string).is_error());

        auto created = TRY_OR_FAIL(store.create_container("Client A"_string, "#22cc88"_string));
        EXPECT_EQ(created.id, "client-a"_string);
        EXPECT_EQ(created.name, "Client A"_string);
        EXPECT_EQ(created.color, "#22cc88"_string);
        EXPECT_EQ(observer.changes, 1uz);
        EXPECT_EQ(store.list_containers().size(), 5uz);
    }

    WebView::ContainerStore reloaded(persistence_path);
    EXPECT_EQ(reloaded.list_containers().size(), 5uz);
    auto persisted = reloaded.get_container("client-a"sv);
    EXPECT(persisted.has_value());
    EXPECT_EQ(persisted->name, "Client A"_string);
    EXPECT_EQ(persisted->color, "#22cc88"_string);

    TRY_OR_FAIL(reloaded.delete_container("client-a"sv));
    EXPECT(!reloaded.get_container("client-a"sv).has_value());
    EXPECT(reloaded.delete_container("client-a"sv).is_error());
}

TEST_CASE(container_sessions_isolate_cookies_and_storage)
{
    auto default_session = WebView::BrowsingSession::create(WebView::IsPrivate::No);
    auto personal_session = WebView::BrowsingSession::create(WebView::IsPrivate::No, "personal"_string);
    auto work_session = WebView::BrowsingSession::create(WebView::IsPrivate::No, "work"_string);

    EXPECT(!default_session->is_container());
    EXPECT(default_session->container_id().is_empty());
    EXPECT(personal_session->is_container());
    EXPECT_EQ(personal_session->container_id(), "personal"_string);
    EXPECT(work_session->is_container());
    EXPECT_EQ(work_session->container_id(), "work"_string);
    EXPECT(default_session->id() != personal_session->id());
    EXPECT(personal_session->id() != work_session->id());

    auto default_db = TRY_OR_FAIL(Database::Database::create_memory_backed());
    auto personal_db = TRY_OR_FAIL(Database::Database::create_memory_backed());
    auto work_db = TRY_OR_FAIL(Database::Database::create_memory_backed());
    TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*default_db));
    TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*personal_db));
    TRY_OR_FAIL(WebView::CookieJar::migrate_schema(*work_db));

    default_session->cookie_jar = TRY_OR_FAIL(WebView::CookieJar::create(*default_db));
    personal_session->cookie_jar = TRY_OR_FAIL(WebView::CookieJar::create(*personal_db));
    work_session->cookie_jar = TRY_OR_FAIL(WebView::CookieJar::create(*work_db));

    auto google_url = parse_url("https://accounts.google.com/"sv);
    auto expiry = UnixDateTime::now() + AK::Duration::from_seconds(3600);

    personal_session->cookie_jar->set_cookie(google_url, { .name = "SAPISID"_string, .value = "youtube-personal"_string, .expiry_time_from_expires_attribute = expiry }, HTTP::Cookie::Source::Http, {}, HTTP::Cookie::SameSiteContext::same_site());
    work_session->cookie_jar->set_cookie(google_url, { .name = "SAPISID"_string, .value = "gmail-work"_string, .expiry_time_from_expires_attribute = expiry }, HTTP::Cookie::Source::Http, {}, HTTP::Cookie::SameSiteContext::same_site());

    EXPECT(default_session->cookie_jar->get_all_cookies().is_empty());
    auto personal_cookies = personal_session->cookie_jar->get_all_cookies();
    auto work_cookies = work_session->cookie_jar->get_all_cookies();
    EXPECT_EQ(personal_cookies.size(), 1uz);
    EXPECT_EQ(personal_cookies[0].value, "youtube-personal"_string);
    EXPECT_EQ(work_cookies.size(), 1uz);
    EXPECT_EQ(work_cookies[0].value, "gmail-work"_string);
}
