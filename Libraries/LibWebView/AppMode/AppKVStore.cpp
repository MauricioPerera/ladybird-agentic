/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibWebView/AppMode/AppKVStore.h>

namespace WebView {

static constexpr u32 APP_KV_SCHEMA_VERSION = 1u;

bool AppKVStore::is_valid_key(StringView key)
{
    if (key.is_empty() || key.length() > MAX_KEY_LENGTH)
        return false;
    return !key.contains('\0');
}

ErrorOr<NonnullOwnPtr<AppKVStore>> AppKVStore::create(ByteString const& data_directory)
{
    TRY(Core::Directory::create(data_directory, Core::Directory::CreateDirectories::Yes, 0700));
    auto database = TRY(Database::Database::create(data_directory, "AppStore"sv));

    auto migrations = to_array<Database::Migration>({
        {
            .version = APP_KV_SCHEMA_VERSION,
            .sql = R"#(
                CREATE TABLE IF NOT EXISTS AppKV (
                    key TEXT PRIMARY KEY,
                    value_json TEXT NOT NULL,
                    updated_at INTEGER NOT NULL
                );
            )#"sv,
        },
    });

    auto outcome = TRY(database->migrate("AppKV"sv, migrations));
    if (outcome != Database::MigrationOutcome::Success)
        return Error::from_string_literal("AppKV database schema version mismatch");

    Statements statements {};
    statements.get = TRY(database->prepare_statement("SELECT value_json, updated_at FROM AppKV WHERE key = ?;"sv));
    statements.set = TRY(database->prepare_statement("INSERT OR REPLACE INTO AppKV (key, value_json, updated_at) VALUES (?, ?, ?);"sv));
    statements.remove = TRY(database->prepare_statement("DELETE FROM AppKV WHERE key = ?;"sv));
    statements.list_all = TRY(database->prepare_statement("SELECT key, updated_at, LENGTH(CAST(value_json AS BLOB)) FROM AppKV ORDER BY key ASC;"sv));
    statements.clear_all = TRY(database->prepare_statement("DELETE FROM AppKV;"sv));

    return adopt_own(*new AppKVStore(move(database), statements));
}

AppKVStore::AppKVStore(NonnullRefPtr<Database::Database> database, Statements statements)
    : m_database(move(database))
    , m_statements(statements)
{
}

ErrorOr<Optional<AppKVEntry>> AppKVStore::get(StringView key)
{
    if (!is_valid_key(key))
        return Error::from_string_literal("Invalid KV key");

    auto key_str = TRY(String::from_utf8(key));
    Optional<String> raw_json;
    i64 updated_at = 0;

    TRY(m_database->try_execute_statement(
        m_statements.get,
        [&](Database::StatementID statement_id) -> ErrorOr<void> {
            raw_json = m_database->result_column<String>(statement_id, 0);
            updated_at = m_database->result_column<i64>(statement_id, 1);
            return {};
        },
        key_str));

    if (!raw_json.has_value())
        return Optional<AppKVEntry> {};

    auto parsed = TRY(JsonValue::from_string(*raw_json));
    return AppKVEntry {
        .key = move(key_str),
        .value = move(parsed),
        .updated_at_ms = updated_at,
    };
}

ErrorOr<i64> AppKVStore::set(StringView key, JsonValue const& value)
{
    if (!is_valid_key(key))
        return Error::from_string_literal("Invalid KV key");

    auto key_str = TRY(String::from_utf8(key));
    auto serialized = value.serialized();
    if (serialized.bytes().size() > MAX_VALUE_BYTES)
        return Error::from_string_literal("KV value exceeds 4 MiB serialized limit");

    auto now_ms = UnixDateTime::now().milliseconds_since_epoch();
    TRY(m_database->try_execute_statement(m_statements.set, {}, key_str, serialized, now_ms));
    return now_ms;
}

ErrorOr<bool> AppKVStore::remove(StringView key)
{
    if (!is_valid_key(key))
        return Error::from_string_literal("Invalid KV key");

    auto key_str = TRY(String::from_utf8(key));
    bool existed = false;
    TRY(m_database->try_execute_statement(
        m_statements.get,
        [&](Database::StatementID) -> ErrorOr<void> {
            existed = true;
            return {};
        },
        key_str));

    if (!existed)
        return false;

    TRY(m_database->try_execute_statement(m_statements.remove, {}, key_str));
    return true;
}

ErrorOr<Vector<AppKVKeySummary>> AppKVStore::list(Optional<StringView> prefix)
{
    Vector<AppKVKeySummary> result;
    TRY(m_database->try_execute_statement(
        m_statements.list_all,
        [&](Database::StatementID statement_id) -> ErrorOr<void> {
            auto key = m_database->result_column<String>(statement_id, 0);
            if (prefix.has_value() && !prefix->is_empty() && !key.bytes_as_string_view().starts_with(*prefix))
                return {};
            if (result.size() >= MAX_LIST_KEYS)
                return {};
            auto updated_at = m_database->result_column<i64>(statement_id, 1);
            auto bytes = m_database->result_column<i64>(statement_id, 2);
            result.append(AppKVKeySummary {
                .key = move(key),
                .updated_at_ms = updated_at,
                .bytes = bytes > 0 ? static_cast<u64>(bytes) : 0,
            });
            return {};
        }));
    return result;
}

ErrorOr<u64> AppKVStore::clear(Optional<StringView> prefix)
{
    auto keys = TRY(list(prefix));
    if (keys.is_empty())
        return 0;

    if (!prefix.has_value() || prefix->is_empty()) {
        TRY(m_database->try_execute_statement(m_statements.clear_all, {}));
        return static_cast<u64>(keys.size());
    }

    TRY(m_database->transaction([&]() -> ErrorOr<void> {
        for (auto const& item : keys)
            TRY(m_database->try_execute_statement(m_statements.remove, {}, item.key));
        return {};
    }));

    return static_cast<u64>(keys.size());
}

}
