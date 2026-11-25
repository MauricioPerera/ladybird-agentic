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
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Automation/FlowStore.h>
#include <LibWebView/Automation/FlowTypes.h>

namespace {

class CountingObserver final : public WebView::FlowStoreObserver {
public:
    virtual void flows_changed() override { ++flows_notifications; }
    virtual void flow_runs_changed() override { ++runs_notifications; }

    size_t flows_notifications { 0 };
    size_t runs_notifications { 0 };
};

ByteString make_temp_flows_dir()
{
    auto dir = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("ladybird-flow-test-{}", get_random<u64>())).string();
    MUST(Core::Directory::create(dir, Core::Directory::CreateDirectories::Yes));
    return dir;
}

}

TEST_CASE(flow_id_validation)
{
    EXPECT(WebView::is_valid_flow_id("smoke"sv));
    EXPECT(WebView::is_valid_flow_id("login-check_2"sv));
    EXPECT(WebView::is_valid_flow_id("0-flow"sv));

    EXPECT(!WebView::is_valid_flow_id(""sv));
    EXPECT(!WebView::is_valid_flow_id("Uppercase"sv));
    EXPECT(!WebView::is_valid_flow_id("../escape"sv));
    EXPECT(!WebView::is_valid_flow_id("a/b"sv));
    EXPECT(!WebView::is_valid_flow_id("has space"sv));
    EXPECT(!WebView::is_valid_flow_id("-starts-with-dash"sv));
}

TEST_CASE(parse_and_serialize_flow_roundtrip)
{
    auto example = WebView::make_example_flow();
    auto serialized = WebView::serialize_flow_definition(example);
    auto reparsed = MUST(WebView::parse_flow_definition(JsonValue { move(serialized) }));

    EXPECT_EQ(reparsed.id, example.id);
    EXPECT_EQ(reparsed.name, example.name);
    EXPECT_EQ(reparsed.steps.size(), example.steps.size());
    EXPECT_EQ(reparsed.variables.get("target_url"sv).value_or(String {}), "https://example.com"sv);
    EXPECT(reparsed.steps[1].locator.has_value());
    EXPECT_EQ(reparsed.steps[1].locator->kind, WebView::FlowLocator::Kind::Role);
    EXPECT_EQ(reparsed.steps[1].locator->role, "heading"sv);

    JsonObject invalid_missing_steps;
    invalid_missing_steps.set("id"sv, "valid-id"sv);
    invalid_missing_steps.set("name"sv, "No Steps"sv);
    EXPECT(WebView::parse_flow_definition(JsonValue { move(invalid_missing_steps) }).is_error());

    JsonObject invalid_id;
    invalid_id.set("id"sv, "../bad"sv);
    invalid_id.set("name"sv, "Bad ID"sv);
    EXPECT(WebView::parse_flow_definition(JsonValue { move(invalid_id) }).is_error());
}

TEST_CASE(variable_interpolation_single_pass)
{
    HashMap<String, String> vars;
    vars.set("host"_string, "https://example.com"_string);
    vars.set("user"_string, "{{secret}}"_string);
    vars.set("secret"_string, "SHOULD_NOT_EXPAND"_string);

    EXPECT_EQ(WebView::interpolate_flow_variables("{{host}}/login?u={{ user }}&keep={{unknown}}"sv, vars),
        "https://example.com/login?u={{secret}}&keep={{unknown}}"sv);
}

TEST_CASE(locator_action_script_escapes_special_characters)
{
    WebView::FlowLocator locator;
    locator.kind = WebView::FlowLocator::Kind::Css;
    locator.value = R"(button[data-label="a'\"</script>\n"])"_string;
    JsonObject params;
    params.set("text"sv, "line1\nline2 'quotes' \"double\""_string);

    auto script = WebView::build_locator_action_script("fill"sv, locator, params);
    EXPECT(script.bytes_as_string_view().starts_with("return (function(cfg)"sv));
    EXPECT(script.contains("</script>"sv) || script.contains("\\u003c/script>"sv) || script.contains("a'\\\"</script>"sv));
    EXPECT(script.contains("\"operation\":\"fill\""sv));
}

TEST_CASE(flow_store_persistence_and_observers)
{
    auto temp_dir = make_temp_flows_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(temp_dir, FileSystem::RecursionMode::Allowed);
    };

    WebView::FlowStore store(temp_dir);
    CountingObserver observer;
    store.add_observer(observer);

    auto example = WebView::make_example_flow();
    auto saved = MUST(store.save_flow(example));
    EXPECT_EQ(observer.flows_notifications, 1u);
    EXPECT_EQ(store.list_flows().size(), 1u);
    EXPECT(saved.created_at_ms > 0);

    // Reload from disk in a second FlowStore instance.
    WebView::FlowStore reloaded(temp_dir);
    auto loaded = reloaded.get_flow("example-smoke"sv);
    EXPECT(loaded.has_value());
    EXPECT_EQ(loaded->steps.size(), example.steps.size());

    WebView::FlowRunRecord run;
    run.run_id = "run-test-1"_string;
    run.flow_id = "example-smoke"_string;
    run.flow_name = example.name;
    run.status = "passed"_string;
    run.started_at_ms = 123456;
    run.finished_at_ms = 123556;
    run.duration_ms = 100;
    run.total_steps = 1;
    store.upsert_run(run, true);
    EXPECT_EQ(observer.runs_notifications, 1u);

    WebView::FlowStore reloaded_with_runs(temp_dir);
    auto persisted_run = reloaded_with_runs.get_run("run-test-1"sv);
    EXPECT(persisted_run.has_value());
    EXPECT_EQ(persisted_run->status, "passed"sv);

    MUST(store.delete_flow("example-smoke"sv));
    EXPECT_EQ(observer.flows_notifications, 2u);
    EXPECT(!store.get_flow("example-smoke"sv).has_value());

    store.remove_observer(observer);
}
