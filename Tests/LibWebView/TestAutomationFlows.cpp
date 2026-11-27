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

TEST_CASE(upload_file_step_parse_serialize_and_shadow_dom_script)
{
    auto flow_json = MUST(JsonValue::from_string(R"JSON({
        "schema_version": 1,
        "id": "yt-upload-video",
        "name": "Upload Video",
        "steps": [
            {
                "action": "upload_file",
                "label": "Attach video",
                "locator": { "kind": "css", "value": "ytcp-uploads-file-picker #select-files-button" },
                "path": "demo.mp4",
                "save_as": "upload_info"
            }
        ]
    })JSON"sv));

    auto parsed = MUST(WebView::parse_flow_definition(flow_json));
    EXPECT_EQ(parsed.steps.size(), 1u);
    EXPECT_EQ(parsed.steps[0].action, WebView::FlowStep::Action::UploadFile);
    EXPECT_EQ(parsed.steps[0].paths.size(), 1u);
    EXPECT_EQ(parsed.steps[0].paths[0], "demo.mp4"sv);
    EXPECT_EQ(parsed.steps[0].save_as.value_or(String {}), "upload_info"sv);

    auto serialized = WebView::serialize_flow_definition(parsed);
    auto reparsed = MUST(WebView::parse_flow_definition(JsonValue { move(serialized) }));
    EXPECT_EQ(reparsed.steps[0].action, WebView::FlowStep::Action::UploadFile);
    EXPECT_EQ(reparsed.steps[0].paths.size(), 1u);
    EXPECT_EQ(reparsed.steps[0].paths[0], "demo.mp4"sv);

    auto missing_paths = MUST(JsonValue::from_string(R"JSON({
        "id": "bad-upload",
        "name": "Bad Upload",
        "steps": [{ "action": "upload_file", "locator": { "kind": "css", "value": "input[type=file]" } }]
    })JSON"sv));
    EXPECT(WebView::parse_flow_definition(missing_paths).is_error());

    auto missing_locator = MUST(JsonValue::from_string(R"JSON({
        "id": "bad-upload-2",
        "name": "Bad Upload 2",
        "steps": [{ "action": "upload_file", "paths": ["demo.mp4"] }]
    })JSON"sv));
    EXPECT(WebView::parse_flow_definition(missing_locator).is_error());

    auto script = WebView::build_locator_action_script("resolve_upload_input"sv, *parsed.steps[0].locator, {});
    EXPECT(script.contains("queryAllDeep"sv));
    EXPECT(script.contains("resolve_upload_input"sv));
    EXPECT(script.contains("shadowRoot"sv));
}

TEST_CASE(flow_source_provenance_validation_and_roundtrip)
{
    EXPECT(WebView::is_valid_flow_source("user"sv));
    EXPECT(WebView::is_valid_flow_source("mcp"sv));
    EXPECT(WebView::is_valid_flow_source("unknown"sv));
    EXPECT(WebView::is_valid_flow_source("extension:my-ext"sv));

    EXPECT(!WebView::is_valid_flow_source(""sv));
    EXPECT(!WebView::is_valid_flow_source("admin"sv));
    EXPECT(!WebView::is_valid_flow_source("extension:../bad"sv));

    auto default_json = MUST(JsonValue::from_string(R"JSON({
        "id": "default-source-flow",
        "name": "Default Source",
        "steps": [{ "action": "sleep", "ms": 10 }]
    })JSON"sv));
    auto parsed_default = MUST(WebView::parse_flow_definition(default_json));
    EXPECT_EQ(parsed_default.source, "unknown"sv);

    auto invalid_src_json = MUST(JsonValue::from_string(R"JSON({
        "id": "invalid-source-flow",
        "name": "Invalid Source",
        "source": "admin",
        "steps": [{ "action": "sleep", "ms": 10 }]
    })JSON"sv));
    auto parsed_invalid = MUST(WebView::parse_flow_definition(invalid_src_json));
    EXPECT_EQ(parsed_invalid.source, "unknown"sv);

    auto mcp_json = MUST(JsonValue::from_string(R"JSON({
        "id": "mcp-source-flow",
        "name": "MCP Source",
        "source": "mcp",
        "steps": [{ "action": "sleep", "ms": 10 }]
    })JSON"sv));
    auto parsed_mcp = MUST(WebView::parse_flow_definition(mcp_json));
    EXPECT_EQ(parsed_mcp.source, "mcp"sv);

    auto full_serialized = WebView::serialize_flow_definition(parsed_mcp);
    EXPECT_EQ(full_serialized.get_string("source"sv).value_or(String {}), "mcp"sv);

    auto summary = WebView::serialize_flow_summary(parsed_mcp);
    EXPECT_EQ(summary.get_string("source"sv).value_or(String {}), "mcp"sv);
}

TEST_CASE(flow_edits_and_legacy_flows_keep_their_execution_policy)
{
    auto directory = make_temp_flows_dir();
    ScopeGuard cleanup = [&] { (void)FileSystem::remove(directory, FileSystem::RecursionMode::Allowed); };
    WebView::FlowStore store(directory);

    auto flow = WebView::make_example_flow();
    flow.source = "mcp"_string;
    auto saved = MUST(store.save_flow(flow));
    flow.source = "user"_string;
    EXPECT(store.requires_mcp_permission(flow));
    EXPECT_EQ(MUST(store.save_flow_from_ui(flow)).source, "mcp"sv);

    flow.id = "older-flow"_string;
    auto legacy = WebView::serialize_flow_definition(flow);
    legacy.remove("source"sv);
    auto migrated = MUST(WebView::parse_flow_definition(JsonValue { move(legacy) }));
    EXPECT(store.requires_mcp_permission(migrated));
    EXPECT_EQ(MUST(store.save_flow(migrated)).source, "unknown"sv);
    EXPECT_EQ(MUST(store.save_flow_from_ui(flow)).source, "unknown"sv);

    flow.id = "local-flow"_string;
    auto user_flow = MUST(store.save_flow_from_ui(flow));
    EXPECT_EQ(user_flow.source, "user"sv);
    EXPECT(!store.requires_mcp_permission(user_flow));

    flow.id = "fresh-import"_string;
    flow.source = "extension:unknown-owner"_string;
    auto import = MUST(store.save_flow_from_ui(flow));
    EXPECT_EQ(import.source, "unknown"sv);
    EXPECT(store.requires_mcp_permission(import));

    store.reload();
    EXPECT_EQ(store.get_flow(saved.id)->source, "mcp"sv);
    EXPECT_EQ(store.get_flow(migrated.id)->source, "unknown"sv);
}

TEST_CASE(extract_flow_template_variables_detects_placeholders)
{
    auto vars = WebView::extract_flow_template_variables("{{ host }}/path?q={{ token }}&dup={{host}}&bad={{unclosed"sv);
    EXPECT_EQ(vars.size(), 2u);
    EXPECT_EQ(vars[0], "host"sv);
    EXPECT_EQ(vars[1], "token"sv);
}
