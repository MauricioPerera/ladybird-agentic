/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/LexicalPath.h>
#include <AK/QuickSort.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Extensions/ExtensionStore.h>
#include <LibWebView/Utilities.h>

namespace WebView {

static ErrorOr<JsonObject> read_strict_json_object(ByteString const& path)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
    auto bytes = TRY(file->read_until_eof());
    auto json = TRY(JsonValue::from_string(bytes));
    if (!json.is_object())
        return Error::from_string_literal("Expected JSON object");
    return move(json.as_object());
}

static ErrorOr<void> verify_manifest_files_exist(ByteString const& root_path, ExtensionManifest const& manifest)
{
    auto check_path = [&](StringView rel_path) -> ErrorOr<void> {
        TRY(resolve_safe_extension_file_path(root_path, rel_path));
        return {};
    };

    for (auto const& cs : manifest.content_scripts) {
        for (auto const& js : cs.js)
            TRY(check_path(js));
        for (auto const& css : cs.css)
            TRY(check_path(css));
    }
    if (manifest.action.has_value() && manifest.action->default_popup.has_value())
        TRY(check_path(*manifest.action->default_popup));
    for (auto const& flow_file : manifest.ladybird_flow_files)
        TRY(check_path(flow_file));
    for (auto const& filter_file : manifest.ladybird_filter_lists)
        TRY(check_path(filter_file));
    for (auto const& ruleset : manifest.declarative_net_request_rulesets)
        TRY(check_path(ruleset.path));

    return {};
}

ExtensionStore::ExtensionStore(ByteString storage_directory)
    : m_storage_directory(move(storage_directory))
{
    reload();
}

ExtensionStore::~ExtensionStore() = default;

ByteString ExtensionStore::registry_file_path() const
{
    return LexicalPath::join(m_storage_directory, "registry.json"sv).string();
}

ByteString ExtensionStore::storage_file_path(StringView id) const
{
    return LexicalPath::join(m_storage_directory, "storage"sv, ByteString::formatted("{}.json", id)).string();
}

void ExtensionStore::reload()
{
    m_extensions.clear();

    auto reg_path = registry_file_path();
    if (!FileSystem::exists(reg_path))
        return;

    auto reg_or = read_strict_json_object(reg_path);
    if (reg_or.is_error()) {
        warnln("Could not read extension registry {}: {}", reg_path, reg_or.error());
        return;
    }

    auto entries = reg_or.value().get_array("extensions"sv);
    if (!entries.has_value())
        return;

    for (auto const& item : entries->values()) {
        if (!item.is_object())
            continue;
        auto const& entry = item.as_object();
        auto id = entry.get_string("id"sv);
        auto root_path = entry.get_string("root_path"sv);
        if (!id.has_value() || !root_path.has_value() || !is_valid_extension_id(*id))
            continue;

        InstalledExtension ext;
        ext.root_path = root_path->to_byte_string();
        ext.enabled = entry.get_bool("enabled"sv).value_or(true);
        ext.allow_in_private = entry.get_bool("allow_in_private"sv).value_or(false);
        ext.installed_at_ms = entry.get_integer<i64>("installed_at_ms"sv).value_or(0);
        ext.updated_at_ms = entry.get_integer<i64>("updated_at_ms"sv).value_or(0);
        ext.last_error = entry.get_string("last_error"sv).copy();

        auto manifest_path = LexicalPath::join(ext.root_path, "manifest.json"sv).string();
        auto manifest_json = read_strict_json_object(manifest_path);
        if (manifest_json.is_error()) {
            ext.manifest.id = *id;
            ext.manifest.name = entry.get_string("name"sv).value_or(*id);
            ext.manifest.version = entry.get_string("version"sv).value_or("0.0.0"_string);
            ext.last_error = MUST(String::formatted("Failed reading manifest.json: {}", manifest_json.error()));
        } else {
            auto parsed = parse_extension_manifest(JsonValue { move(manifest_json.value()) }, *id);
            if (parsed.is_error()) {
                ext.manifest.id = *id;
                ext.manifest.name = entry.get_string("name"sv).value_or(*id);
                ext.manifest.version = entry.get_string("version"sv).value_or("0.0.0"_string);
                ext.last_error = MUST(String::formatted("Invalid manifest.json: {}", parsed.error()));
            } else {
                ext.manifest = parsed.release_value();
                if (auto files_res = verify_manifest_files_exist(ext.root_path, ext.manifest); files_res.is_error())
                    ext.last_error = MUST(String::formatted("Missing extension file: {}", files_res.error()));
            }
        }

        m_extensions.set(ext.manifest.id, move(ext));
    }
}

