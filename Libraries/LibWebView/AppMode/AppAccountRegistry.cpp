/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/AppMode/AppAccountRegistry.h>
#include <LibWebView/AppMode/AppManifest.h>

namespace WebView {

String AppAccountRegistry::default_label_for_account(StringView account_id)
{
    if (account_id.is_empty() || account_id == "default"sv)
        return "Default"_string;

    StringBuilder sb;
    bool uppercase_next = true;
    for (auto ch : account_id) {
        if (ch == '-' || ch == '_') {
            sb.append(' ');
            uppercase_next = true;
            continue;
        }
        if (uppercase_next && is_ascii_lower_alpha(ch)) {
            sb.append(static_cast<char>(ch - ('a' - 'A')));
            uppercase_next = false;
        } else {
            sb.append(ch);
            uppercase_next = false;
        }
    }
    auto built = sb.to_string_without_validation();
    return built.is_empty() ? "Default"_string : built;
}

ErrorOr<Vector<AppAccountRecord>> AppAccountRegistry::list(ByteString const& registry_path)
{
    Vector<AppAccountRecord> records;
    if (!FileSystem::exists(registry_path))
        return records;

    auto file = TRY(Core::File::open(registry_path, Core::File::OpenMode::Read));
    auto bytes = TRY(file->read_until_eof());
    auto json = TRY(JsonValue::from_string(bytes));
    if (!json.is_object())
        return records;

    auto accounts_array = json.as_object().get_array("accounts"sv);
    if (!accounts_array.has_value())
        return records;

    for (auto const& item : accounts_array->values()) {
        if (!item.is_object())
            continue;
        auto const& obj = item.as_object();
        auto id = obj.get_string("id"sv);
        if (!id.has_value() || !AppManifest::is_valid_app_id(*id))
            continue;
        auto label = obj.get_string("label"sv).copy().value_or_lazy_evaluated([&] { return default_label_for_account(*id); });
        auto profile_name = obj.get_string("profileName"sv);
        if (!profile_name.has_value() || profile_name->is_empty())
            continue;
        auto created_at = obj.get_i64("createdAtMs"sv).value_or(0);
        auto last_used_at = obj.get_i64("lastUsedAtMs"sv).value_or(created_at);

        records.append(AppAccountRecord {
            .id = *id,
            .label = label.is_empty() ? default_label_for_account(*id) : label,
            .profile_name = *profile_name,
            .created_at_ms = created_at,
            .last_used_at_ms = last_used_at,
        });
    }

    return records;
}

ErrorOr<Optional<AppAccountRecord>> AppAccountRegistry::find(ByteString const& registry_path, StringView account_id)
{
    auto records = TRY(list(registry_path));
    for (auto const& record : records) {
        if (record.id == account_id)
            return record;
    }
    return Optional<AppAccountRecord> {};
}

ErrorOr<AppAccountRecord> AppAccountRegistry::upsert(
    ByteString const& registry_path,
    StringView app_id,
    StringView account_id,
    Optional<StringView> label,
    bool touch_last_used)
{
    if (!AppManifest::is_valid_app_id(app_id))
        return Error::from_string_literal("Invalid app id for account registry");
    if (!AppManifest::is_valid_app_id(account_id))
        return Error::from_string_literal("Account id must start with [a-z0-9] and contain only [a-z0-9_-] (1..64 chars)");

    auto now_ms = UnixDateTime::now().milliseconds_since_epoch();
    auto existing_records = TRY(list(registry_path));
    auto profile_name = AppManifest::profile_name_for_account(app_id, account_id);

    Optional<AppAccountRecord> result;
    for (auto& record : existing_records) {
        if (record.id != account_id)
            continue;
        record.profile_name = profile_name;
        if (label.has_value() && !label->trim_whitespace().is_empty()) {
            auto trimmed = label->trim_whitespace();
            if (trimmed.length() > 64)
                trimmed = trimmed.substring_view(0, 64);
            record.label = TRY(String::from_utf8(trimmed));
        }
        if (touch_last_used)
            record.last_used_at_ms = now_ms;
        if (record.created_at_ms == 0)
            record.created_at_ms = now_ms;
        result = record;
        break;
    }

    if (!result.has_value()) {
        String resolved_label = default_label_for_account(account_id);
        if (label.has_value() && !label->trim_whitespace().is_empty()) {
            auto trimmed = label->trim_whitespace();
            if (trimmed.length() > 64)
                trimmed = trimmed.substring_view(0, 64);
            resolved_label = TRY(String::from_utf8(trimmed));
        }
        AppAccountRecord created {
            .id = TRY(String::from_utf8(account_id)),
            .label = move(resolved_label),
            .profile_name = move(profile_name),
            .created_at_ms = now_ms,
            .last_used_at_ms = touch_last_used ? now_ms : 0,
        };
        existing_records.append(created);
        result = move(created);
    }

    auto parent_dir = LexicalPath { registry_path }.parent();
    TRY(Core::Directory::create(parent_dir, Core::Directory::CreateDirectories::Yes, 0700));

    JsonArray accounts_json;
    for (auto const& record : existing_records) {
        JsonObject item;
        item.set("id"sv, record.id);
        item.set("label"sv, record.label);
        item.set("profileName"sv, record.profile_name);
        item.set("createdAtMs"sv, record.created_at_ms);
        item.set("lastUsedAtMs"sv, record.last_used_at_ms);
        accounts_json.must_append(move(item));
    }

    JsonObject root;
    root.set("appId"sv, MUST(String::from_utf8(app_id)));
    root.set("accounts"sv, move(accounts_json));

    auto serialized = root.serialized();
    auto file = TRY(Core::File::open(registry_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(file->write_until_depleted(serialized.bytes()));

    return *result;
}

}
