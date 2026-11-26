/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibWebView/Application.h>
#include <LibWebView/Automation/FlowTypes.h>
#include <LibWebView/Extensions/ExtensionManager.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

static ErrorOr<String> read_extension_text_file(ByteString const& root_path, StringView rel_path)
{
    if (!is_safe_relative_extension_path(rel_path))
        return Error::from_string_literal("Unsafe relative extension path");
    auto full_path = LexicalPath::join(root_path, rel_path).string();
    auto file = TRY(Core::File::open(full_path, Core::File::OpenMode::Read));
    auto bytes = TRY(file->read_until_eof());
    return String::from_utf8(bytes);
}

static bool content_script_matches_url(ExtensionContentScript const& cs, URL::URL const& url)
{
    bool matched = false;
    for (auto const& pattern : cs.matches) {
        if (url_matches_extension_pattern(url, pattern)) {
            matched = true;
            break;
        }
    }
    if (!matched)
        return false;

    for (auto const& exclude : cs.exclude_matches) {
        if (url_matches_extension_pattern(url, exclude))
            return false;
    }
    return true;
}

ExtensionManager::ExtensionManager(Application& application, ExtensionStore& store, FlowStore& flow_store)
    : m_application(application)
    , m_store(store)
    , m_flow_store(flow_store)
{
    m_store.add_observer(*this);
    sync_bundled_flows();
}

ExtensionManager::~ExtensionManager()
{
    m_store.remove_observer(*this);
}

void ExtensionManager::extensions_changed()
{
    if (!m_syncing_flows)
        sync_bundled_flows();
}

void ExtensionManager::sync_bundled_flows()
{
    if (m_syncing_flows)
        return;
    m_syncing_flows = true;

    HashMap<String, InstalledExtension> by_id;
    for (auto const& ext : m_store.list_extensions())
        by_id.set(ext.manifest.id, ext);

    // Delete flows previously owned by removed or disabled extensions.
    Vector<String> stale_ext_ids;
    for (auto const& [ext_id, old_flows] : m_registered_flows_by_ext) {
        auto current = by_id.get(ext_id);
        if (!current.has_value() || !current->enabled) {
            for (auto const& flow_id : old_flows)
                (void)m_flow_store.delete_flow(flow_id);
            stale_ext_ids.append(ext_id);
        }
    }
    for (auto const& ext_id : stale_ext_ids)
        m_registered_flows_by_ext.remove(ext_id);

    for (auto const& ext : m_store.list_extensions()) {
        if (!ext.enabled || ext.manifest.ladybird_flow_files.is_empty())
            continue;

        Vector<String> new_flow_ids;
        for (auto const& rel_path : ext.manifest.ladybird_flow_files) {
            auto raw = read_extension_text_file(ext.root_path, rel_path);
            if (raw.is_error()) {
                m_store.set_last_error(ext.manifest.id, MUST(String::formatted("Failed reading bundled flow '{}': {}", rel_path, raw.error())));
                continue;
            }
            auto json = JsonValue::from_string(raw.value());
            if (json.is_error()) {
                m_store.set_last_error(ext.manifest.id, MUST(String::formatted("Invalid JSON in bundled flow '{}': {}", rel_path, json.error())));
                continue;
            }
            auto parsed = parse_flow_definition(json.value());
            if (parsed.is_error()) {
                m_store.set_last_error(ext.manifest.id, MUST(String::formatted("Invalid flow '{}': {}", rel_path, parsed.error())));
                continue;
            }
            auto saved = m_flow_store.save_flow(parsed.release_value());
            if (saved.is_error()) {
                m_store.set_last_error(ext.manifest.id, MUST(String::formatted("Failed saving bundled flow '{}': {}", rel_path, saved.error())));
                continue;
            }
            new_flow_ids.append(saved.value().id);
        }

        if (auto prev = m_registered_flows_by_ext.get(ext.manifest.id); prev.has_value()) {
            for (auto const& old_id : *prev) {
                if (!new_flow_ids.contains_slow(old_id))
                    (void)m_flow_store.delete_flow(old_id);
            }
        }

        m_store.set_registered_flow_ids(ext.manifest.id, new_flow_ids);
        m_registered_flows_by_ext.set(ext.manifest.id, move(new_flow_ids));
    }

    m_syncing_flows = false;
    m_application.refresh_content_blockers_for_extensions();
}

String ExtensionManager::collect_combined_filter_rules() const
{
    StringBuilder combined;
    for (auto const& ext : m_store.list_extensions()) {
        if (!ext.enabled)
            continue;

        for (auto const& filter_rel : ext.manifest.ladybird_filter_lists) {
            auto text = read_extension_text_file(ext.root_path, filter_rel);
            if (text.is_error()) {
                m_store.set_last_error(ext.manifest.id, MUST(String::formatted("Failed reading filter list '{}': {}", filter_rel, text.error())));
                continue;
            }
            combined.append(text.value());
            if (!text.value().bytes_as_string_view().ends_with('\n'))
                combined.append('\n');
        }

        for (auto const& ruleset : ext.manifest.declarative_net_request_rulesets) {
            if (!ruleset.enabled)
                continue;
            auto raw = read_extension_text_file(ext.root_path, ruleset.path);
            if (raw.is_error()) {
                m_store.set_last_error(ext.manifest.id, MUST(String::formatted("Failed reading DNR ruleset '{}': {}", ruleset.path, raw.error())));
                continue;
            }
            auto json = JsonValue::from_string(raw.value());
            if (json.is_error()) {
                m_store.set_last_error(ext.manifest.id, MUST(String::formatted("Invalid JSON in DNR ruleset '{}': {}", ruleset.path, json.error())));
                continue;
            }
            auto translated = translate_dnr_ruleset_to_adblock_rules(json.value());
            if (translated.is_error()) {
                m_store.set_last_error(ext.manifest.id, MUST(String::formatted("Invalid DNR ruleset '{}': {}", ruleset.path, translated.error())));
                continue;
            }
            combined.append(translated.value());
        }
    }
    return MUST(combined.to_string());
}

