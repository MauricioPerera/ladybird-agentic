/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/Extensions/ExtensionStore.h>
#include <LibWebView/Extensions/ExtensionTypes.h>

namespace {

class CountingExtensionObserver final : public WebView::ExtensionStoreObserver {
public:
    virtual void extensions_changed() override { ++changes; }

    size_t changes { 0 };
};

ByteString make_temp_extensions_dir()
{
    auto dir = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("ladybird-ext-test-{}", get_random<u64>())).string();
    MUST(Core::Directory::create(dir, Core::Directory::CreateDirectories::Yes));
    return dir;
}

void write_text_file(ByteString const& path, StringView contents)
{
    MUST(Core::Directory::create(LexicalPath { path }.parent(), Core::Directory::CreateDirectories::Yes));
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    MUST(file->write_until_depleted(contents.bytes()));
}

}

TEST_CASE(extension_id_and_relative_path_validation)
{
    EXPECT(WebView::is_valid_extension_id("page-inspector_1"sv));
    EXPECT(WebView::is_valid_extension_id("a"sv));

    EXPECT(!WebView::is_valid_extension_id(""sv));
    EXPECT(!WebView::is_valid_extension_id("Uppercase"sv));
    EXPECT(!WebView::is_valid_extension_id("-leading-dash"sv));
    EXPECT(!WebView::is_valid_extension_id("bad/slash"sv));

    EXPECT(WebView::is_safe_relative_extension_path("content.js"sv));
    EXPECT(WebView::is_safe_relative_extension_path("scripts/content.js"sv));
    EXPECT(!WebView::is_safe_relative_extension_path("../secret.js"sv));
    EXPECT(!WebView::is_safe_relative_extension_path("/abs/path.js"sv));
    EXPECT(!WebView::is_safe_relative_extension_path("C:/win/path.js"sv));
    EXPECT(!WebView::is_safe_relative_extension_path("a\\b.js"sv));
    EXPECT(!WebView::is_safe_relative_extension_path("a//b.js"sv));

    EXPECT_EQ(WebView::sanitize_extension_id_from_name("My Cool Plugin!"sv), "my-cool-plugin"_string);
}

TEST_CASE(extension_match_patterns)
{
    auto http_example = URL::Parser::basic_parse("http://example.com/foo/bar?q=1"sv).value();
    auto https_sub = URL::Parser::basic_parse("https://app.docs.example.com/guide"sv).value();
    auto about_ext = URL::Parser::basic_parse("about:extensions"sv).value();
    auto file_url = URL::Parser::basic_parse("file:///tmp/test.html"sv).value();

    EXPECT(WebView::url_matches_extension_pattern(http_example, "<all_urls>"sv));
    EXPECT(WebView::url_matches_extension_pattern(http_example, "*://*.example.com/foo/*"sv));
    EXPECT(WebView::url_matches_extension_pattern(https_sub, "https://*.example.com/*"sv));
    EXPECT(!WebView::url_matches_extension_pattern(https_sub, "http://*.example.com/*"sv));
    EXPECT(!WebView::url_matches_extension_pattern(http_example, "https://other.com/*"sv));

    // Protected schemes are never matched.
    EXPECT(!WebView::url_matches_extension_pattern(about_ext, "<all_urls>"sv));
    EXPECT(!WebView::url_matches_extension_pattern(file_url, "<all_urls>"sv));
}