ErrorOr<void> ExtensionStore::save_registry()
{
    TRY(Core::Directory::create(m_storage_directory, Core::Directory::CreateDirectories::Yes));

    JsonArray entries;
    for (auto const& ext : list_extensions()) {
        JsonObject entry;
        entry.set("id"sv, ext.manifest.id);
        entry.set("name"sv, ext.manifest.name);
        entry.set("version"sv, ext.manifest.version);
        entry.set("root_path"sv, String::from_utf8_without_validation(ext.root_path.bytes()));
        entry.set("enabled"sv, ext.enabled);
        entry.set("allow_in_private"sv, ext.allow_in_private);
        entry.set("installed_at_ms"sv, ext.installed_at_ms);
        entry.set("updated_at_ms"sv, ext.updated_at_ms);
        if (ext.last_error.has_value())
            entry.set("last_error"sv, *ext.last_error);
        entries.must_append(move(entry));
    }

    JsonObject root;
    root.set("schema_version"sv, 1);
    root.set("extensions"sv, move(entries));
    return write_json_file(registry_file_path(), JsonValue { move(root) });
}

Vector<InstalledExtension> ExtensionStore::list_extensions() const
{
    Vector<InstalledExtension> result;
    result.ensure_capacity(m_extensions.size());
    for (auto const& [_, ext] : m_extensions)
        result.unchecked_append(ext);
    quick_sort(result, [](InstalledExtension const& a, InstalledExtension const& b) {
        if (a.manifest.name != b.manifest.name)
            return a.manifest.name < b.manifest.name;
        return a.manifest.id < b.manifest.id;
    });
    return result;
}

Optional<InstalledExtension> ExtensionStore::get_extension(StringView id) const
{
    if (auto it = m_extensions.get(id); it.has_value())
        return *it;
    return {};
}

ErrorOr<InstalledExtension> ExtensionStore::install_unpacked(ByteString directory_path)
{
    auto trimmed = directory_path.view().trim_whitespace();
    if (trimmed.is_empty())
        return Error::from_string_literal("Extension directory path cannot be empty");

    auto normalized = LexicalPath::canonicalized_path(trimmed);
    if (normalized.is_empty() || !FileSystem::is_directory(normalized))
        return Error::from_string_literal("Extension path is not an existing directory");

    auto manifest_path = LexicalPath::join(normalized, "manifest.json"sv).string();
    if (!FileSystem::exists(manifest_path))
        return Error::from_string_literal("Directory does not contain manifest.json");

    auto manifest_obj = TRY(read_strict_json_object(manifest_path));
    auto folder_path = LexicalPath { normalized };
    auto fallback_id = sanitize_extension_id_from_name(folder_path.basename());
    auto manifest = TRY(parse_extension_manifest(JsonValue { move(manifest_obj) }, fallback_id));
    TRY(verify_manifest_files_exist(normalized, manifest));

    auto now = UnixDateTime::now().milliseconds_since_epoch();
    InstalledExtension ext;
    if (auto existing = m_extensions.get(manifest.id); existing.has_value()) {
        ext.enabled = existing->enabled;
        ext.allow_in_private = existing->allow_in_private;
        ext.installed_at_ms = existing->installed_at_ms;
    } else {
        ext.enabled = true;
        ext.allow_in_private = false;
        ext.installed_at_ms = now;
    }
    ext.manifest = move(manifest);
    ext.root_path = move(normalized);
    ext.updated_at_ms = now;
    ext.last_error = {};

    m_extensions.set(ext.manifest.id, ext);
    TRY(save_registry());
    notify_extensions_changed();
    return m_extensions.get(ext.manifest.id).value();
}

