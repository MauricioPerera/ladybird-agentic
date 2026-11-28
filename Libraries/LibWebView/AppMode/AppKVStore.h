/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibDatabase/Database.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct WEBVIEW_API AppKVEntry {
    String key;
    JsonValue value;
    i64 updated_at_ms { 0 };
};

struct WEBVIEW_API AppKVKeySummary {
    String key;
    i64 updated_at_ms { 0 };
    u64 bytes { 0 };
};

class WEBVIEW_API AppKVStore final {
public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr size_t MAX_KEY_LENGTH = 256;
    static constexpr size_t MAX_VALUE_BYTES = 4 * 1024 * 1024;
    static constexpr size_t MAX_LIST_KEYS = 10000;

    static ErrorOr<NonnullOwnPtr<AppKVStore>> create(ByteString const& data_directory);
    ~AppKVStore() = default;

    static bool is_valid_key(StringView key);

    ErrorOr<Optional<AppKVEntry>> get(StringView key);
    ErrorOr<i64> set(StringView key, JsonValue const& value);
    ErrorOr<bool> remove(StringView key);
    ErrorOr<Vector<AppKVKeySummary>> list(Optional<StringView> prefix = {});
    ErrorOr<u64> clear(Optional<StringView> prefix = {});

private:
    struct Statements {
        Database::StatementID get { 0 };
        Database::StatementID set { 0 };
        Database::StatementID remove { 0 };
        Database::StatementID list_all { 0 };
        Database::StatementID clear_all { 0 };
    };

    AppKVStore(NonnullRefPtr<Database::Database> database, Statements statements);

    NonnullRefPtr<Database::Database> m_database;
    Statements m_statements;
};

}
