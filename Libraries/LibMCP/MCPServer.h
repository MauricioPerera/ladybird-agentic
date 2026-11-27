/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashTable.h>
#include <AK/JsonValue.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibCore/Forward.h>
#include <LibMCP/Forward.h>

namespace MCP {

class MCP_API MCPServer final : public Weakable<MCPServer> {
public:
    AK_ALLOC_WITH_KMALLOC;

    static ErrorOr<NonnullOwnPtr<MCPServer>> create(MCPDelegate&, u16 port, String auth_token);
    ~MCPServer();

    Optional<u16> local_port() const;
    String const& auth_token() const { return m_auth_token; }
    String const& session_id() const { return m_session_id; }

    void broadcast_notification(StringView method, JsonValue params);
    void notify_resource_updated(StringView uri);
    void close_all_sse_streams();
    size_t sse_client_count() const;

    class Connection;

private:
    MCPServer(MCPDelegate&, NonnullRefPtr<Core::TCPServer>, String auth_token, String session_id);

    ErrorOr<void> on_new_client();
    void remove_connection(Connection&);

    MCPDelegate& m_delegate;
    NonnullRefPtr<Core::TCPServer> m_server;
    String m_auth_token;
    String m_session_id;
    HashTable<NonnullRefPtr<Connection>> m_connections;
    HashTable<String> m_subscribed_resources;
    bool m_is_shutting_down { false };
};

}