ErrorOr<InstalledExtension> ExtensionStore::reload_extension(StringView id)
{
    auto existing = m_extensions.get(id);
    if (!existing.has_value())
        return Error::from_string_literal("Extension not found");

    auto manifest_path = LexicalPath::join(existing->root_path, "manifest.json"sv).string();
    auto manifest_obj = read_strict_json_object(manifest_path);
    if (manifest_obj.is_error()) {
        auto err = MUST(String::formatted("Failed reading manifest.json: {}", manifest_obj.error()));
        set_last_error(id, err);
        return manifest_obj.release_error();
    }

    auto parsed = parse_extension_manifest(JsonValue { move(manifest_obj.value()) }, id);
    if (parsed.is_error()) {
        auto err = MUST(String::formatted("Invalid manifest.json: {}", parsed.error()));
        set_last_error(id, err);
        return parsed.release_error();
    }

    if (parsed.value().id != existing->manifest.id)
        return Error::from_string_literal("Reloaded manifest.json changed extension id");

    if (auto files_res = verify_manifest_files_exist(existing->root_path, parsed.value()); files_res.is_error()) {
        auto err = MUST(String::formatted("Missing extension file: {}", files_res.error()));
        set_last_error(id, err);
        return files_res.release_error();
    }

    InstalledExtension updated = *existing;
    updated.manifest = parsed.release_value();
    updated.updated_at_ms = UnixDateTime::now().milliseconds_since_epoch();
    updated.last_error = {};
    m_extensions.set(updated.manifest.id, updated);
    TRY(save_registry());
    notify_extensions_changed();
    return m_extensions.get(updated.manifest.id).value();
}

ErrorOr<InstalledExtension> ExtensionStore::set_enabled(StringView id, bool enabled)
{
    auto existing = m_extensions.get(id);
    if (!existing.has_value())
        return Error::from_string_literal("Extension not found");

    InstalledExtension updated = *existing;
    updated.enabled = enabled;
    updated.updated_at_ms = UnixDateTime::now().milliseconds_since_epoch();
    m_extensions.set(updated.manifest.id, updated);
    TRY(save_registry());
    notify_extensions_changed();
    return m_extensions.get(updated.manifest.id).value();
}

ErrorOr<InstalledExtension> ExtensionStore::set_allow_in_private(StringView id, bool allow)
{
    auto existing = m_extensions.get(id);
    if (!existing.has_value())
        return Error::from_string_literal("Extension not found");

    InstalledExtension updated = *existing;
    updated.allow_in_private = allow;
    updated.updated_at_ms = UnixDateTime::now().milliseconds_since_epoch();
    m_extensions.set(updated.manifest.id, updated);
    TRY(save_registry());
    notify_extensions_changed();
    return m_extensions.get(updated.manifest.id).value();
}

ErrorOr<void> ExtensionStore::remove_extension(StringView id)
{
    if (!is_valid_extension_id(id))
        return Error::from_string_literal("Invalid extension id");
    if (!m_extensions.remove(id))
        return Error::from_string_literal("Extension not found");

    auto storage_path = storage_file_path(id);
    if (FileSystem::exists(storage_path))
        (void)FileSystem::remove(storage_path, FileSystem::RecursionMode::Disallowed);

    TRY(save_registry());
    notify_extensions_changed();
    return {};
}

void ExtensionStore::set_last_error(StringView id, Optional<String> error)
{
    auto existing = m_extensions.get(id);
    if (!existing.has_value())
        return;
    if (existing->last_error == error)
        return;

    InstalledExtension updated = *existing;
    updated.last_error = move(error);
    m_extensions.set(updated.manifest.id, move(updated));
    (void)save_registry();
    notify_extensions_changed();
}

void ExtensionStore::set_registered_flow_ids(StringView id, Vector<String> flow_ids)
{
    auto existing = m_extensions.get(id);
    if (!existing.has_value())
        return;
    InstalledExtension updated = *existing;
    updated.registered_flow_ids = move(flow_ids);
    m_extensions.set(updated.manifest.id, move(updated));
}

JsonObject ExtensionStore::get_storage(StringView id) const
{
    if (!is_valid_extension_id(id))
        return {};
    auto path = storage_file_path(id);
    if (!FileSystem::exists(path))
        return {};
    auto obj = read_strict_json_object(path);
    if (obj.is_error())
        return {};
    return obj.release_value();
}