TEST_CASE(extension_manifest_parse_and_errors)
{
    auto valid_json = MUST(JsonValue::from_string(R"JSON({
        "manifest_version": 3,
        "name": "Demo Plugin",
        "version": "1.2.3",
        "permissions": ["storage", "flows"],
        "content_scripts": [
            { "matches": ["https://*.example.com/*"], "js": ["content.js"], "css": ["style.css"] }
        ],
        "ladybird": { "flows": ["flows/demo.json"] }
    })JSON"sv));

    auto manifest = MUST(WebView::parse_extension_manifest(valid_json));
    EXPECT_EQ(manifest.id, "demo-plugin"_string);
    EXPECT_EQ(manifest.permissions.size(), 2u);
    EXPECT_EQ(manifest.content_scripts.size(), 1u);
    EXPECT_EQ(manifest.ladybird_flow_files.size(), 1u);

    auto bad_mv2 = MUST(JsonValue::from_string(R"JSON({ "manifest_version": 2, "name": "Old", "version": "1.0" })JSON"sv));
    EXPECT(WebView::parse_extension_manifest(bad_mv2).is_error());

    auto bad_path = MUST(JsonValue::from_string(R"JSON({
        "manifest_version": 3,
        "name": "Bad Path",
        "version": "1.0",
        "content_scripts": [{ "matches": ["<all_urls>"], "js": ["../escape.js"] }]
    })JSON"sv));
    EXPECT(WebView::parse_extension_manifest(bad_path).is_error());
}

TEST_CASE(extension_store_unpacked_storage_and_example)
{
    auto store_dir = make_temp_extensions_dir();
    auto unpacked_dir = make_temp_extensions_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(store_dir, FileSystem::RecursionMode::Allowed);
        (void)FileSystem::remove(unpacked_dir, FileSystem::RecursionMode::Allowed);
    };

    write_text_file(LexicalPath::join(unpacked_dir, "manifest.json"sv).string(), R"JSON({
        "manifest_version": 3,
        "id": "custom-helper",
        "name": "Custom Helper",
        "version": "0.1.0",
        "permissions": ["storage"],
        "content_scripts": [{ "matches": ["http://*/*"], "js": ["main.js"] }]
    })JSON"sv);
    write_text_file(LexicalPath::join(unpacked_dir, "main.js"sv).string(), "console.log('hi');"sv);

    WebView::ExtensionStore store(store_dir);
    CountingExtensionObserver observer;
    store.add_observer(observer);

    auto installed = MUST(store.install_unpacked(unpacked_dir));
    EXPECT_EQ(installed.manifest.id, "custom-helper"_string);
    EXPECT_EQ(observer.changes, 1u);

    JsonObject storage;
    storage.set("greeting"sv, "hola"_string);
    MUST(store.set_storage("custom-helper"sv, move(storage)));
    EXPECT_EQ(store.get_storage("custom-helper"sv).get_string("greeting"sv).value_or(String {}), "hola"_string);

    auto example = MUST(store.create_example_extension());
    EXPECT_EQ(example.manifest.id, "ladybird-page-inspector"_string);
    EXPECT_EQ(store.list_extensions().size(), 2u);

    // Reload from disk in a fresh store instance.
    WebView::ExtensionStore reloaded(store_dir);
    EXPECT_EQ(reloaded.list_extensions().size(), 2u);
    MUST(reloaded.set_enabled("custom-helper"sv, false));
    EXPECT(!reloaded.get_extension("custom-helper"sv)->enabled);

    MUST(reloaded.remove_extension("custom-helper"sv));
    EXPECT(!reloaded.get_extension("custom-helper"sv).has_value());
}

TEST_CASE(extension_injection_script_generation)
{
    Vector<String> css_list;
    css_list.append("body { border: 1px solid red; }"_string);
    Vector<String> js_list;
    js_list.append("console.log(ladybirdExt.id);"_string);
    JsonObject storage;
    storage.set("flag"sv, true);

    auto script = WebView::build_extension_injection_script("demo-ext"sv, "Demo Ext"sv, css_list, js_list, storage);
    EXPECT(script.bytes_as_string_view().starts_with("return (function(__cfg)"sv));
    EXPECT(script.contains("data-ladybird-ext-id"sv));
    EXPECT(script.contains("\"demo-ext\""sv));
    EXPECT(script.contains("console.log(ladybirdExt.id);"sv));
}

