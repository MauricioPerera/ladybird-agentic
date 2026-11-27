/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API FlowLocator {
    enum class Kind {
        Css,
        XPath,
        Role,
        Label,
        Placeholder,
        Text,
        TestId,
    };

    Kind kind { Kind::Css };
    String value;
    String role;
    Optional<String> name;
    bool exact { false };
    Optional<size_t> nth;
};

struct WEBVIEW_API FlowStep {
    enum class Action {
        Open,
        Reload,
        Back,
        Forward,
        Click,
        Fill,
        Press,
        WaitFor,
        AssertText,
        AssertVisible,
        AssertUrl,
        Extract,
        Eval,
        Sleep,
        Screenshot,
        UploadFile,
    };

    Action action { Action::Open };
    Optional<String> label;
    Optional<String> url;
    Optional<FlowLocator> locator;
    Optional<String> text;
    Vector<String> paths;
    Optional<String> key;
    bool clear { true };
    Optional<String> contains;
    Optional<String> equals;
    bool wait_for_load { true };
    Optional<String> script;
    Optional<String> save_as;
    Optional<u32> timeout_ms;
    Optional<u32> ms;
};

struct WEBVIEW_API FlowDefinition {
    u32 schema_version { 1 };
    String id;
    String name;
    String description;
    HashMap<String, String> variables;
    u32 default_timeout_ms { 10000 };
    i64 created_at_ms { 0 };
    i64 updated_at_ms { 0 };
    Vector<FlowStep> steps;
};

struct WEBVIEW_API FlowStepResult {
    size_t index { 0 };
    String action;
    Optional<String> label;
    String status;
    i64 duration_ms { 0 };
    Optional<String> error;
    JsonValue output;
    Optional<String> screenshot_path;
};

struct WEBVIEW_API FlowRunRecord {
    String run_id;
    String flow_id;
    String flow_name;
    String status;
    Optional<u64> tab_id;
    i64 started_at_ms { 0 };
    Optional<i64> finished_at_ms;
    i64 duration_ms { 0 };
    size_t current_step_index { 0 };
    size_t total_steps { 0 };
    Optional<String> error;
    JsonObject outputs;
    Vector<FlowStepResult> steps;
};

WEBVIEW_API bool is_valid_flow_id(StringView id);
WEBVIEW_API StringView flow_action_name(FlowStep::Action action);
WEBVIEW_API StringView flow_locator_kind_name(FlowLocator::Kind kind);
WEBVIEW_API ErrorOr<FlowDefinition> parse_flow_definition(JsonValue const& value);
WEBVIEW_API JsonObject serialize_flow_definition(FlowDefinition const& flow);
WEBVIEW_API JsonObject serialize_flow_summary(FlowDefinition const& flow);
WEBVIEW_API JsonObject serialize_flow_run(FlowRunRecord const& run);
WEBVIEW_API String interpolate_flow_variables(StringView input, HashMap<String, String> const& vars);
WEBVIEW_API String build_locator_action_script(StringView operation, FlowLocator const& locator, JsonObject const& params = {});
WEBVIEW_API FlowDefinition make_example_flow();

}