ErrorOr<void> ExtensionStore::set_storage(StringView id, JsonObject data)
{
    if (!m_extensions.contains(id))
        return Error::from_string_literal("Extension not found");
    return write_json_file(storage_file_path(id), JsonValue { move(data) });
}

ErrorOr<InstalledExtension> ExtensionStore::create_example_extension()
{
    auto example_dir = LexicalPath::join(m_storage_directory, "examples"sv, "page-inspector"sv).string();
    auto flows_dir = LexicalPath::join(example_dir, "flows"sv).string();
    TRY(Core::Directory::create(flows_dir, Core::Directory::CreateDirectories::Yes));

    auto write_text = [](ByteString const& path, StringView content) -> ErrorOr<void> {
        auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
        return file->write_until_depleted(content.bytes());
    };

    TRY(write_text(LexicalPath::join(example_dir, "manifest.json"sv).string(), R"JSON({
  "manifest_version": 3,
  "id": "ladybird-page-inspector",
  "name": "Page Inspector",
  "version": "1.0.0",
  "description": "Adds an unobtrusive link/heading badge on web pages and bundles a sample automation flow.",
  "permissions": ["storage", "flows"],
  "host_permissions": ["http://*/*", "https://*/*"],
  "content_scripts": [
    {
      "matches": ["http://*/*", "https://*/*"],
      "css": ["content.css"],
      "js": ["content.js"],
      "run_at": "document_idle"
    }
  ],
  "ladybird": {
    "flows": ["flows/inspect-page.json"]
  }
}
)JSON"sv));

    TRY(write_text(LexicalPath::join(example_dir, "content.css"sv).string(), R"CSS(#ladybird-ext-inspector-badge {
  position: fixed;
  right: 14px;
  bottom: 14px;
  z-index: 2147483647;
  padding: 6px 10px;
  border-radius: 999px;
  background: rgba(15, 23, 42, 0.88);
  color: #f8fafc;
  font: 12px/1.3 system-ui, -apple-system, sans-serif;
  border: 1px solid rgba(148, 163, 184, 0.35);
  box-shadow: 0 4px 14px rgba(0, 0, 0, 0.28);
  cursor: pointer;
}
)CSS"sv));

    TRY(write_text(LexicalPath::join(example_dir, "content.js"sv).string(), R"JS((function() {
  if (!document.body) return;
  var existing = document.getElementById('ladybird-ext-inspector-badge');
  if (existing) existing.remove();
  var links = document.querySelectorAll('a').length;
  var headings = document.querySelectorAll('h1,h2,h3').length;
  ladybirdExt.storage.local.get({ labelPrefix: 'Inspector' }).then(function(cfg) {
    var badge = document.createElement('button');
    badge.id = 'ladybird-ext-inspector-badge';
    badge.type = 'button';
    badge.textContent = cfg.labelPrefix + ': ' + headings + 'h / ' + links + 'a';
    badge.title = 'Click to dismiss (Ladybird extension example)';
    badge.addEventListener('click', function() { badge.remove(); });
    document.body.appendChild(badge);
  });
})();
)JS"sv));

    TRY(write_text(LexicalPath::join(flows_dir, "inspect-page.json"sv).string(), R"JSON({
  "schema_version": 1,
  "id": "ext-page-inspector-smoke",
  "name": "Extension: Page Inspector Smoke",
  "description": "Bundled by the Page Inspector example extension.",
  "variables": {
    "target_url": "http://example.com"
  },
  "steps": [
    { "action": "open", "url": "{{target_url}}" },
    { "action": "wait_for", "locator": { "kind": "css", "value": "#ladybird-ext-inspector-badge" } },
    { "action": "extract", "locator": { "kind": "css", "value": "#ladybird-ext-inspector-badge" }, "save_as": "badge_text" }
  ]
}
)JSON"sv));

    return install_unpacked(move(example_dir));
}

void ExtensionStore::add_observer(ExtensionStoreObserver& observer)
{
    m_observers.append(observer);
}

void ExtensionStore::remove_observer(ExtensionStoreObserver& observer)
{
    m_observers.remove_first_matching([&](auto const& entry) {
        return &entry == &observer;
    });
}

void ExtensionStore::notify_extensions_changed()
{
    for (size_t i = 0; i < m_observers.size(); ++i)
        m_observers[i].extensions_changed();
}

}