TEST_CASE(extension_dnr_translation_and_filter_lists)
{
    auto valid_manifest_json = MUST(JsonValue::from_string(R"JSON({
        "manifest_version": 3,
        "id": "adblock-demo",
        "name": "Adblock Demo",
        "version": "1.0.0",
        "permissions": ["declarativeNetRequest", "storage"],
        "declarative_net_request": {
            "rule_resources": [
                { "id": "ads", "enabled": true, "path": "rules.json" }
            ]
        },
        "ladybird": {
            "filter_lists": ["filters.txt"]
        }
    })JSON"sv));

    auto manifest = MUST(WebView::parse_extension_manifest(valid_manifest_json));
    EXPECT_EQ(manifest.declarative_net_request_rulesets.size(), 1u);
    EXPECT_EQ(manifest.declarative_net_request_rulesets[0].id, "ads"_string);
    EXPECT_EQ(manifest.declarative_net_request_rulesets[0].path, "rules.json"_string);
    EXPECT_EQ(manifest.ladybird_filter_lists.size(), 1u);
    EXPECT_EQ(manifest.ladybird_filter_lists[0], "filters.txt"_string);

    auto bad_dnr = MUST(JsonValue::from_string(R"JSON({
        "manifest_version": 3,
        "name": "Bad DNR",
        "version": "1.0",
        "declarative_net_request": {
            "rule_resources": [{ "id": "bad", "path": "../outside.json" }]
        }
    })JSON"sv));
    EXPECT(WebView::parse_extension_manifest(bad_dnr).is_error());

    auto ruleset_json = MUST(JsonValue::from_string(R"JSON([
        {
            "id": 1,
            "action": { "type": "block" },
            "condition": {
                "requestDomains": ["doubleclick.net"],
                "initiatorDomains": ["youtube.com"],
                "excludedInitiatorDomains": ["ads.youtube.com"],
                "resourceTypes": ["script", "sub_frame"]
            }
        },
        {
            "id": 2,
            "action": { "type": "allow" },
            "condition": {
                "urlFilter": "||youtube.com/api/stats/allow*",
                "resourceTypes": ["xmlhttprequest"]
            }
        }
    ])JSON"sv));

    auto adblock_rules = MUST(WebView::translate_dnr_ruleset_to_adblock_rules(ruleset_json));
    EXPECT(adblock_rules.contains("||doubleclick.net^$script,subdocument,domain=youtube.com|~ads.youtube.com"sv));
    EXPECT(adblock_rules.contains("@@||youtube.com/api/stats/allow*$xmlhttprequest"sv));
}

TEST_CASE(extension_store_validates_dnr_and_filter_files)
{
    auto store_dir = make_temp_extensions_dir();
    auto unpacked_dir = make_temp_extensions_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(store_dir, FileSystem::RecursionMode::Allowed);
        (void)FileSystem::remove(unpacked_dir, FileSystem::RecursionMode::Allowed);
    };

    write_text_file(LexicalPath::join(unpacked_dir, "manifest.json"sv).string(), R"JSON({
        "manifest_version": 3,
        "id": "yt-clean-test",
        "name": "YT Clean Test",
        "version": "1.0.0",
        "permissions": ["declarativeNetRequest"],
        "declarative_net_request": {
            "rule_resources": [{ "id": "ads", "enabled": true, "path": "rules.json" }]
        },
        "ladybird": {
            "filter_lists": ["filters.txt"]
        }
    })JSON"sv);

    WebView::ExtensionStore store(store_dir);
    // Should fail if rules.json / filters.txt don't exist yet.
    EXPECT(store.install_unpacked(unpacked_dir).is_error());

    write_text_file(LexicalPath::join(unpacked_dir, "rules.json"sv).string(), R"JSON([
        { "id": 1, "action": { "type": "block" }, "condition": { "urlFilter": "||ads.example.com^" } }
    ])JSON"sv);
    write_text_file(LexicalPath::join(unpacked_dir, "filters.txt"sv).string(), "youtube.com##ytd-reel-shelf-renderer\n"sv);

    auto installed = MUST(store.install_unpacked(unpacked_dir));
    EXPECT_EQ(installed.manifest.declarative_net_request_rulesets.size(), 1u);
    EXPECT_EQ(installed.manifest.ladybird_filter_lists.size(), 1u);

    auto serialized = WebView::serialize_extension(installed);
    EXPECT_EQ(serialized.get_array("declarative_net_request_rulesets"sv)->size(), 1u);
    EXPECT_EQ(serialized.get_array("filter_lists"sv)->size(), 1u);
}