void ExtensionManager::did_start_load(ViewImplementation& view, URL::URL const& url)
{
    if (view.owner_view_id().has_value())
        return;
    if (url.scheme() != "http"sv && url.scheme() != "https"sv)
        return;

    bool is_private = view.is_private() == IsPrivate::Yes;
    for (auto const& ext : m_store.list_extensions()) {
        if (!ext.enabled)
            continue;
        if (is_private && !ext.allow_in_private)
            continue;
        inject_extension_into_view(ext, view, url, true, InjectionPhase::DocumentStart);
    }
}

void ExtensionManager::did_finish_load(ViewImplementation& view, URL::URL const& url)
{
    if (view.owner_view_id().has_value())
        return;
    if (url.scheme() != "http"sv && url.scheme() != "https"sv)
        return;

    bool is_private = view.is_private() == IsPrivate::Yes;
    for (auto const& ext : m_store.list_extensions()) {
        if (!ext.enabled)
            continue;
        if (is_private && !ext.allow_in_private)
            continue;
        inject_extension_into_view(ext, view, url, true, InjectionPhase::DocumentEndOrIdle);
    }
}

ErrorOr<size_t> ExtensionManager::inject_extension_into_active_tab(StringView extension_id)
{
    auto ext = m_store.get_extension(extension_id);
    if (!ext.has_value())
        return Error::from_string_literal("Extension not found");
    if (!ext->enabled)
        return Error::from_string_literal("Extension is disabled");

    auto view = m_application.active_web_view();
    if (!view.has_value())
        return Error::from_string_literal("No active browser tab");
    if (view->url().scheme() == "about"sv)
        return Error::from_string_literal("Cannot inject extension into internal about: page");
    if (view->is_private() == IsPrivate::Yes && !ext->allow_in_private)
        return Error::from_string_literal("Extension is not allowed in private tabs");

    return inject_extension_into_view(*ext, *view, view->url(), false, InjectionPhase::All);
}

size_t ExtensionManager::inject_extension_into_view(InstalledExtension const& ext, ViewImplementation& view, URL::URL const& url, bool match_url, InjectionPhase phase)
{
    size_t injected_groups = 0;
    auto storage_snapshot = m_store.get_storage(ext.manifest.id);

    for (auto const& cs : ext.manifest.content_scripts) {
        if (phase == InjectionPhase::DocumentStart && cs.run_at != "document_start"sv)
            continue;
        if (phase == InjectionPhase::DocumentEndOrIdle && cs.run_at == "document_start"sv)
            continue;
        if (match_url && !content_script_matches_url(cs, url))
            continue;

        Vector<String> css_contents;
        Vector<String> js_contents;
        bool read_ok = true;

        for (auto const& css_rel : cs.css) {
            auto text = read_extension_text_file(ext.root_path, css_rel);
            if (text.is_error()) {
                m_store.set_last_error(ext.manifest.id, MUST(String::formatted("Failed reading CSS '{}': {}", css_rel, text.error())));
                read_ok = false;
                break;
            }
            css_contents.append(text.release_value());
        }
        if (!read_ok)
            continue;

        for (auto const& js_rel : cs.js) {
            auto text = read_extension_text_file(ext.root_path, js_rel);
            if (text.is_error()) {
                m_store.set_last_error(ext.manifest.id, MUST(String::formatted("Failed reading JS '{}': {}", js_rel, text.error())));
                read_ok = false;
                break;
            }
            js_contents.append(text.release_value());
        }
        if (!read_ok)
            continue;

        auto script = build_extension_injection_script(ext.manifest.id, ext.manifest.name, css_contents, js_contents, storage_snapshot);
        JsonObject payload;
        payload.set("script"sv, move(script));
        payload.set("args"sv, JsonArray {});

        m_application.enqueue_automation_webdriver_command(view.view_id(), "execute_script"_string, move(payload), {}, false, [weak_self = make_weak_ptr<ExtensionManager>(), ext_id = ext.manifest.id](Web::WebDriver::Response response) {
            if (!weak_self)
                return;
            if (response.is_error()) {
                weak_self->m_store.set_last_error(ext_id, MUST(String::formatted("Injection WebDriver error: {}", response.error().message)));
                return;
            }
            if (response.value().is_object()) {
                auto const& obj = response.value().as_object();
                if (!obj.get_bool("ok"sv).value_or(true)) {
                    auto errors = obj.get_array("errors"sv);
                    if (errors.has_value() && !errors->is_empty()) {
                        weak_self->m_store.set_last_error(ext_id, errors->at(0).as_string());
                        return;
                    }
                }
            }
        });
        ++injected_groups;
    }

    return injected_groups;
}

}
