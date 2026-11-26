/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Automation/FlowStore.h>
#include <LibWebView/Utilities.h>

namespace WebView {

static constexpr size_t max_in_memory_runs = 50;

static Optional<FlowRunRecord> parse_flow_run_record(JsonObject const& obj)
{
    auto run_id = obj.get_string("run_id"sv);
    auto flow_id = obj.get_string("flow_id"sv);
    auto flow_name = obj.get_string("flow_name"sv);
    auto status = obj.get_string("status"sv);
    if (!run_id.has_value() || !flow_id.has_value() || !flow_name.has_value() || !status.has_value())
        return {};

    FlowRunRecord record;
    record.run_id = *run_id;
    record.flow_id = *flow_id;
    record.flow_name = *flow_name;
    record.status = *status;
    record.tab_id = obj.get_u64("tab_id"sv);
    record.started_at_ms = obj.get_integer<i64>("started_at_ms"sv).value_or(0);
    record.finished_at_ms = obj.get_integer<i64>("finished_at_ms"sv);
    record.duration_ms = obj.get_integer<i64>("duration_ms"sv).value_or(0);
    record.current_step_index = obj.get_integer<u64>("current_step_index"sv).value_or(0);
    record.total_steps = obj.get_integer<u64>("total_steps"sv).value_or(0);
    record.error = obj.get_string("error"sv).copy();
    if (auto outputs = obj.get_object("outputs"sv); outputs.has_value())
        record.outputs = *outputs;

    if (auto steps = obj.get_array("steps"sv); steps.has_value()) {
        for (auto const& val : steps->values()) {
            if (!val.is_object())
                continue;
            auto const& step_obj = val.as_object();
            FlowStepResult step;
            step.index = step_obj.get_integer<u64>("index"sv).value_or(0);
            step.action = step_obj.get_string("action"sv).value_or(String {});
            step.label = step_obj.get_string("label"sv).copy();
            step.status = step_obj.get_string("status"sv).value_or(String {});
            step.duration_ms = step_obj.get_integer<i64>("duration_ms"sv).value_or(0);
            step.error = step_obj.get_string("error"sv).copy();
            if (auto output = step_obj.get("output"sv); output.has_value())
                step.output = *output;
            step.screenshot_path = step_obj.get_string("screenshot_path"sv).copy();
            record.steps.append(move(step));
        }
    }

    return record;
}

FlowStore::FlowStore(ByteString storage_directory)
    : m_storage_directory(move(storage_directory))
{
    reload();
}

FlowStore::~FlowStore() = default;

ByteString FlowStore::runs_directory() const
{
    return LexicalPath::join(m_storage_directory, "runs"sv).string();
}

ByteString FlowStore::flow_file_path(StringView id) const
{
    return LexicalPath::join(m_storage_directory, ByteString::formatted("{}.json", id)).string();
}

ByteString FlowStore::run_file_path(StringView run_id) const
{
    return LexicalPath::join(runs_directory(), ByteString::formatted("{}.json", run_id)).string();
}

void FlowStore::reload()
{
    m_flows.clear();
    m_runs.clear();

    if (FileSystem::is_directory(m_storage_directory)) {
        auto result = Core::Directory::for_each_entry(m_storage_directory, Core::DirIterator::SkipParentAndBaseDir, [&](Core::DirectoryEntry const& entry, Core::Directory const& parent) -> ErrorOr<IterationDecision> {
            if (entry.type != Core::DirectoryEntry::Type::File || !entry.name.ends_with(".json"sv))
                return IterationDecision::Continue;

            auto full_path = parent.path().append(entry.name).string();
            auto json = read_json_file(full_path);
            if (json.is_error()) {
                warnln("Could not read flow file {}: {}", full_path, json.error());
                return IterationDecision::Continue;
            }
            auto flow = parse_flow_definition(JsonValue { move(json.value()) });
            if (flow.is_error()) {
                warnln("Skipping invalid flow file {}: {}", full_path, flow.error());
                return IterationDecision::Continue;
            }
            auto flow_value = flow.release_value();
            m_flows.set(flow_value.id, move(flow_value));
            return IterationDecision::Continue;
        });
        if (result.is_error())
            warnln("Failed scanning flow storage directory {}: {}", m_storage_directory, result.error());
    }

    auto runs_dir = runs_directory();
    if (FileSystem::is_directory(runs_dir)) {
        auto result = Core::Directory::for_each_entry(runs_dir, Core::DirIterator::SkipParentAndBaseDir, [&](Core::DirectoryEntry const& entry, Core::Directory const& parent) -> ErrorOr<IterationDecision> {
            if (entry.type != Core::DirectoryEntry::Type::File || !entry.name.ends_with(".json"sv))
                return IterationDecision::Continue;

            auto full_path = parent.path().append(entry.name).string();
            auto json = read_json_file(full_path);
            if (json.is_error())
                return IterationDecision::Continue;

            if (auto record = parse_flow_run_record(json.value()); record.has_value())
                m_runs.append(record.release_value());
            return IterationDecision::Continue;
        });
        if (result.is_error())
            warnln("Failed scanning flow runs directory {}: {}", runs_dir, result.error());

        quick_sort(m_runs, [](FlowRunRecord const& a, FlowRunRecord const& b) {
            return a.started_at_ms > b.started_at_ms;
        });
        if (m_runs.size() > max_in_memory_runs) {
            for (size_t i = max_in_memory_runs; i < m_runs.size(); ++i)
                remove_run_artifacts_from_disk(m_runs[i].run_id);
            m_runs.shrink(max_in_memory_runs);
        }
    }
}

Vector<FlowDefinition> FlowStore::list_flows() const
{
    Vector<FlowDefinition> flows;
    flows.ensure_capacity(m_flows.size());
    for (auto const& [_, flow] : m_flows)
        flows.unchecked_append(flow);

    quick_sort(flows, [](FlowDefinition const& a, FlowDefinition const& b) {
        if (a.updated_at_ms != b.updated_at_ms)
            return a.updated_at_ms > b.updated_at_ms;
        return a.id < b.id;
    });
    return flows;
}

Optional<FlowDefinition> FlowStore::get_flow(StringView id) const
{
    if (!is_valid_flow_id(id))
        return {};
    if (auto found = m_flows.get(id); found.has_value())
        return *found;
    return {};
}

ErrorOr<FlowDefinition> FlowStore::save_flow(FlowDefinition flow)
{
    if (!is_valid_flow_id(flow.id))
        return Error::from_string_literal("Invalid flow 'id'");
    if (flow.steps.is_empty())
        return Error::from_string_literal("Flow must contain at least one step");

    auto now_ms = UnixDateTime::now().milliseconds_since_epoch();
    if (auto existing = m_flows.get(flow.id); existing.has_value() && existing->created_at_ms > 0)
        flow.created_at_ms = existing->created_at_ms;
    else if (flow.created_at_ms <= 0)
        flow.created_at_ms = now_ms;
    flow.updated_at_ms = now_ms;

    auto serialized = serialize_flow_definition(flow);
    TRY(write_json_file(flow_file_path(flow.id), JsonValue { move(serialized) }));

    m_flows.set(flow.id, flow);
    notify_flows_changed();
    return flow;
}

ErrorOr<void> FlowStore::delete_flow(StringView id)
{
    if (!is_valid_flow_id(id))
        return Error::from_string_literal("Invalid flow 'id'");

    auto path = flow_file_path(id);
    bool existed_on_disk = FileSystem::exists(path);
    if (existed_on_disk)
        TRY(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));

