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
    EXPECT_EQ(reparsed.variables.get("target_url"sv).value_or(String {}), "http://example.com"sv);
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

    auto make_locator_flow = [](i64 nth) {
        JsonObject loc;
        loc.set("kind"sv, "css"sv);
        loc.set("value"sv, "button"sv);
        loc.set("nth"sv, nth);
        JsonObject step;
        step.set("action"sv, "click"sv);
        step.set("locator"sv, move(loc));
        JsonArray steps;
        steps.must_append(move(step));
        JsonObject flow;
        flow.set("id"sv, "nth-bound-check"sv);
        flow.set("name"sv, "Nth Bound Check"sv);
        flow.set("steps"sv, move(steps));
        return flow;
    };
    EXPECT(!WebView::parse_flow_definition(JsonValue { make_locator_flow(1000) }).is_error());
    EXPECT(WebView::parse_flow_definition(JsonValue { make_locator_flow(1001) }).is_error());
    EXPECT(WebView::parse_flow_definition(JsonValue { make_locator_flow(-1) }).is_error());
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
    EXPECT(script.contains(R"("value":"button[data-label=\"a'\\\"</script>\\n\"]")"sv));
    EXPECT(script.contains(R"("text":"line1\nline2 'quotes' \"double\"")"sv));
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
    ScopeGuard remove_observer = [&] {
        store.remove_observer(observer);
    };

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
    EXPECT(store.delete_flow("example-smoke"sv).is_error());
}

TEST_CASE(flow_store_prunes_evicted_runs_from_disk)
{
    auto temp_dir = make_temp_flows_dir();
    ScopeGuard cleanup = [&] {
        (void)FileSystem::remove(temp_dir, FileSystem::RecursionMode::Allowed);
    };

    WebView::FlowStore store(temp_dir);
    for (size_t i = 0; i < 53; ++i) {
        WebView::FlowRunRecord run;
        run.run_id = MUST(String::formatted("run-{:03}", i));
        run.flow_id = "example-smoke"_string;
        run.flow_name = "Example"_string;
        run.status = "passed"_string;
        run.started_at_ms = static_cast<i64>(1000 + i);
        run.finished_at_ms = static_cast<i64>(1010 + i);
        run.duration_ms = 10;
        run.total_steps = 1;
        store.upsert_run(run, true);

        if (i < 3) {
            auto png_path = LexicalPath::join(store.runs_directory(), ByteString::formatted("{}_step_0.png", run.run_id)).string();
            auto file = MUST(Core::File::open(png_path, Core::File::OpenMode::Write));
            MUST(file->write_until_depleted("png"sv));
        }
    }

    EXPECT_EQ(store.list_runs({}, 100).size(), 50u);
    EXPECT(!store.get_run("run-000"sv).has_value());
    EXPECT(!store.get_run("run-002"sv).has_value());
    EXPECT(store.get_run("run-003"sv).has_value());
    EXPECT(store.get_run("run-052"sv).has_value());

    EXPECT(!FileSystem::exists(LexicalPath::join(store.runs_directory(), "run-000.json"sv).string()));
    EXPECT(!FileSystem::exists(LexicalPath::join(store.runs_directory(), "run-000_step_0.png"sv).string()));
    EXPECT(!FileSystem::exists(LexicalPath::join(store.runs_directory(), "run-002.json"sv).string()));
    EXPECT(!FileSystem::exists(LexicalPath::join(store.runs_directory(), "run-002_step_0.png"sv).string()));
    EXPECT(FileSystem::exists(LexicalPath::join(store.runs_directory(), "run-052.json"sv).string()));

    WebView::FlowStore reloaded(temp_dir);
    EXPECT_EQ(reloaded.list_runs({}, 100).size(), 50u);
    EXPECT(!reloaded.get_run("run-000"sv).has_value());
    EXPECT(reloaded.get_run("run-052"sv).has_value());
}
