/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibWebView/Application.h>
#include <LibWebView/Skills/SkillTypes.h>
#include <LibWebView/ViewImplementation.h>
#include <LibWebView/WebUI/SkillsUI.h>

namespace WebView {

SkillsUI::~SkillsUI()
{
    if (m_store)
        m_store->remove_observer(*this);
}

void SkillsUI::register_interfaces()
{
    auto& store = Application::the().skill_store();
    store.add_observer(*this);
    m_store = store.make_weak_ptr();

    register_interface("loadState"sv, [this](auto const&) {
        send_state();
    });
    register_interface("saveSkill"sv, [this](auto const& data) {
        save_skill(data);
    });
    register_interface("setSkillEnabled"sv, [this](auto const& data) {
        set_skill_enabled(data);
    });
    register_interface("deleteSkill"sv, [this](auto const& data) {
        delete_skill(data);
    });
    register_interface("createExampleSkill"sv, [this](auto const& data) {
        create_example_skill(data);
    });
}

void SkillsUI::skills_changed()
{
    if (has_client())
        send_state();
}

void SkillsUI::send_state()
{
    auto& app = Application::the();
    auto& store = app.skill_store();

    JsonArray skills_json;
    for (auto const& skill : store.list_skills(true)) {
        auto supporting = store.read_supporting_files(skill.name).value_or({});
        skills_json.must_append(serialize_skill_for_ui(skill, supporting));
    }

    JsonArray tabs_json;
    auto active = app.active_web_view();
    Optional<u64> active_id = active.has_value() ? Optional<u64> { active->view_id() } : OptionalNone {};
    ViewImplementation::for_each_view([&](ViewImplementation& view) {
        if (view.owner_view_id().has_value() || view.is_private() == IsPrivate::Yes)
            return IterationDecision::Continue;
        if (!view.url().scheme().is_one_of("http"sv, "https"sv))
            return IterationDecision::Continue;

        auto matching = store.matching_skills_for_url(view.url());
        JsonObject tab;
        tab.set("id"sv, view.view_id());
        tab.set("title"sv, view.title().to_well_formed_utf8());
        tab.set("url"sv, view.url().to_string());
        tab.set("suggestedPattern"sv, suggest_match_pattern_for_url(view.url()));
        tab.set("isActive"sv, active_id.has_value() && *active_id == view.view_id());
        tab.set("mcpAllowed"sv, view.is_mcp_access_allowed());
        tab.set("matchingSkillCount"sv, static_cast<u64>(matching.size()));
        tabs_json.must_append(move(tab));
        return IterationDecision::Continue;
    });

    JsonObject mcp_json;
    bool mcp_enabled = app.mcp_enabled();
    mcp_json.set("enabled"sv, mcp_enabled);
    mcp_json.set("allowSkillWrite"sv, Application::browser_options().mcp_allow_skill_write);
    if (auto port = app.mcp_port(); port.has_value()) {
        mcp_json.set("port"sv, static_cast<u64>(*port));
        mcp_json.set("endpoint"sv, MUST(String::formatted("http://127.0.0.1:{}/mcp", *port)));
    }

    JsonObject state;
    state.set("skills"sv, move(skills_json));
    state.set("tabs"sv, move(tabs_json));
    state.set("mcp"sv, move(mcp_json));
    state.set("storageDirectory"sv, String::from_utf8_without_validation(store.storage_directory().bytes()));

    async_send_message("stateUpdated"sv, move(state));
}

void SkillsUI::send_action_result(bool ok, Optional<String> message, Optional<String> skill_name)
{
    JsonObject result;
    result.set("ok"sv, ok);
    if (message.has_value())
        result.set("message"sv, *message);
    if (skill_name.has_value())
        result.set("skillName"sv, *skill_name);
    async_send_message("actionResult"sv, move(result));
}

void SkillsUI::save_skill(JsonValue const& data)
{
    Vector<SkillSupportingFile> supporting_files;
    auto parsed = parse_skill_json(data, supporting_files);
    if (parsed.is_error()) {
        send_action_result(false, MUST(String::formatted("Invalid skill: {}", parsed.error())));
        return;
    }

    auto saved = Application::the().skill_store().save_skill(parsed.release_value(), move(supporting_files));
    if (saved.is_error()) {
        send_action_result(false, MUST(String::formatted("Failed to save skill: {}", saved.error())));
        return;
    }

    send_action_result(true, "Skill saved."_string, saved.value().name);
}

void SkillsUI::set_skill_enabled(JsonValue const& data)
{
    if (!data.is_object()) {
        send_action_result(false, "Expected object payload for setSkillEnabled"_string);
        return;
    }

    auto const& obj = data.as_object();
    auto name = obj.get_string("name"sv);
    auto enabled = obj.get_bool("enabled"sv);
    if (!name.has_value() || !enabled.has_value()) {
        send_action_result(false, "Missing 'name' or 'enabled' field"_string);
        return;
    }

    auto updated = Application::the().skill_store().set_skill_enabled(*name, *enabled);
    if (updated.is_error()) {
        send_action_result(false, MUST(String::formatted("Failed to update skill: {}", updated.error())));
        return;
    }

    send_action_result(true, *enabled ? "Skill enabled."_string : "Skill disabled."_string, updated.value().name);
}

void SkillsUI::delete_skill(JsonValue const& data)
{
    if (!data.is_string()) {
        send_action_result(false, "Expected skill name string"_string);
        return;
    }

    auto name = data.as_string();
    if (auto res = Application::the().skill_store().delete_skill(name); res.is_error()) {
        send_action_result(false, MUST(String::formatted("Failed to delete skill: {}", res.error())));
        return;
    }

    send_action_result(true, "Skill deleted."_string);
}

void SkillsUI::create_example_skill(JsonValue const& data)
{
    Optional<String> site_url;
    if (data.is_object()) {
        if (auto s = data.as_object().get_string("siteUrl"sv); s.has_value() && !s->is_empty())
            site_url = *s;
    }

    auto& store = Application::the().skill_store();
    auto skill = make_example_skill(site_url);
    auto base_name = skill.name;

    if (store.get_skill(skill.name, true).has_value()) {
        for (size_t suffix = 2; suffix < 100; ++suffix) {
            auto candidate = MUST(String::formatted("{}-{}", base_name, suffix));
            if (is_valid_skill_name(candidate) && !store.get_skill(candidate, true).has_value()) {
                skill.name = move(candidate);
                break;
            }
        }
    }

    Vector<SkillSupportingFile> files;
    if (!site_url.has_value()) {
        files.append({
            .relative_path = "references/selectors.md"_string,
            .content = "# Verified YouTube Selectors\n\n"
                       "- Search box: `input[name=\"search_query\"]`\n"
                       "- Video results: `ytd-video-renderer a#video-title`\n"
                       "- YouTube Studio analytics date selector: inspect snapshot for the range menu button instead of navigating to `/period-...` directly.\n"_string,
        });
    }

    auto saved = store.save_skill(move(skill), move(files));
    if (saved.is_error()) {
        send_action_result(false, MUST(String::formatted("Could not create example skill: {}", saved.error())));
        return;
    }

    send_action_result(true, "Example skill created."_string, saved.value().name);
}

}