    bool removed_in_memory = m_flows.remove(id);
    if (!existed_on_disk && !removed_in_memory)
        return Error::from_string_literal("Flow not found");

    notify_flows_changed();
    return {};
}

void FlowStore::remove_run_artifacts_from_disk(StringView run_id) const
{
    if (!is_valid_flow_id(run_id))
        return;

    auto json_path = run_file_path(run_id);
    if (FileSystem::exists(json_path))
        (void)FileSystem::remove(json_path, FileSystem::RecursionMode::Disallowed);

    auto runs_dir = runs_directory();
    if (!FileSystem::is_directory(runs_dir))
        return;

    auto screenshot_prefix = ByteString::formatted("{}_step_", run_id);
    Vector<ByteString> files_to_remove;
    (void)Core::Directory::for_each_entry(runs_dir, Core::DirIterator::SkipParentAndBaseDir, [&](Core::DirectoryEntry const& entry, Core::Directory const& parent) -> ErrorOr<IterationDecision> {
        if (entry.type == Core::DirectoryEntry::Type::File
            && entry.name.starts_with(screenshot_prefix)
            && entry.name.ends_with(".png"sv)) {
            files_to_remove.append(parent.path().append(entry.name).string());
        }
        return IterationDecision::Continue;
    });
    for (auto const& file : files_to_remove)
        (void)FileSystem::remove(file, FileSystem::RecursionMode::Disallowed);
}

void FlowStore::upsert_run(FlowRunRecord run, bool persist_to_disk)
{
    if (persist_to_disk && is_valid_flow_id(run.run_id)) {
        auto serialized = serialize_flow_run(run);
        if (auto res = write_json_file(run_file_path(run.run_id), JsonValue { move(serialized) }); res.is_error())
            warnln("Could not persist flow run {}: {}", run.run_id, res.error());
    }

    bool updated = false;
    for (auto& existing : m_runs) {
        if (existing.run_id == run.run_id) {
            existing = move(run);
            updated = true;
            break;
        }
    }
    if (!updated) {
        m_runs.prepend(move(run));
        if (m_runs.size() > max_in_memory_runs) {
            for (size_t i = max_in_memory_runs; i < m_runs.size(); ++i)
                remove_run_artifacts_from_disk(m_runs[i].run_id);
            m_runs.shrink(max_in_memory_runs);
        }
    }

    notify_flow_runs_changed();
}

Vector<FlowRunRecord> FlowStore::list_runs(Optional<StringView> flow_id, size_t limit) const
{
    Vector<FlowRunRecord> out;
    for (auto const& run : m_runs) {
        if (flow_id.has_value() && run.flow_id != *flow_id)
            continue;
        out.append(run);
        if (out.size() >= limit)
            break;
    }
    return out;
}

Optional<FlowRunRecord> FlowStore::get_run(StringView run_id) const
{
    for (auto const& run : m_runs) {
        if (run.run_id == run_id)
            return run;
    }
    return {};
}

void FlowStore::add_observer(FlowStoreObserver& observer)
{
    m_observers.append(observer);
}

void FlowStore::remove_observer(FlowStoreObserver& observer)
{
    m_observers.remove_first_matching([&](auto const& entry) { return &entry == &observer; });
}

void FlowStore::notify_flows_changed()
{
    Vector<FlowStoreObserver*> snapshot;
    snapshot.ensure_capacity(m_observers.size());
    for (auto& observer : m_observers)
        snapshot.unchecked_append(&observer);
    for (auto* observer : snapshot)
        observer->flows_changed();
}

void FlowStore::notify_flow_runs_changed()
{
    Vector<FlowStoreObserver*> snapshot;
    snapshot.ensure_capacity(m_observers.size());
    for (auto& observer : m_observers)
        snapshot.unchecked_append(&observer);
    for (auto* observer : snapshot)
        observer->flow_runs_changed();
}

}
