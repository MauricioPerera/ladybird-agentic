/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Socket.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TemporaryDirectory.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/AppMode/AppManifest.h>
#include <LibWebView/AppMode/AppModeServer.h>
#include <LibWebView/Automation/FlowStore.h>

namespace {

struct HttpReply {
    int status_code { 0 };
    String headers;
    String body;
    ByteBuffer raw_body;
};

HttpReply send_app_http(u16 port, StringView method, StringView target, StringView body = {}, Optional<StringView> origin = {}, Optional<StringView> token = {}, Optional<StringView> extra_headers = {}, StringView content_type = "application/json"sv)
{
    auto socket = MUST(Core::TCPSocket::connect(Core::SocketAddress { IPv4Address { 127, 0, 0, 1 }, port }));

    StringBuilder req;
    req.appendff("{} {} HTTP/1.1\r\n", method, target);
    req.appendff("Host: 127.0.0.1:{}\r\n", port);
    if (origin.has_value())
        req.appendff("Origin: {}\r\n", *origin);
    if (token.has_value())
        req.appendff("X-Ladybird-App-Token: {}\r\n", *token);
    if (extra_headers.has_value() && !extra_headers->is_empty())
        req.append(*extra_headers);
    if (!body.is_empty() || method == "PUT"sv) {
        req.appendff("Content-Type: {}\r\n", content_type);
        req.appendff("Content-Length: {}\r\n", body.length());
    }
    req.append("Connection: close\r\n\r\n"sv);
    if (!body.is_empty())
        req.append(body);

    MUST(socket->write_until_depleted(req.string_view().bytes()));
    MUST(socket->set_blocking(false));

    StringBuilder raw_response;
    bool done = false;
    bool timed_out = false;

    auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(5), [&] {
        timed_out = true;
        done = true;
    });
    timer->start();

    socket->on_ready_to_read = [&] {
        Array<u8, 16384> buffer;
        while (!done) {
            auto read_res = socket->read_some(buffer);
            if (read_res.is_error()) {
                if (read_res.error().is_errno() && (read_res.error().code() == EAGAIN || read_res.error().code() == EWOULDBLOCK))
                    break;
                done = true;
                break;
            }
            auto bytes = read_res.value();
            if (bytes.is_empty() || socket->is_eof()) {
                if (!bytes.is_empty())
                    raw_response.append(StringView { bytes });
                done = true;
                break;
            }
            raw_response.append(StringView { bytes });
        }
    };

    Core::EventLoop::current().spin_until([&] { return done; });
    timer->stop();
    socket->close();
    EXPECT(!timed_out);

    auto raw_view = raw_response.string_view();
    auto split = raw_view.find("\r\n\r\n"sv);
    EXPECT(split.has_value());

    auto header_block = raw_view.substring_view(0, *split);
    auto first_line = header_block.find_first_split_view('\r');
    auto parts = first_line.split_view(' ');
    int status_code = parts.size() >= 2 ? parts[1].to_number<int>().value_or(0) : 0;

    auto body_bytes = raw_view.bytes().slice(*split + 4);
    auto raw_buf = MUST(ByteBuffer::copy(body_bytes));
    auto utf8_body = String::from_utf8(body_bytes);

    return {
        .status_code = status_code,
        .headers = MUST(String::from_utf8(header_block.bytes())),
        .body = utf8_body.is_error() ? String {} : utf8_body.release_value(),
        .raw_body = move(raw_buf),
    };
}

}

TEST_CASE(app_manifest_validation_and_confinement)
{
    Test::TemporaryDirectory temp { "TestAppManifest"sv };
    auto root = MUST(FileSystem::real_path(temp.path().string()));

    auto index_path = LexicalPath::join(root, "index.html"sv).string();
    auto index_file = MUST(Core::File::open(index_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(index_file->write_until_depleted("<h1>Hello App</h1>"sv.bytes()));
    index_file->close();

    auto manifest_path = LexicalPath::join(root, "ladybird-app.json"sv).string();
    auto manifest_file = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(manifest_file->write_until_depleted(R"JSON({
        "id": "demo-notes",
        "name": "Demo Notes",
        "version": "1.2.3",
        "entry": "index.html",
        "window": { "width": 900, "height": 640, "resizable": false },
        "mcp": { "enabled": true, "allowWebMCP": true },
        "permissions": { "fsRead": ["notes"], "fsWrite": ["notes"], "systemInfo": true }
    })JSON"sv.bytes()));
    manifest_file->close();

    auto loaded = TRY_OR_FAIL(WebView::AppManifest::load_from_file(manifest_path));
    EXPECT_EQ(loaded.id, "demo-notes"_string);
    EXPECT_EQ(loaded.name, "Demo Notes"_string);
    EXPECT_EQ(loaded.window.width, 900);
    EXPECT_EQ(loaded.window.resizable, false);
    EXPECT_EQ(loaded.permissions.fs_read.size(), 1uz);

    auto remote = TRY_OR_FAIL(WebView::AppManifest::from_remote_url("https://app.example.com/workspace"sv));
    EXPECT(remote.is_remote());
    EXPECT(remote.permissions.fs_read.is_empty());
    EXPECT(remote.permissions.fs_write.is_empty());
    EXPECT(!remote.permissions.clipboard_write);
    EXPECT(WebView::AppManifest::from_remote_url("file:///C:/secret.txt"sv).is_error());

    EXPECT(WebView::AppManifest::resolve_confined_subpath(root, "../escape.txt"sv, true).is_error());
    EXPECT(WebView::AppManifest::resolve_confined_subpath(root, "C:\\Windows\\win.ini"sv, true).is_error());
    EXPECT(WebView::AppManifest::resolve_confined_subpath(root, "/etc/passwd"sv, true).is_error());
    EXPECT(!WebView::AppManifest::is_valid_app_id("../bad"sv));
    EXPECT(WebView::AppManifest::is_valid_app_id("demo-app_01"sv));

    auto bad_manifest_path = LexicalPath::join(root, "bad-manifest.json"sv).string();
    auto bad_file = MUST(Core::File::open(bad_manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(bad_file->write_until_depleted(R"JSON({
        "id": "bad-app",
        "name": "Bad App",
        "entry": "../outside.html"
    })JSON"sv.bytes()));
    bad_file->close();
    EXPECT(WebView::AppManifest::load_from_file(bad_manifest_path).is_error());
}

TEST_CASE(app_mode_server_assets_auth_and_fs_jail)
{
    Core::EventLoop loop;
    Test::TemporaryDirectory temp { "TestAppModeServer"sv };
    auto root = MUST(FileSystem::real_path(temp.path().string()));
    auto app_dir = LexicalPath::join(root, "app"sv).string();
    auto data_dir = LexicalPath::join(root, "data"sv).string();
    MUST(Core::Directory::create(app_dir, Core::Directory::CreateDirectories::Yes));

    auto index_file = MUST(Core::File::open(LexicalPath::join(app_dir, "index.html"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(index_file->write_until_depleted("<!doctype html><html><body>OK</body></html>"sv.bytes()));
    index_file->close();

    auto outside_file = MUST(Core::File::open(LexicalPath::join(root, "secret.txt"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(outside_file->write_until_depleted("top-secret"sv.bytes()));
    outside_file->close();

    auto manifest_path = LexicalPath::join(app_dir, "ladybird-app.json"sv).string();
    auto manifest_file = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(manifest_file->write_until_depleted(R"JSON({
        "id": "notes-test",
        "name": "Notes Test",
        "entry": "index.html",
        "permissions": { "fsRead": ["notes"], "fsWrite": ["notes"], "systemInfo": true }
    })JSON"sv.bytes()));
    manifest_file->close();

    auto manifest = TRY_OR_FAIL(WebView::AppManifest::load_from_file(manifest_path));
    auto server = TRY_OR_FAIL(WebView::AppModeServer::create({
        .manifest = move(manifest),
        .data_directory = data_dir,
        .mcp_port_provider = []() -> Optional<u16> { return 17999; },
    }));

    auto port = server->local_port();
    EXPECT(port > 0);
    auto origin = server->origin();
    auto token = server->session_token();

    auto root_resp = send_app_http(port, "GET"sv, "/"sv);
    EXPECT_EQ(root_resp.status_code, 200);
    EXPECT(root_resp.body.contains("OK"sv));

    auto bridge_resp = send_app_http(port, "GET"sv, "/__ladybird_bridge.js"sv);
    EXPECT_EQ(bridge_resp.status_code, 200);
    EXPECT(bridge_resp.body.contains("ladybirdApp"sv));

    auto traversal_resp = send_app_http(port, "GET"sv, "/%2e%2e/secret.txt"sv);
    EXPECT(traversal_resp.status_code == 403 || traversal_resp.status_code == 404);

    // Reject missing token or wrong Origin
    auto no_token_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"system.info","args":{}})"sv, origin);
    EXPECT_EQ(no_token_resp.status_code, 403);

    auto bad_origin_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"system.info","args":{}})"sv, "https://evil.example"sv, token);
    EXPECT_EQ(bad_origin_resp.status_code, 403);

    // system.info
    auto sys_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"system.info","args":{}})"sv, origin, token);
    EXPECT_EQ(sys_resp.status_code, 200);
    auto sys_json = TRY_OR_FAIL(JsonValue::from_string(sys_resp.body)).as_object();
    EXPECT_EQ(sys_json.get_bool("ok"sv).value_or(false), true);
    EXPECT_EQ(sys_json.get_object("result"sv)->get_u64("mcpPort"sv).value_or(0), 17999ULL);

    // fs.writeText + fs.readText + fs.listDir + fs.deleteFile
    auto write_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"fs.writeText","args":{"path":"notes/hello.md","content":"hola mundo"}})"sv, origin, token);
    EXPECT_EQ(write_resp.status_code, 200);

    auto read_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"fs.readText","args":{"path":"notes/hello.md"}})"sv, origin, token);
    EXPECT_EQ(read_resp.status_code, 200);
    auto read_json = TRY_OR_FAIL(JsonValue::from_string(read_resp.body)).as_object();
    EXPECT_EQ(read_json.get_object("result"sv)->get_string("content"sv).value(), "hola mundo"_string);

    auto list_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"fs.listDir","args":{"path":"notes"}})"sv, origin, token);
    EXPECT_EQ(list_resp.status_code, 200);
    EXPECT(list_resp.body.contains("hello.md"sv));

    // Reject disallowed subdirectory and traversal inside fs IPC
    auto disallowed_dir_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"fs.writeText","args":{"path":"secrets/x.txt","content":"no"}})"sv, origin, token);
    EXPECT_EQ(disallowed_dir_resp.status_code, 403);

    auto escape_ipc_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"fs.writeText","args":{"path":"notes/../secret.txt","content":"no"}})"sv, origin, token);
    EXPECT_EQ(escape_ipc_resp.status_code, 403);

    auto del_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"fs.deleteFile","args":{"path":"notes/hello.md"}})"sv, origin, token);
    EXPECT_EQ(del_resp.status_code, 200);
    EXPECT(del_resp.body.contains("\"deleted\":true"sv));
}

TEST_CASE(app_mode_stable_port_and_sqlite_kv_persistence)
{
    Core::EventLoop loop;
    Test::TemporaryDirectory temp { "TestAppModeKV"sv };
    auto root = MUST(FileSystem::real_path(temp.path().string()));
    auto app_dir = LexicalPath::join(root, "app"sv).string();
    auto data_default = LexicalPath::join(root, "data-default"sv).string();
    auto data_work = LexicalPath::join(root, "data-work"sv).string();
    MUST(Core::Directory::create(app_dir, Core::Directory::CreateDirectories::Yes));

    auto index_file = MUST(Core::File::open(LexicalPath::join(app_dir, "index.html"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(index_file->write_until_depleted("<!doctype html><html><body>KV</body></html>"sv.bytes()));
    index_file->close();

    auto manifest_path = LexicalPath::join(app_dir, "ladybird-app.json"sv).string();
    auto manifest_file = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(manifest_file->write_until_depleted(R"JSON({
        "id": "kv-notes-test",
        "name": "KV Notes Test",
        "entry": "index.html",
        "permissions": { "fsRead": ["notes"], "fsWrite": ["notes"], "storage": true, "systemInfo": true }
    })JSON"sv.bytes()));
    manifest_file->close();

    auto manifest_default = TRY_OR_FAIL(WebView::AppManifest::load_from_file(manifest_path));
    auto manifest_work = TRY_OR_FAIL(WebView::AppManifest::load_from_file(manifest_path));

    u16 port_default = 0;
    {
        auto server_default = TRY_OR_FAIL(WebView::AppModeServer::create({
            .manifest = manifest_default,
            .data_directory = data_default,
            .account_id = "default"_string,
            .account_label = "Default"_string,
            .profile_name = "app-kv-notes-test"_string,
        }));
        auto server_work = TRY_OR_FAIL(WebView::AppModeServer::create({
            .manifest = manifest_work,
            .data_directory = data_work,
            .account_id = "trabajo"_string,
            .account_label = "Trabajo"_string,
            .profile_name = "app-kv-notes-test--trabajo"_string,
        }));

        port_default = server_default->local_port();
        auto port_work = server_work->local_port();
        EXPECT(port_default >= 42000 && port_default <= 58999);
        EXPECT(port_work >= 42000 && port_work <= 58999);
        EXPECT(port_default != port_work);
        EXPECT(server_default->is_using_stable_port());
        EXPECT(server_work->is_using_stable_port());
        EXPECT(FileSystem::is_regular_file(LexicalPath::join(data_default, "origin-port.txt"sv).string()));
        EXPECT(FileSystem::is_regular_file(LexicalPath::join(data_default, "AppStore.db"sv).string()));

        auto origin_default = server_default->origin();
        auto token_default = server_default->session_token();
        auto origin_work = server_work->origin();
        auto token_work = server_work->session_token();

        // Write KV in default and work accounts independently
        auto set1 = send_app_http(port_default, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"kv.set","args":{"key":"ui:theme","value":"midnight"}})"sv, origin_default, token_default);
        EXPECT_EQ(set1.status_code, 200);
        auto set2 = send_app_http(port_default, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"kv.set","args":{"key":"meta:count","value":42}})"sv, origin_default, token_default);
        EXPECT_EQ(set2.status_code, 200);
        auto set_work = send_app_http(port_work, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"kv.set","args":{"key":"ui:theme","value":"solarized"}})"sv, origin_work, token_work);
        EXPECT_EQ(set_work.status_code, 200);

        // Work account must NOT see default's meta:count key
        auto missing_in_work = send_app_http(port_work, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"kv.get","args":{"key":"meta:count"}})"sv, origin_work, token_work);
        EXPECT_EQ(missing_in_work.status_code, 200);
        auto missing_json = TRY_OR_FAIL(JsonValue::from_string(missing_in_work.body)).as_object();
        EXPECT_EQ(missing_json.get_object("result"sv)->get_bool("found"sv).value_or(true), false);

        // Prefix list in default
        auto list_ui = send_app_http(port_default, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"kv.list","args":{"prefix":"ui:"}})"sv, origin_default, token_default);
        EXPECT_EQ(list_ui.status_code, 200);
        EXPECT(list_ui.body.contains("ui:theme"sv));
        EXPECT(!list_ui.body.contains("meta:count"sv));
    }

    // Restart default server and verify BOTH same port and SQLite KV persistence
    auto server_default_restarted = TRY_OR_FAIL(WebView::AppModeServer::create({
        .manifest = manifest_default,
        .data_directory = data_default,
        .account_id = "default"_string,
        .account_label = "Default"_string,
        .profile_name = "app-kv-notes-test"_string,
    }));
    EXPECT_EQ(server_default_restarted->local_port(), port_default);

    auto restarted_origin = server_default_restarted->origin();
    auto restarted_token = server_default_restarted->session_token();
    auto persisted_get = send_app_http(port_default, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"kv.get","args":{"key":"meta:count"}})"sv, restarted_origin, restarted_token);
    EXPECT_EQ(persisted_get.status_code, 200);
    auto persisted_json = TRY_OR_FAIL(JsonValue::from_string(persisted_get.body)).as_object();
    EXPECT_EQ(persisted_json.get_object("result"sv)->get_bool("found"sv).value_or(false), true);
    EXPECT_EQ(persisted_json.get_object("result"sv)->get_i64("value"sv).value_or(0), 42LL);

    // kv.clear with prefix only deletes matching prefix
    auto clear_ui = send_app_http(port_default, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"kv.clear","args":{"prefix":"ui:"}})"sv, restarted_origin, restarted_token);
    EXPECT_EQ(clear_ui.status_code, 200);
    EXPECT(clear_ui.body.contains("\"cleared\":1"sv));

    auto theme_after_clear = send_app_http(port_default, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"kv.get","args":{"key":"ui:theme"}})"sv, restarted_origin, restarted_token);
    EXPECT_EQ(TRY_OR_FAIL(JsonValue::from_string(theme_after_clear.body)).as_object().get_object("result"sv)->get_bool("found"sv).value_or(true), false);

    auto count_after_clear = send_app_http(port_default, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"kv.get","args":{"key":"meta:count"}})"sv, restarted_origin, restarted_token);
    EXPECT_EQ(TRY_OR_FAIL(JsonValue::from_string(count_after_clear.body)).as_object().get_object("result"sv)->get_bool("found"sv).value_or(false), true);
}

TEST_CASE(app_mode_accounts_registry_and_ipc)
{
    Core::EventLoop loop;
    Test::TemporaryDirectory temp { "TestAppModeAccounts"sv };
    auto root = MUST(FileSystem::real_path(temp.path().string()));
    auto app_dir = LexicalPath::join(root, "app"sv).string();
    auto data_dir = LexicalPath::join(root, "data"sv).string();
    auto registry_path = LexicalPath::join(root, "AppAccounts"sv, "acct-test.json"sv).string();
    MUST(Core::Directory::create(app_dir, Core::Directory::CreateDirectories::Yes));

    auto index_file = MUST(Core::File::open(LexicalPath::join(app_dir, "index.html"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(index_file->write_until_depleted("<!doctype html><html><body>Accounts</body></html>"sv.bytes()));
    index_file->close();

    auto manifest_path = LexicalPath::join(app_dir, "ladybird-app.json"sv).string();
    auto manifest_file = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(manifest_file->write_until_depleted(R"JSON({
        "id": "acct-test",
        "name": "Acct Test",
        "entry": "index.html"
    })JSON"sv.bytes()));
    manifest_file->close();

    Optional<String> launched_account;
    auto manifest = TRY_OR_FAIL(WebView::AppManifest::load_from_file(manifest_path));
    auto server = TRY_OR_FAIL(WebView::AppModeServer::create({
        .manifest = move(manifest),
        .data_directory = data_dir,
        .account_id = "default"_string,
        .account_label = "Default"_string,
        .profile_name = "app-acct-test"_string,
        .accounts_registry_path = registry_path,
        .on_launch_account = [&](StringView account_id) -> ErrorOr<void> {
            launched_account = TRY(String::from_utf8(account_id));
            return {};
        },
    }));

    auto port = server->local_port();
    auto origin = server->origin();
    auto token = server->session_token();

    auto current_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"accounts.current","args":{}})"sv, origin, token);
    EXPECT_EQ(current_resp.status_code, 200);
    EXPECT(current_resp.body.contains("\"id\":\"default\""sv));

    auto bad_create = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"accounts.create","args":{"id":"../bad"}})"sv, origin, token);
    EXPECT_EQ(bad_create.status_code, 400);

    auto create_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"accounts.create","args":{"id":"trabajo","label":"Trabajo"}})"sv, origin, token);
    EXPECT_EQ(create_resp.status_code, 200);
    EXPECT(create_resp.body.contains("app-acct-test--trabajo"sv));

    auto list_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"accounts.list","args":{}})"sv, origin, token);
    EXPECT_EQ(list_resp.status_code, 200);
    EXPECT(list_resp.body.contains("\"id\":\"default\""sv));
    EXPECT(list_resp.body.contains("\"id\":\"trabajo\""sv));

    auto launch_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"accounts.launch","args":{"id":"trabajo"}})"sv, origin, token);
    EXPECT_EQ(launch_resp.status_code, 200);
    EXPECT(launched_account.has_value());
    EXPECT_EQ(*launched_account, "trabajo"_string);
}

TEST_CASE(app_mode_bridge_injects_webmcp_shim_when_enabled)
{
    Core::EventLoop loop;
    Test::TemporaryDirectory temp { "TestAppModeWebMCPShim"sv };
    auto root = MUST(FileSystem::real_path(temp.path().string()));
    auto app_dir = LexicalPath::join(root, "app"sv).string();
    auto data_dir = LexicalPath::join(root, "data"sv).string();
    MUST(Core::Directory::create(app_dir, Core::Directory::CreateDirectories::Yes));

    auto index_file = MUST(Core::File::open(LexicalPath::join(app_dir, "index.html"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(index_file->write_until_depleted("<!doctype html><html><body>WebMCP</body></html>"sv.bytes()));
    index_file->close();

    auto manifest_path = LexicalPath::join(app_dir, "ladybird-app.json"sv).string();
    auto manifest_file = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(manifest_file->write_until_depleted(R"JSON({
        "id": "webmcp-bridge-test",
        "name": "WebMCP Bridge Test",
        "entry": "index.html",
        "mcp": { "enabled": true, "allowWebMCP": true }
    })JSON"sv.bytes()));
    manifest_file->close();

    {
        auto manifest = TRY_OR_FAIL(WebView::AppManifest::load_from_file(manifest_path));
        auto server = TRY_OR_FAIL(WebView::AppModeServer::create({
            .manifest = move(manifest),
            .data_directory = data_dir,
            .enable_webmcp_shim = true,
        }));
        auto bridge_resp = send_app_http(server->local_port(), "GET"sv, "/__ladybird_bridge.js"sv);
        EXPECT_EQ(bridge_resp.status_code, 200);
        EXPECT(bridge_resp.body.contains("__ladybirdWebMCP"sv));
        EXPECT(bridge_resp.body.contains("modelContext"sv));
    }

    {
        auto manifest = TRY_OR_FAIL(WebView::AppManifest::load_from_file(manifest_path));
        auto server = TRY_OR_FAIL(WebView::AppModeServer::create({
            .manifest = move(manifest),
            .data_directory = data_dir,
            .enable_webmcp_shim = false,
        }));
        auto bridge_resp = send_app_http(server->local_port(), "GET"sv, "/__ladybird_bridge.js"sv);
        EXPECT_EQ(bridge_resp.status_code, 200);
        EXPECT(!bridge_resp.body.contains("__ladybirdWebMCP"sv));
    }
}

TEST_CASE(app_mode_manifest_and_bridge_webmcp_only_privacy)
{
    Core::EventLoop loop;
    Test::TemporaryDirectory temp { "TestAppModeWebMCPOnly"sv };
    auto root = MUST(FileSystem::real_path(temp.path().string()));
    auto app_dir = LexicalPath::join(root, "app"sv).string();
    auto data_dir = LexicalPath::join(root, "data"sv).string();
    MUST(Core::Directory::create(app_dir, Core::Directory::CreateDirectories::Yes));

    auto index_file = MUST(Core::File::open(LexicalPath::join(app_dir, "index.html"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(index_file->write_until_depleted("<!doctype html><html><body>Private Banking</body></html>"sv.bytes()));
    index_file->close();

    auto manifest_path = LexicalPath::join(app_dir, "ladybird-app.json"sv).string();
    auto write_manifest = [&](StringView json) {
        auto f = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted(json.bytes()));
        f->close();
    };

    write_manifest(R"JSON({
        "id": "vault-finance",
        "name": "Vault Finance",
        "entry": "index.html",
        "mcp": { "enabled": true, "allowWebMCP": true, "access": "webmcp-only" }
    })JSON"sv);
    auto loaded = TRY_OR_FAIL(WebView::AppManifest::load_from_file(manifest_path));
    EXPECT_EQ(to_underlying(loaded.mcp.access), to_underlying(WebView::AppMCPAccess::WebMCPOnly));

    auto server = TRY_OR_FAIL(WebView::AppModeServer::create({
        .manifest = loaded,
        .data_directory = data_dir,
        .enable_webmcp_shim = true,
    }));
    auto bridge_resp = send_app_http(server->local_port(), "GET"sv, "/__ladybird_bridge.js"sv);
    EXPECT_EQ(bridge_resp.status_code, 200);
    EXPECT(bridge_resp.body.contains("\"mcpAccess\":\"webmcp-only\""sv));

    auto origin = server->origin();
    auto token = server->session_token();
    auto sys_resp = send_app_http(server->local_port(), "POST"sv, "/__ladybird_ipc"sv, R"({"command":"system.info","args":{}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(sys_resp.status_code, 200);
    auto sys_json = TRY_OR_FAIL(JsonValue::from_string(sys_resp.body)).as_object();
    EXPECT_EQ(sys_json.get_object("result"sv)->get_string("mcpAccess"sv).value(), "webmcp-only"_string);

    write_manifest(R"JSON({
        "id": "vault-finance",
        "name": "Vault Finance",
        "entry": "index.html",
        "mcp": { "enabled": true, "allowWebMCP": true, "access": "invalid-mode" }
    })JSON"sv);
    EXPECT(WebView::AppManifest::load_from_file(manifest_path).is_error());

    write_manifest(R"JSON({
        "id": "vault-finance",
        "name": "Vault Finance",
        "entry": "index.html",
        "mcp": { "enabled": true, "allowWebMCP": false, "access": "webmcp-only" }
    })JSON"sv);
    EXPECT(WebView::AppManifest::load_from_file(manifest_path).is_error());
}

TEST_CASE(app_mode_binary_fs_range_stat_and_mkdir)
{
    Core::EventLoop loop;
    Test::TemporaryDirectory temp { "TestAppModeBinaryFS"sv };
    auto root = MUST(FileSystem::real_path(temp.path().string()));
    auto app_dir = LexicalPath::join(root, "app"sv).string();
    auto data_dir = LexicalPath::join(root, "data"sv).string();
    MUST(Core::Directory::create(app_dir, Core::Directory::CreateDirectories::Yes));

    auto index_file = MUST(Core::File::open(LexicalPath::join(app_dir, "index.html"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(index_file->write_until_depleted("<!doctype html><html><body>Audio</body></html>"sv.bytes()));
    index_file->close();

    auto manifest_path = LexicalPath::join(app_dir, "ladybird-app.json"sv).string();
    auto mf = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    MUST(mf->write_until_depleted(R"JSON({
        "id": "binary-audio-test",
        "name": "Binary Audio Test",
        "entry": "index.html",
        "permissions": { "fsRead": ["audio"], "fsWrite": ["audio"], "systemInfo": true }
    })JSON"sv.bytes()));
    mf->close();

    auto manifest = TRY_OR_FAIL(WebView::AppManifest::load_from_file(manifest_path));
    auto server = TRY_OR_FAIL(WebView::AppModeServer::create({
        .manifest = move(manifest),
        .data_directory = data_dir,
    }));

    auto port = server->local_port();
    auto origin = server->origin();
    auto token = server->session_token();

    // fs.mkdir + fs.stat
    auto mkdir_resp = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"fs.mkdir","args":{"path":"audio/stems"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(mkdir_resp.status_code, 200);
    EXPECT(mkdir_resp.body.contains("\"created\":true"sv));

    auto stat_dir = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"fs.stat","args":{"path":"audio/stems"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(stat_dir.status_code, 200);
    EXPECT(stat_dir.body.contains("\"exists\":true"sv));
    EXPECT(stat_dir.body.contains("\"isDirectory\":true"sv));

    auto stat_missing = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"fs.stat","args":{"path":"audio/missing.wav"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(stat_missing.status_code, 200);
    EXPECT(stat_missing.body.contains("\"exists\":false"sv));

    auto stat_denied = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"fs.stat","args":{"path":"secrets/x.wav"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(stat_denied.status_code, 403);

    // Binary PUT with embedded NUL bytes
    Array<u8, 12> payload { 'R', 'I', 'F', 'F', 0x00, 0x01, 0x02, 0x03, 'W', 'A', 'V', 'E' };
    StringView payload_view { payload.span() };

    auto bad_put = send_app_http(port, "PUT"sv, "/__ladybird_fs/audio/stems/clip.wav"sv, payload_view, origin.bytes_as_string_view(), "bad-token"sv, {}, "application/octet-stream"sv);
    EXPECT_EQ(bad_put.status_code, 403);

    auto put_resp = send_app_http(port, "PUT"sv, "/__ladybird_fs/audio/stems/clip.wav"sv, payload_view, origin.bytes_as_string_view(), token.bytes_as_string_view(), {}, "application/octet-stream"sv);
    EXPECT_EQ(put_resp.status_code, 200);
    EXPECT(put_resp.body.contains("\"bytesWritten\":12"sv));

    auto stat_file = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"fs.stat","args":{"path":"audio/stems/clip.wav"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(stat_file.status_code, 200);
    EXPECT(stat_file.body.contains("\"isFile\":true"sv));
    EXPECT(stat_file.body.contains("\"size\":12"sv));

    // Full GET (header auth + query auth) & HEAD
    auto get_resp = send_app_http(port, "GET"sv, "/__ladybird_fs/audio/stems/clip.wav"sv, {}, {}, token.bytes_as_string_view());
    EXPECT_EQ(get_resp.status_code, 200);
    EXPECT_EQ(get_resp.raw_body.bytes(), payload.span());

    auto query_url = MUST(String::formatted("/__ladybird_fs/audio/stems/clip.wav?token={}", token));
    auto get_query = send_app_http(port, "GET"sv, query_url);
    EXPECT_EQ(get_query.status_code, 200);
    EXPECT_EQ(get_query.raw_body.bytes(), payload.span());

    auto head_resp = send_app_http(port, "HEAD"sv, "/__ladybird_fs/audio/stems/clip.wav"sv, {}, {}, token.bytes_as_string_view());
    EXPECT_EQ(head_resp.status_code, 200);
    EXPECT(head_resp.headers.contains("Content-Length: 12"sv));
    EXPECT_EQ(head_resp.raw_body.size(), 0uz);

    // Byte Range GET & 416 handling
    auto range_mid = send_app_http(port, "GET"sv, "/__ladybird_fs/audio/stems/clip.wav"sv, {}, {}, token.bytes_as_string_view(), "Range: bytes=4-7\r\n"sv);
    EXPECT_EQ(range_mid.status_code, 206);
    EXPECT(range_mid.headers.contains("Content-Range: bytes 4-7/12"sv));
    EXPECT_EQ(range_mid.raw_body.bytes(), payload.span().slice(4, 4));

    auto range_suffix = send_app_http(port, "GET"sv, "/__ladybird_fs/audio/stems/clip.wav"sv, {}, {}, token.bytes_as_string_view(), "Range: bytes=-4\r\n"sv);
    EXPECT_EQ(range_suffix.status_code, 206);
    EXPECT(range_suffix.headers.contains("Content-Range: bytes 8-11/12"sv));
    EXPECT_EQ(range_suffix.raw_body.bytes(), payload.span().slice(8, 4));

    auto range_bad = send_app_http(port, "GET"sv, "/__ladybird_fs/audio/stems/clip.wav"sv, {}, {}, token.bytes_as_string_view(), "Range: bytes=99-120\r\n"sv);
    EXPECT_EQ(range_bad.status_code, 416);

    // Bridge advertises binary FS methods
    auto bridge_resp = send_app_http(port, "GET"sv, "/__ladybird_bridge.js"sv);
    EXPECT_EQ(bridge_resp.status_code, 200);
    EXPECT(bridge_resp.body.contains("readBytes"sv));
    EXPECT(bridge_resp.body.contains("writeBytes"sv));
    EXPECT(bridge_resp.body.contains("fileUrl"sv));
}

TEST_CASE(app_mode_service_tabs_and_notifications)
{
    Core::EventLoop loop;
    Test::TemporaryDirectory temp_dir { "app_mode_services"sv };
    auto root = MUST(FileSystem::real_path(temp_dir.path().string()));
    auto data_dir = LexicalPath::join(root, "data"sv).string();
    auto entry_path = LexicalPath::join(root, "index.html"sv).string();
    {
        auto f = MUST(Core::File::open(entry_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted("<!doctype html><html><body>ok</body></html>"sv.bytes()));
    }

    // Invalid service manifest (defaultUrl outside origins) must be rejected
    auto bad_manifest_path = LexicalPath::join(root, "bad-manifest.json"sv).string();
    {
        auto f = MUST(Core::File::open(bad_manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted(R"({
            "id": "svc-bad",
            "name": "Bad Services",
            "services": {
                "mail": {
                    "defaultUrl": "https://mail.google.com/mail/u/0/#inbox",
                    "origins": ["https://calendar.google.com"],
                    "permissions": ["open"]
                }
            }
        })"sv.bytes()));
    }
    EXPECT(WebView::AppManifest::load_from_file(bad_manifest_path).is_error());

    // Invalid container id in service must be rejected
    auto bad_container_path = LexicalPath::join(root, "bad-container-manifest.json"sv).string();
    {
        auto f = MUST(Core::File::open(bad_container_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted(R"({
            "id": "svc-bad-container",
            "name": "Bad Container",
            "services": {
                "mail": {
                    "defaultUrl": "https://mail.google.com/mail/u/0/#inbox",
                    "origins": ["https://mail.google.com"],
                    "container": "../bad"
                }
            }
        })"sv.bytes()));
    }
    EXPECT(WebView::AppManifest::load_from_file(bad_container_path).is_error());

    // Valid manifest with showTabBar + services
    auto manifest_path = LexicalPath::join(root, "ladybird-app.json"sv).string();
    {
        auto f = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted(R"({
            "id": "svc-app",
            "name": "Service App",
            "window": { "showTabBar": true },
            "services": {
                "mail": {
                    "label": "Gmail",
                    "defaultUrl": "https://mail.google.com/mail/u/0/#inbox",
                    "origins": ["https://mail.google.com"],
                    "container": "personal",
                    "permissions": ["open", "navigate", "notifications", "eval"]
                },
                "calendar": {
                    "label": "Calendar",
                    "defaultUrl": "https://calendar.google.com/calendar/u/0/r",
                    "origins": ["https://calendar.google.com"],
                    "permissions": ["open", "navigate"]
                }
            }
        })"sv.bytes()));
    }

    auto manifest = MUST(WebView::AppManifest::load_from_file(manifest_path));
    EXPECT_EQ(manifest.window.show_tab_bar, true);
    EXPECT_EQ(manifest.services.size(), 2uz);
    EXPECT(manifest.find_service("mail"sv).has_value());
    EXPECT_EQ(manifest.find_service("mail"sv)->container, "personal"sv);
    EXPECT(manifest.find_service("calendar"sv).has_value());

    bool mail_open = false;
    String mail_url = "https://mail.google.com/mail/u/0/#inbox"_string;
    String mail_container = "personal"_string;

    auto server = MUST(WebView::AppModeServer::create({
        .manifest = manifest,
        .data_directory = data_dir,
        .disable_sql_database = true,
        .enable_webmcp_shim = false,
        .on_service_open = [&](StringView service_id, Optional<URL::URL> url, bool, bool, Optional<String> container_override) -> ErrorOr<WebView::AppServiceTabState> {
            mail_open = true;
            if (url.has_value())
                mail_url = url->to_string();
            mail_container = container_override.value_or("personal"_string);
            return WebView::AppServiceTabState {
                .service_id = TRY(String::from_utf8(service_id)),
                .tab_id = 42,
                .open = true,
                .url = mail_url,
                .title = "Inbox"_string,
                .container = mail_container,
                .origin_allowed = true,
            };
        },
        .on_service_navigate = [&](StringView service_id, URL::URL url) -> ErrorOr<WebView::AppServiceTabState> {
            mail_url = url.to_string();
            return WebView::AppServiceTabState {
                .service_id = TRY(String::from_utf8(service_id)),
                .tab_id = 42,
                .open = true,
                .url = mail_url,
                .title = "Compose"_string,
                .container = mail_container,
                .origin_allowed = true,
            };
        },
        .on_service_list = [&]() -> Vector<WebView::AppServiceTabState> {
            if (!mail_open)
                return {};
            return {
                WebView::AppServiceTabState {
                    .service_id = "mail"_string,
                    .tab_id = 42,
                    .open = true,
                    .url = mail_url,
                    .title = "Inbox"_string,
                    .container = mail_container,
                    .origin_allowed = true,
                },
            };
        },
        .on_service_eval = [&](StringView service_id, String script, Function<void(Optional<JsonValue>, Optional<String>)> on_complete) {
            EXPECT_EQ(service_id, "mail"sv);
            EXPECT(script.contains("ping"sv));
            JsonObject obj;
            obj.set("pong"sv, 7);
            on_complete(JsonValue { move(obj) }, {});
        },
    }));

    auto port = server->local_port();
    auto origin = server->origin();
    auto token = server->session_token();

    auto list_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.list","args":{}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(list_res.status_code, 200);
    EXPECT(list_res.body.contains("\"id\":\"mail\""sv));
    EXPECT(list_res.body.contains("\"container\":\"personal\""sv));
    EXPECT(list_res.body.contains("\"id\":\"calendar\""sv));

    // Off-origin & invalid-container tabs.open & tabs.navigate checks
    auto bad_open = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.open","args":{"serviceId":"mail","url":"https://evil.example/"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(bad_open.status_code, 403);

    auto bad_container_open = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.open","args":{"serviceId":"mail","container":"../bad"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(bad_container_open.status_code, 400);

    auto good_open = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.open","args":{"serviceId":"mail","container":"work"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(good_open.status_code, 200);
    EXPECT(good_open.body.contains("\"open\":true"sv));
    EXPECT(good_open.body.contains("\"container\":\"work\""sv));
    EXPECT(good_open.body.contains("\"tabId\":42"sv));

    auto bad_nav = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.navigate","args":{"serviceId":"mail","url":"https://other.example/x"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(bad_nav.status_code, 403);

    // tabs.eval permission check (calendar has no eval) and async evaluation on mail
    auto denied_eval = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.eval","args":{"serviceId":"calendar","script":"1+1"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(denied_eval.status_code, 403);

    auto ok_eval = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"tabs.eval","args":{"serviceId":"mail","script":"(() => 'ping')()"}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(ok_eval.status_code, 200);
    EXPECT(ok_eval.body.contains("\"pong\":7"sv));

    // Notifications filtered by origin and allow_notifications
    server->push_service_notification("https://bank.example"sv, 99, "shown"sv, "Secret Balance"sv, "$999"sv, "tag1"sv, 100);
    server->push_service_notification("https://calendar.google.com"sv, 43, "shown"sv, "Cal Disabled"sv, "no perm"sv, "tag2"sv, 101);
    server->push_service_notification("https://mail.google.com"sv, 42, "shown"sv, "Updated invitation: Demo"sv, "Fri 3pm"sv, "tag3"sv, 102);

    auto notifs = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.notifications","args":{"sinceSeq":0}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(notifs.status_code, 200);
    EXPECT(!notifs.body.contains("Secret Balance"sv));
    EXPECT(!notifs.body.contains("Cal Disabled"sv));
    EXPECT(notifs.body.contains("Updated invitation: Demo"sv));
    EXPECT(notifs.body.contains("\"lastSeq\":1"sv));

    auto after_seq1 = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.notifications","args":{"sinceSeq":1}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(after_seq1.status_code, 200);
    EXPECT(!after_seq1.body.contains("Updated invitation: Demo"sv));

    // Bridge script exposes tabs API & service metadata
    auto bridge_resp = send_app_http(port, "GET"sv, "/__ladybird_bridge.js"sv);
    EXPECT_EQ(bridge_resp.status_code, 200);
    EXPECT(bridge_resp.body.contains("onNotification"sv));
    EXPECT(bridge_resp.body.contains("\"Gmail\""sv));
}

TEST_CASE(app_mode_automation_engine_flows_network_and_triggers)
{
    Core::EventLoop loop;
    Test::TemporaryDirectory temp_dir { "app_mode_automation_engine"sv };
    auto root = MUST(FileSystem::real_path(temp_dir.path().string()));
    auto data_dir = LexicalPath::join(root, "data"sv).string();
    auto entry_path = LexicalPath::join(root, "index.html"sv).string();
    {
        auto f = MUST(Core::File::open(entry_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted("<!doctype html><html><body>engine</body></html>"sv.bytes()));
    }

    // Invalid permission must be rejected
    auto bad_perm_path = LexicalPath::join(root, "bad-perm.json"sv).string();
    {
        auto f = MUST(Core::File::open(bad_perm_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted(R"JSON({
            "id": "bad-perm-app",
            "name": "Bad Perm",
            "services": {
                "mail": {
                    "defaultUrl": "https://mail.google.com/",
                    "origins": ["https://mail.google.com"],
                    "permissions": ["open", "magic"]
                }
            }
        })JSON"sv.bytes()));
    }
    EXPECT(WebView::AppManifest::load_from_file(bad_perm_path).is_error());

    auto manifest_path = LexicalPath::join(root, "ladybird-app.json"sv).string();
    {
        auto f = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted(R"JSON({
            "id": "auto-engine-app",
            "name": "Automation Engine",
            "services": {
                "mail": {
                    "label": "Gmail",
                    "defaultUrl": "https://mail.google.com/mail/u/0/#inbox",
                    "origins": ["https://mail.google.com"],
                    "permissions": ["open", "navigate", "notifications", "eval", "flows", "network"]
                },
                "readonly": {
                    "label": "ReadOnly",
                    "defaultUrl": "https://calendar.google.com/",
                    "origins": ["https://calendar.google.com"],
                    "permissions": ["open"]
                }
            }
        })JSON"sv.bytes()));
    }

    auto manifest = MUST(WebView::AppManifest::load_from_file(manifest_path));
    auto mail_svc = manifest.find_service("mail"sv);
    auto ro_svc = manifest.find_service("readonly"sv);
    EXPECT(mail_svc.has_value());
    EXPECT(mail_svc->allow_flows);
    EXPECT(mail_svc->allow_network);
    EXPECT(ro_svc.has_value());
    EXPECT(!ro_svc->allow_flows);
    EXPECT(!ro_svc->allow_network);

    WebView::FlowStore flow_store { LexicalPath::join(data_dir, "flows"sv).string() };
    u32 flow_invocations = 0;
    String last_run_service;
    String last_run_flow_id;
    String last_network_url_var;
    bool last_network_url_tainted = false;

    auto server_config = [&](WebView::AppManifest const& mf) {
        return WebView::AppModeServerConfig {
            .manifest = mf,
            .data_directory = data_dir,
            .disable_sql_database = true,
            .enable_webmcp_shim = false,
            .on_flows_list = [&]() { return flow_store.list_flows(); },
            .on_flows_get = [&](StringView id) { return flow_store.get_flow(id); },
            .on_flows_save = [&](WebView::FlowDefinition flow) {
                flow.source = "user"_string;
                return flow_store.save_flow(move(flow));
            },
            .on_flows_delete = [&](StringView id) { return flow_store.delete_flow(id); },
            .on_flows_get_run = [&](StringView id) { return flow_store.get_run(id); },
            .on_flows_list_runs = [&](Optional<StringView> id, size_t limit) { return flow_store.list_runs(id, limit); },
            .on_flows_cancel_run = [&](StringView) { return false; },
            .on_service_run_flow = [&](StringView service_id, Optional<String> flow_id, Optional<WebView::FlowDefinition>, HashMap<String, String> variables, HashTable<String> tainted_variables, bool, bool, bool, bool, Function<void(Optional<WebView::FlowRunRecord>, Optional<String>)> on_done) {
                flow_invocations++;
                last_run_service = String::from_utf8_without_validation(service_id.bytes());
                last_run_flow_id = flow_id.value_or("inline"_string);
                if (auto net_url = variables.get("network_url"sv); net_url.has_value())
                    last_network_url_var = *net_url;
                last_network_url_tainted = tainted_variables.contains("network_url"sv);
                WebView::FlowRunRecord rec;
                rec.run_id = MUST(String::formatted("run-test-{}", flow_invocations));
                rec.flow_id = last_run_flow_id;
                rec.flow_name = "Test Flow"_string;
                rec.status = "completed"_string;
                flow_store.upsert_run(rec, false);
                on_done(rec, {});
            },
        };
    };

    {
        auto server = MUST(WebView::AppModeServer::create(server_config(manifest)));
        auto port = server->local_port();
        auto origin = server->origin();
        auto token = server->session_token();

        // Save flow via flows.save and verify flows.list / flows.get
        auto save_flow_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"flows.save","args":{"flow":{"id":"sync-mail","name":"Sync Mail","steps":[{"action":"sleep","ms":10}]}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(save_flow_res.status_code, 200);
        EXPECT(save_flow_res.body.contains("\"id\":\"sync-mail\""sv));

        auto list_flows_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"flows.list","args":{}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(list_flows_res.status_code, 200);
        EXPECT(list_flows_res.body.contains("\"sync-mail\""sv));

        // tabs.runFlow permission checks & success
        auto denied_flow_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.runFlow","args":{"serviceId":"readonly","flowId":"sync-mail"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(denied_flow_res.status_code, 403);

        auto ok_flow_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.runFlow","args":{"serviceId":"mail","flowId":"sync-mail"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(ok_flow_res.status_code, 200);
        EXPECT(ok_flow_res.body.contains("\"run-test-1\""sv));

        // Trigger permission & interval validation checks
        auto bad_trig_perm = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"triggers.save","args":{"trigger":{"id":"ro-trig","type":"network","serviceId":"readonly","flowId":"sync-mail"}}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(bad_trig_perm.status_code, 403);

        auto bad_trig_interval = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"triggers.save","args":{"trigger":{"id":"fast-trig","type":"interval","serviceId":"mail","flowId":"sync-mail","intervalMs":100}}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(bad_trig_interval.status_code, 400);

        // Save a network trigger with urlContains & cooldown
        auto ok_trig_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"triggers.save","args":{"trigger":{"id":"gmail-sync-net","name":"On Gmail Sync","type":"network","serviceId":"mail","flowId":"sync-mail","urlContains":"/sync/u/0/i/s","method":"POST","statusCode":200,"cooldownMs":5000}}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(ok_trig_res.status_code, 200);
        EXPECT(ok_trig_res.body.contains("\"id\":\"gmail-sync-net\""sv));

        auto mail_tab_url = URL::Parser::basic_parse("https://mail.google.com/mail/u/0/#inbox"sv).release_value();
        auto other_tab_url = URL::Parser::basic_parse("https://evil.example/"sv).release_value();
        auto cal_tab_url = URL::Parser::basic_parse("https://calendar.google.com/calendar/u/0/r"sv).release_value();

        // Off-origin and permission-denied network events must not leak into mail or readonly
        server->push_service_network_event(99, other_tab_url, 1, "https://evil.example/api/secret"sv, "POST"sv, "fetch"sv, 200, 50, {}, 1000);
        server->push_service_network_event(43, cal_tab_url, 2, "https://calendar.google.com/sync"sv, "POST"sv, "fetch"sv, 200, 50, {}, 1001);
        server->push_service_network_event(42, mail_tab_url, 3, "https://mail.google.com/sync/u/0/i/s?hl=es"sv, "POST"sv, "fetch"sv, 200, 2048, {}, 1002);
        // Second matching network event inside cooldown must not re-fire the trigger
        server->push_service_network_event(42, mail_tab_url, 4, "https://mail.google.com/sync/u/0/i/s?hl=es&b=2"sv, "POST"sv, "fetch"sv, 200, 512, {}, 1003);

        EXPECT_EQ(flow_invocations, 2u); // 1 from tabs.runFlow + 1 from gmail-sync-net (cooldown blocked 2nd)
        EXPECT_EQ(last_run_service, "mail"sv);
        EXPECT_EQ(last_run_flow_id, "sync-mail"sv);
        EXPECT(last_network_url_var.contains("/sync/u/0/i/s"sv));
        EXPECT(last_network_url_tainted);

        auto denied_net_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.network","args":{"serviceId":"readonly"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(denied_net_res.status_code, 403);

        auto net_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.network","args":{"serviceId":"mail","method":"POST"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(net_res.status_code, 200);
        EXPECT(!net_res.body.contains("evil.example"sv));
        EXPECT(!net_res.body.contains("calendar.google.com"sv));
        EXPECT(net_res.body.contains("/sync/u/0/i/s"sv));

        // Navigation & unified events stream check
        server->push_service_navigation_event(42, mail_tab_url, "Inbox"_string, 1004);
        auto ev_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.events","args":{"serviceId":"mail"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(ev_res.status_code, 200);
        EXPECT(ev_res.body.contains("\"type\":\"network\""sv));
        EXPECT(ev_res.body.contains("\"type\":\"navigation\""sv));
    }

    // Persistence of triggers across server reload
    auto server2 = MUST(WebView::AppModeServer::create(server_config(manifest)));
    auto origin2 = server2->origin();
    auto list_trig_res = send_app_http(server2->local_port(), "POST"sv, "/__ladybird_ipc"sv, R"({"command":"triggers.list","args":{}})"sv, origin2.bytes_as_string_view(), server2->session_token().bytes_as_string_view());
    EXPECT_EQ(list_trig_res.status_code, 200);
    EXPECT(list_trig_res.body.contains("\"gmail-sync-net\""sv));
    EXPECT(list_trig_res.body.contains("\"fireCount\":1"sv));

    // Bridge exposes flows, triggers, runFlow, onNetwork, onEvent
    auto bridge_resp = send_app_http(server2->local_port(), "GET"sv, "/__ladybird_bridge.js"sv);
    EXPECT_EQ(bridge_resp.status_code, 200);
    EXPECT(bridge_resp.body.contains("onNetwork"sv));
    EXPECT(bridge_resp.body.contains("onEvent"sv));
    EXPECT(bridge_resp.body.contains("runFlow"sv));
    EXPECT(bridge_resp.body.contains("triggers"sv));
}

TEST_CASE(app_mode_dynamic_services_and_persistence)
{
    Core::EventLoop loop;
    Test::TemporaryDirectory temp_dir { "app_mode_dynamic_services"sv };
    auto root = MUST(FileSystem::real_path(temp_dir.path().string()));
    auto data_dir = LexicalPath::join(root, "data"sv).string();
    auto entry_path = LexicalPath::join(root, "index.html"sv).string();
    {
        auto f = MUST(Core::File::open(entry_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted("<!doctype html><html><body>Dynamic</body></html>"sv.bytes()));
    }

    auto make_manifest = [&](bool allow_dynamic) {
        auto manifest_path = LexicalPath::join(root, allow_dynamic ? "manifest-dyn.json"sv : "manifest-static.json"sv).string();
        auto f = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        auto content = MUST(String::formatted(R"JSON({{
            "id": "dyn-app",
            "name": "Dynamic Studio",
            "permissions": {{ "dynamicServices": {} }},
            "services": {{
                "mail": {{
                    "label": "Gmail",
                    "defaultUrl": "https://mail.google.com/mail/u/0/#inbox",
                    "origins": ["https://mail.google.com"],
                    "container": "personal",
                    "permissions": ["open", "navigate", "eval", "flows", "notifications", "network"]
                }}
            }}
        }})JSON",
            allow_dynamic ? "true"sv : "false"sv));
        MUST(f->write_until_depleted(content.bytes()));
        return MUST(WebView::AppManifest::load_from_file(manifest_path));
    };

    auto build_server_cfg = [&](WebView::AppManifest manifest) -> WebView::AppModeServerConfig {
        return {
            .manifest = move(manifest),
            .data_directory = data_dir,
            .disable_sql_database = true,
            .enable_webmcp_shim = false,
            .on_service_open = [](StringView service_id, Optional<URL::URL> url, bool, bool, Optional<String> container_override) -> ErrorOr<WebView::AppServiceTabState> {
                return WebView::AppServiceTabState {
                    .service_id = TRY(String::from_utf8(service_id)),
                    .tab_id = 77,
                    .open = true,
                    .url = url.has_value() ? url->to_string() : "https://app.slack.com/client"_string,
                    .title = "Slack"_string,
                    .container = container_override.value_or("work"_string),
                    .origin_allowed = true,
                };
            },
            .on_service_close = [](StringView) -> ErrorOr<bool> { return true; },
            .on_service_list = []() -> Vector<WebView::AppServiceTabState> { return {}; },
            .on_service_eval = [](StringView service_id, String, Function<void(Optional<JsonValue>, Optional<String>)> on_complete) {
                JsonObject obj;
                obj.set("service"sv, service_id);
                on_complete(JsonValue { move(obj) }, {});
            },
            .on_service_run_flow = [](StringView, Optional<String>, Optional<WebView::FlowDefinition>, HashMap<String, String>, HashTable<String>, bool, bool, bool, bool, Function<void(Optional<WebView::FlowRunRecord>, Optional<String>)> on_done) {
                on_done({}, {});
            },
        };
    };

    // 1. Disabled dynamicServices must return 403
    {
        auto static_server = MUST(WebView::AppModeServer::create(build_server_cfg(make_manifest(false))));
        auto static_origin = static_server->origin();
        auto res = send_app_http(
            static_server->local_port(),
            "POST"sv,
            "/__ladybird_ipc"sv,
            R"JSON({"command":"tabs.registerService","args":{"service":{"id":"slack-work","label":"Slack Work","defaultUrl":"https://app.slack.com/client","origins":["https://app.slack.com"],"container":"work","permissions":["open","navigate","eval","flows"]}}})JSON"sv,
            static_origin.bytes_as_string_view(),
            static_server->session_token().bytes_as_string_view());
        EXPECT_EQ(res.status_code, 403);
    }

    // 2. Enabled dynamicServices: validation, registration, eval/trigger usage, and persistence
    auto dyn_manifest = make_manifest(true);
    {
        auto server = MUST(WebView::AppModeServer::create(build_server_cfg(dyn_manifest)));
        auto port = server->local_port();
        auto origin = server->origin();
        auto token = server->session_token();

        // Collision with manifest service 'mail'
        auto collide = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"tabs.registerService","args":{"service":{"id":"mail","defaultUrl":"https://mail.google.com/","origins":["https://mail.google.com"]}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(collide.status_code, 400);

        // defaultUrl outside origins
        auto bad_origin = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"tabs.registerService","args":{"service":{"id":"slack-work","defaultUrl":"https://app.slack.com/client","origins":["https://slack.com"]}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(bad_origin.status_code, 400);

        // Invalid container id
        auto bad_container = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"tabs.registerService","args":{"service":{"id":"slack-work","defaultUrl":"https://app.slack.com/client","origins":["https://app.slack.com"],"container":"../bad"}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(bad_container.status_code, 400);

        // Valid registration
        auto ok_reg = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"tabs.registerService","args":{"service":{"id":"slack-work","label":"Slack Work","defaultUrl":"https://app.slack.com/client","origins":["https://app.slack.com"],"container":"work","permissions":["open","navigate","notifications","eval","flows","network"]}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(ok_reg.status_code, 200);
        EXPECT(ok_reg.body.contains("\"id\":\"slack-work\""sv));
        EXPECT(ok_reg.body.contains("\"dynamic\":true"sv));

        // tabs.list shows both mail (dynamic:false) and slack-work (dynamic:true)
        auto list_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.list","args":{}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(list_res.status_code, 200);
        EXPECT(list_res.body.contains("\"id\":\"mail\""sv));
        EXPECT(list_res.body.contains("\"dynamic\":false"sv));
        EXPECT(list_res.body.contains("\"id\":\"slack-work\""sv));
        EXPECT(list_res.body.contains("\"configuredContainer\":\"work\""sv));

        // tabs.open, tabs.eval, and triggers.save work on slack-work
        auto open_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.open","args":{"serviceId":"slack-work"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(open_res.status_code, 200);

        auto eval_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.eval","args":{"serviceId":"slack-work","script":"return 1;"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(eval_res.status_code, 200);
        EXPECT(eval_res.body.contains("\"slack-work\""sv));

        auto trig_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"triggers.save","args":{"trigger":{"id":"slack-watch","type":"network","serviceId":"slack-work","flowId":"sync-slack"}}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(trig_res.status_code, 200);
    }

    // 3. Reload server from same data_directory -> slack-work persists & can be removed
    auto server2 = MUST(WebView::AppModeServer::create(build_server_cfg(dyn_manifest)));
    auto port2 = server2->local_port();
    auto origin2 = server2->origin();
    auto token2 = server2->session_token();

    auto list_res2 = send_app_http(port2, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.list","args":{}})"sv, origin2.bytes_as_string_view(), token2.bytes_as_string_view());
    EXPECT_EQ(list_res2.status_code, 200);
    EXPECT(list_res2.body.contains("\"id\":\"slack-work\""sv));

    auto bridge_resp = send_app_http(port2, "GET"sv, "/__ladybird_bridge.js"sv);
    EXPECT_EQ(bridge_resp.status_code, 200);
    EXPECT(bridge_resp.body.contains("registerService"sv));
    EXPECT(bridge_resp.body.contains("removeService"sv));
    EXPECT(bridge_resp.body.contains("dynamicServices"sv));

    // Cannot remove manifest service
    auto rm_builtin = send_app_http(port2, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.removeService","args":{"serviceId":"mail"}})"sv, origin2.bytes_as_string_view(), token2.bytes_as_string_view());
    EXPECT_EQ(rm_builtin.status_code, 403);

    // Remove dynamic service
    auto rm_dyn = send_app_http(port2, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.removeService","args":{"serviceId":"slack-work"}})"sv, origin2.bytes_as_string_view(), token2.bytes_as_string_view());
    EXPECT_EQ(rm_dyn.status_code, 200);
    EXPECT(rm_dyn.body.contains("\"removed\":true"sv));

    auto list_after = send_app_http(port2, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.list","args":{}})"sv, origin2.bytes_as_string_view(), token2.bytes_as_string_view());
    EXPECT_EQ(list_after.status_code, 200);
    EXPECT(!list_after.body.contains("\"id\":\"slack-work\""sv));
}

TEST_CASE(app_mode_inbound_webhooks)
{
    Core::EventLoop loop;
    Test::TemporaryDirectory temp_dir { "app_mode_webhooks"sv };
    auto root = MUST(FileSystem::real_path(temp_dir.path().string()));
    auto data_dir = LexicalPath::join(root, "data"sv).string();
    auto entry_path = LexicalPath::join(root, "index.html"sv).string();
    {
        auto f = MUST(Core::File::open(entry_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted("<!doctype html><html><body>Webhooks</body></html>"sv.bytes()));
    }

    auto manifest_path = LexicalPath::join(root, "ladybird-app.json"sv).string();
    {
        auto f = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted(R"({
            "id": "hook-app",
            "name": "Webhook App",
            "services": {
                "mail": {
                    "label": "Gmail",
                    "defaultUrl": "https://mail.google.com/",
                    "origins": ["https://mail.google.com"],
                    "permissions": ["open", "navigate", "flows"]
                }
            }
        })"sv.bytes()));
    }
    auto manifest = MUST(WebView::AppManifest::load_from_file(manifest_path));

    u32 native_invocations = 0;
    String last_webhook_body;
    auto build_cfg = [&]() -> WebView::AppModeServerConfig {
        return {
            .manifest = manifest,
            .data_directory = data_dir,
            .disable_sql_database = true,
            .enable_webmcp_shim = false,
            .on_service_list = []() -> Vector<WebView::AppServiceTabState> { return {}; },
            .on_service_run_flow = [&](StringView, Optional<String>, Optional<WebView::FlowDefinition>, HashMap<String, String> vars, HashTable<String>, bool, bool, bool, bool, Function<void(Optional<WebView::FlowRunRecord>, Optional<String>)> on_done) {
                native_invocations++;
                last_webhook_body = vars.get("webhook_body"sv).value_or(String {});
                on_done(WebView::FlowRunRecord { .run_id = "run-hook-1"_string, .flow_id = "f1"_string, .flow_name = "F1"_string, .status = "passed"_string, .tab_id = {}, .started_at_ms = 1, .finished_at_ms = 2, .duration_ms = 1, .current_step_index = 0, .total_steps = 0, .error = {}, .outputs = {}, .steps = {} }, {});
            },
        };
    };

    String generated_secret;
    {
        auto server = MUST(WebView::AppModeServer::create(build_cfg()));
        auto port = server->local_port();
        auto origin = server->origin();
        auto token = server->session_token();

        auto save_hook = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"triggers.save","args":{"trigger":{"id":"stripe-hook","name":"Stripe","type":"webhook","serviceId":"mail","flowId":"f1","cooldownMs":5000}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(save_hook.status_code, 200);
        auto parsed_res = MUST(JsonValue::from_string(save_hook.body));
        generated_secret = parsed_res.as_object().get_object("result"sv)->get_object("trigger"sv)->get_string("webhookSecret"sv).value();
        EXPECT_EQ(generated_secret.bytes_as_string_view().length(), 32uz);

        // GET -> 405
        auto get_res = send_app_http(port, "GET"sv, "/__ladybird_webhook/stripe-hook"sv);
        EXPECT_EQ(get_res.status_code, 405);

        // Unknown trigger -> 404
        auto not_found = send_app_http(port, "POST"sv, "/__ladybird_webhook/missing-hook"sv, "{}"sv, {}, {}, {}, "application/json"sv);
        EXPECT_EQ(not_found.status_code, 404);

        // Wrong token -> 401
        auto bad_tok = send_app_http(port, "POST"sv, "/__ladybird_webhook/stripe-hook"sv, R"({"event":"charge.succeeded"})"sv, {}, {}, "X-Webhook-Token: wrong\r\n"sv);
        EXPECT_EQ(bad_tok.status_code, 401);

        // Valid X-Webhook-Token -> 202
        auto hdr = MUST(String::formatted("X-Webhook-Token: {}\r\n", generated_secret));
        auto ok_hook = send_app_http(port, "POST"sv, "/__ladybird_webhook/stripe-hook"sv, R"({"event":"charge.succeeded","amount":99})"sv, {}, {}, hdr.bytes_as_string_view());
        EXPECT_EQ(ok_hook.status_code, 202);
        EXPECT(ok_hook.body.contains("\"accepted\":true"sv));
        EXPECT_EQ(native_invocations, 1u);
        EXPECT(last_webhook_body.contains("charge.succeeded"sv));

        // Cooldown -> 429
        auto cooldown_res = send_app_http(port, "POST"sv, "/__ladybird_webhook/stripe-hook"sv, R"({"event":"dup"})"sv, {}, {}, hdr.bytes_as_string_view());
        EXPECT_EQ(cooldown_res.status_code, 429);

        // Events stream captured webhook payload
        auto ev_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.events","args":{"serviceId":"mail"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(ev_res.status_code, 200);
        EXPECT(ev_res.body.contains("\"type\":\"webhook\""sv));
        EXPECT(ev_res.body.contains("charge.succeeded"sv));

        // Client-handled webhook trigger with short cooldown
        auto save_ch = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"triggers.save","args":{"trigger":{"id":"hybrid-hook","type":"webhook","serviceId":"mail","flowId":"f2","clientHandled":true,"cooldownMs":500}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(save_ch.status_code, 200);
        auto ch_parsed = MUST(JsonValue::from_string(save_ch.body));
        auto ch_secret = ch_parsed.as_object().get_object("result"sv)->get_object("trigger"sv)->get_string("webhookSecret"sv).value();
        auto bearer_hdr = MUST(String::formatted("Authorization: Bearer {}\r\n", ch_secret));
        auto ch_fire = send_app_http(port, "POST"sv, "/__ladybird_webhook/hybrid-hook"sv, R"({"hello":"world"})"sv, {}, {}, bearer_hdr.bytes_as_string_view());
        EXPECT_EQ(ch_fire.status_code, 202);
        // clientHandled triggers do not invoke on_service_run_flow directly
        EXPECT_EQ(native_invocations, 1u);
    }

    // Secret persists across server restart
    auto server2 = MUST(WebView::AppModeServer::create(build_cfg()));
    auto origin2 = server2->origin();
    auto token2 = server2->session_token();
    auto trig_list = send_app_http(server2->local_port(), "POST"sv, "/__ladybird_ipc"sv, R"({"command":"triggers.list","args":{}})"sv, origin2.bytes_as_string_view(), token2.bytes_as_string_view());
    EXPECT_EQ(trig_list.status_code, 200);
    EXPECT(trig_list.body.contains(generated_secret));
}

TEST_CASE(app_mode_sse_events_cron_and_service_consent)
{
    Core::EventLoop loop;
    EXPECT(WebView::AppModeServer::is_valid_cron_expression("30 9 * * 1-5"sv));
    EXPECT(WebView::AppModeServer::is_valid_cron_expression("*/15 0-23 1,15 * 0,7"sv));
    EXPECT(!WebView::AppModeServer::is_valid_cron_expression("bad cron"sv));
    EXPECT(!WebView::AppModeServer::is_valid_cron_expression("99 * * * *"sv));
    EXPECT(WebView::AppModeServer::cron_matches_local_time("30 9 * * 1-5"sv, 30, 9, 30, 11, 1));
    EXPECT(!WebView::AppModeServer::cron_matches_local_time("30 9 * * 1-5"sv, 30, 9, 29, 11, 0));

    Test::TemporaryDirectory temp_dir { "app_mode_core_ola"sv };
    auto root_dir = MUST(FileSystem::real_path(temp_dir.path().string()));
    auto data_dir = LexicalPath::join(root_dir, "data"sv).string();
    {
        auto f = MUST(Core::File::open(LexicalPath::join(root_dir, "index.html"sv).string(), Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted("<!doctype html><html><body>Core Ola</body></html>"sv.bytes()));
    }
    auto manifest_path = LexicalPath::join(root_dir, "ladybird-app.json"sv).string();
    {
        auto f = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
        MUST(f->write_until_depleted(R"JSON({
        "id": "core-ola-test",
        "name": "Core Ola Test",
        "entry": "index.html",
        "permissions": {
            "storage": false,
            "dynamicServices": true
        },
        "services": {
            "mail": {
                "label": "Mail",
                "defaultUrl": "https://mail.example.com/",
                "origins": ["https://mail.example.com"],
                "permissions": ["open", "navigate", "notifications", "eval", "flows", "network"]
            }
        }
    })JSON"sv.bytes()));
    }
    auto manifest = MUST(WebView::AppManifest::load_from_file(manifest_path));

    u32 cron_invocations = 0;
    u32 consent_calls = 0;
    bool approve_consent = false;

    auto build_cfg = [&]() -> WebView::AppModeServerConfig {
        return {
            .manifest = manifest,
            .data_directory = data_dir,
            .disable_sql_database = true,
            .enable_webmcp_shim = false,
            .on_service_list = []() -> Vector<WebView::AppServiceTabState> { return {}; },
            .on_service_register_consent = [&](WebView::AppServiceConfig const&, Function<void(bool approved)> on_decision) {
                consent_calls++;
                on_decision(approve_consent);
            },
            .on_service_run_flow = [&](StringView, Optional<String>, Optional<WebView::FlowDefinition>, HashMap<String, String>, HashTable<String>, bool, bool, bool, bool, Function<void(Optional<WebView::FlowRunRecord>, Optional<String>)> on_done) {
                cron_invocations++;
                on_done(WebView::FlowRunRecord { .run_id = "run-cron-1"_string, .flow_id = "f-cron"_string, .flow_name = "Cron"_string, .status = "passed"_string, .tab_id = {}, .started_at_ms = 1, .finished_at_ms = 2, .duration_ms = 1, .current_step_index = 0, .total_steps = 0, .error = {}, .outputs = {}, .steps = {} }, {});
            },
        };
    };

    {
        auto server = MUST(WebView::AppModeServer::create(build_cfg()));
        auto port = server->local_port();
        auto origin = server->origin();
        auto token = server->session_token();

        // 1. SSE auth enforcement (403 without/wrong token) & live streaming
        auto bad_sse = send_app_http(port, "GET"sv, "/__ladybird_events"sv);
        EXPECT_EQ(bad_sse.status_code, 403);

        auto sse_socket = MUST(Core::TCPSocket::connect(Core::SocketAddress { IPv4Address { 127, 0, 0, 1 }, port }));
        StringBuilder sse_req;
        sse_req.appendff("GET /__ladybird_events?token={} HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nAccept: text/event-stream\r\n\r\n", token, port);
        MUST(sse_socket->write_until_depleted(sse_req.string_view().bytes()));
        MUST(sse_socket->set_blocking(false));

        StringBuilder sse_buf;
        sse_socket->on_ready_to_read = [&] {
            Array<u8, 8192> chunk;
            for (;;) {
                auto r = sse_socket->read_some(chunk);
                if (r.is_error() || r.value().is_empty())
                    break;
                sse_buf.append(StringView { r.value() });
            }
        };

        bool sse_timed_out = false;
        auto wait_for_substr = [&](StringView needle) {
            sse_timed_out = false;
            auto t = Core::Timer::create_single_shot(AK::Duration::from_seconds(3), [&] { sse_timed_out = true; });
            t->start();
            Core::EventLoop::current().spin_until([&] {
                return sse_timed_out || sse_buf.string_view().contains(needle);
            });
            t->stop();
            EXPECT(!sse_timed_out);
        };

        wait_for_substr(": connected\n\n"sv);
        EXPECT(sse_buf.string_view().contains("Content-Type: text/event-stream"sv));
        EXPECT_EQ(server->sse_client_count(), 1uz);

        // Trigger a live notification event and verify immediate SSE push on both notification and event channels
        server->push_service_notification("https://mail.example.com"sv, 7, "shown"sv, "Live SSE Alert"_string, "Instant delivery"_string, "tag1"sv, 123456);
        wait_for_substr("Live SSE Alert"sv);
        EXPECT(sse_buf.string_view().contains("event: notification\n"sv));
        EXPECT(sse_buf.string_view().contains("event: event\n"sv));

        sse_socket->close();
        bool close_timed_out = false;
        auto close_timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(3), [&] { close_timed_out = true; });
        close_timer->start();
        Core::EventLoop::current().spin_until([&] {
            return close_timed_out || server->sse_client_count() == 0;
        });
        close_timer->stop();
        EXPECT(!close_timed_out);

        // 2. Native Cron triggers: validation, slot deduplication & clientHandled
        auto bad_cron = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"triggers.save","args":{"trigger":{"id":"bad-cron","type":"cron","serviceId":"mail","flowId":"f-cron","cronExpr":"bad cron"}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(bad_cron.status_code, 400);

        auto ok_cron = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"triggers.save","args":{"trigger":{"id":"morning-cron","type":"cron","serviceId":"mail","flowId":"f-cron","cronExpr":"15 10 * * *"}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(ok_cron.status_code, 200);

        // Non-matching minute -> does not fire
        server->evaluate_cron_at(2026, 11, 28, 10, 14, 6, 1795860840000LL);
        EXPECT_EQ(cron_invocations, 0u);

        // Matching minute twice in same slot -> fires exactly once
        server->evaluate_cron_at(2026, 11, 28, 10, 15, 6, 1795860900000LL);
        server->evaluate_cron_at(2026, 11, 28, 10, 15, 6, 1795860920000LL);
        EXPECT_EQ(cron_invocations, 1u);

        // 3. Dynamic service consent gate
        approve_consent = false;
        auto denied_svc = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"tabs.registerService","args":{"service":{"id":"crm-ext","label":"CRM","defaultUrl":"https://crm.example.com/","origins":["https://crm.example.com"],"container":"work","permissions":["open","navigate"]}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(denied_svc.status_code, 403);
        EXPECT_EQ(consent_calls, 1u);
        EXPECT(!server->find_effective_service("crm-ext"sv).has_value());

        approve_consent = true;
        auto approved_svc = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"tabs.registerService","args":{"service":{"id":"crm-ext","label":"CRM","defaultUrl":"https://crm.example.com/","origins":["https://crm.example.com"],"container":"work","permissions":["open","navigate"]}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(approved_svc.status_code, 200);
        EXPECT_EQ(consent_calls, 2u);
        EXPECT(server->find_effective_service("crm-ext"sv).has_value());

        // Re-registering unchanged origins/container/permissions skips consent re-prompt
        auto same_svc = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"tabs.registerService","args":{"service":{"id":"crm-ext","label":"CRM Renamed","defaultUrl":"https://crm.example.com/home","origins":["https://crm.example.com"],"container":"work","permissions":["open","navigate"]}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(same_svc.status_code, 200);
        EXPECT_EQ(consent_calls, 2u);

        // Changing container re-triggers consent prompt
        auto changed_svc = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"JSON({"command":"tabs.registerService","args":{"service":{"id":"crm-ext","label":"CRM Renamed","defaultUrl":"https://crm.example.com/home","origins":["https://crm.example.com"],"container":"personal","permissions":["open","navigate"]}}})JSON"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(changed_svc.status_code, 200);
        EXPECT_EQ(consent_calls, 3u);
    }

    // Cron trigger & consented dynamic service persist across restart
    auto server2 = MUST(WebView::AppModeServer::create(build_cfg()));
    EXPECT(server2->find_trigger("morning-cron"sv).has_value());
    EXPECT(server2->find_effective_service("crm-ext"sv).has_value());
}

TEST_CASE(app_mode_offline_airgap_and_cross_origin_isolation)
{
    Core::EventLoop loop;
    Test::TemporaryDirectory temp { "TestAppModeOffline"sv };
    auto root = MUST(FileSystem::real_path(temp.path().string()));
    auto app_dir = LexicalPath::join(root, "offline-app"sv).string();
    auto data_dir = LexicalPath::join(root, "data"sv).string();
    MUST(Core::Directory::create(app_dir, Core::Directory::CreateDirectories::Yes));
    MUST(Core::Directory::create(data_dir, Core::Directory::CreateDirectories::Yes));

    auto index_path = LexicalPath::join(app_dir, "index.html"sv).string();
    auto index_file = MUST(Core::File::open(index_path, Core::File::OpenMode::Write));
    MUST(index_file->write_until_depleted("<!doctype html><html><body>Offline Wasm App</body></html>"sv.bytes()));
    index_file->close();

    auto write_manifest = [&](StringView filename, StringView content) {
        auto path = LexicalPath::join(app_dir, filename).string();
        auto f = MUST(Core::File::open(path, Core::File::OpenMode::Write));
        MUST(f->write_until_depleted(content.bytes()));
        f->close();
        return path;
    };

    // Invalid network policy and invalid crossOriginIsolated type must be rejected
    auto bad_net = write_manifest("bad-net.json"sv, R"JSON({
        "id": "bad-net",
        "name": "Bad Net",
        "permissions": { "network": "tor-only" }
    })JSON"sv);
    EXPECT(WebView::AppManifest::load_from_file(bad_net).is_error());

    auto bad_coi = write_manifest("bad-coi.json"sv, R"JSON({
        "id": "bad-coi",
        "name": "Bad COI",
        "permissions": { "crossOriginIsolated": "yes" }
    })JSON"sv);
    EXPECT(WebView::AppManifest::load_from_file(bad_coi).is_error());

    // Offline + crossOriginIsolated manifest
    auto offline_path = write_manifest("ladybird-app.json"sv, R"JSON({
        "id": "offline-wasm",
        "name": "Offline Wasm App",
        "permissions": {
            "network": "offline",
            "crossOriginIsolated": true
        }
    })JSON"sv);
    auto offline_manifest = MUST(WebView::AppManifest::load_from_file(offline_path));
    EXPECT_EQ(offline_manifest.permissions.network, WebView::AppNetworkPolicy::Offline);
    EXPECT(offline_manifest.permissions.cross_origin_isolated);

    {
        auto server = MUST(WebView::AppModeServer::create({
            .manifest = offline_manifest,
            .data_directory = data_dir,
            .disable_sql_database = true,
            .enable_webmcp_shim = false,
        }));
        auto port = server->local_port();
        auto origin = server->origin();
        auto token = server->session_token();

        auto page = send_app_http(port, "GET"sv, "/"sv);
        EXPECT_EQ(page.status_code, 200);
        EXPECT(page.headers.contains("Content-Security-Policy:"sv));
        EXPECT(page.headers.contains("connect-src 'self'"sv));
        EXPECT(page.headers.contains("'wasm-unsafe-eval'"sv));
        EXPECT(page.headers.contains("form-action 'self'"sv));
        EXPECT(page.headers.contains("Cross-Origin-Opener-Policy: same-origin"sv));
        EXPECT(page.headers.contains("Cross-Origin-Embedder-Policy: require-corp"sv));
        EXPECT(page.headers.contains("Cross-Origin-Resource-Policy: same-origin"sv));

        auto bridge = send_app_http(port, "GET"sv, "/__ladybird_bridge.js"sv);
        EXPECT_EQ(bridge.status_code, 200);
        EXPECT(bridge.headers.contains("Cross-Origin-Embedder-Policy: require-corp"sv));
        EXPECT(bridge.headers.contains("Cross-Origin-Resource-Policy: same-origin"sv));
        EXPECT(bridge.body.contains("\"network\":\"offline\""sv));
        EXPECT(bridge.body.contains("\"crossOriginIsolated\":true"sv));

        auto info = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"system.info","args":{}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(info.status_code, 200);
        EXPECT(info.body.contains("\"networkPolicy\":\"offline\""sv));
        EXPECT(info.body.contains("\"crossOriginIsolated\":true"sv));

        // Offline App Mode must block networked service IPC commands and inbound webhooks
        auto blocked_open = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.open","args":{"serviceId":"mail"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(blocked_open.status_code, 403);
        auto blocked_reg = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.registerService","args":{"id":"x"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(blocked_reg.status_code, 403);
        auto blocked_call = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.callTool","args":{"serviceId":"mail","toolName":"t"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
        EXPECT_EQ(blocked_call.status_code, 403);
        auto blocked_hook = send_app_http(port, "POST"sv, "/__ladybird_webhook/hook1"sv, R"({"x":1})"sv);
        EXPECT_EQ(blocked_hook.status_code, 403);

        // SSE stream must include Cross-Origin-Resource-Policy: same-origin under COEP
        auto sse_socket = MUST(Core::TCPSocket::connect(Core::SocketAddress { IPv4Address { 127, 0, 0, 1 }, port }));
        StringBuilder sse_req;
        sse_req.appendff("GET /__ladybird_events?token={} HTTP/1.1\r\nHost: 127.0.0.1:{}\r\nAccept: text/event-stream\r\n\r\n", token, port);
        MUST(sse_socket->write_until_depleted(sse_req.string_view().bytes()));
        MUST(sse_socket->set_blocking(false));

        StringBuilder sse_buf;
        sse_socket->on_ready_to_read = [&] {
            Array<u8, 4096> chunk;
            for (;;) {
                auto r = sse_socket->read_some(chunk);
                if (r.is_error() || r.value().is_empty())
                    break;
                sse_buf.append(StringView { r.value() });
            }
        };
        bool timed_out = false;
        auto timer = Core::Timer::create_single_shot(AK::Duration::from_seconds(3), [&] { timed_out = true; });
        timer->start();
        Core::EventLoop::current().spin_until([&] {
            return timed_out || sse_buf.string_view().contains(": connected\n\n"sv);
        });
        timer->stop();
        sse_socket->close();
        EXPECT(!timed_out);
        EXPECT(sse_buf.string_view().contains("Cross-Origin-Resource-Policy: same-origin"sv));
    }

    // Default unrestricted manifest must not emit offline CSP or COEP
    auto default_path = write_manifest("default.json"sv, R"JSON({
        "id": "default-app",
        "name": "Default App"
    })JSON"sv);
    auto default_manifest = MUST(WebView::AppManifest::load_from_file(default_path));
    EXPECT_EQ(default_manifest.permissions.network, WebView::AppNetworkPolicy::Unrestricted);
    EXPECT(!default_manifest.permissions.cross_origin_isolated);

    auto default_server = MUST(WebView::AppModeServer::create({
        .manifest = default_manifest,
        .data_directory = data_dir,
        .disable_sql_database = true,
        .enable_webmcp_shim = false,
    }));
    auto default_page = send_app_http(default_server->local_port(), "GET"sv, "/"sv);
    EXPECT_EQ(default_page.status_code, 200);
    EXPECT(!default_page.headers.contains("Content-Security-Policy:"sv));
    EXPECT(!default_page.headers.contains("Cross-Origin-Embedder-Policy:"sv));
    EXPECT(default_page.headers.contains("Cross-Origin-Resource-Policy: same-origin"sv));

    // Bridge script must always emit CORP: same-origin and reject cross-site fetches even when COI is false
    auto default_bridge = send_app_http(default_server->local_port(), "GET"sv, "/__ladybird_bridge.js"sv);
    EXPECT_EQ(default_bridge.status_code, 200);
    EXPECT(default_bridge.headers.contains("Cross-Origin-Resource-Policy: same-origin"sv));

    auto cross_site_bridge = send_app_http(default_server->local_port(), "GET"sv, "/__ladybird_bridge.js"sv, {}, {}, {}, "Sec-Fetch-Site: cross-site\r\n"sv);
    EXPECT_EQ(cross_site_bridge.status_code, 403);
}

TEST_CASE(app_mode_service_read_markdown_and_webmcp_ipc)
{
    Core::EventLoop loop;
    Test::TemporaryDirectory temp { "TestAppModeInterTab"sv };
    auto root = MUST(FileSystem::real_path(temp.path().string()));
    auto app_dir = LexicalPath::join(root, "app"sv).string();
    auto data_dir = LexicalPath::join(root, "data"sv).string();
    MUST(Core::Directory::create(app_dir, Core::Directory::CreateDirectories::Yes));
    MUST(Core::Directory::create(data_dir, Core::Directory::CreateDirectories::Yes));

    auto index_file = MUST(Core::File::open(LexicalPath::join(app_dir, "index.html"sv).string(), Core::File::OpenMode::Write));
    MUST(index_file->write_until_depleted("<!doctype html><html><body>Hub</body></html>"sv.bytes()));
    index_file->close();

    auto manifest_path = LexicalPath::join(app_dir, "ladybird-app.json"sv).string();
    auto mf = MUST(Core::File::open(manifest_path, Core::File::OpenMode::Write));
    MUST(mf->write_until_depleted(R"JSON({
        "id": "intertab-broker",
        "name": "Inter-Tab Broker",
        "services": {
            "reader": {
                "label": "Reader Only",
                "defaultUrl": "https://mail.example.com/",
                "origins": ["https://mail.example.com"],
                "container": "work",
                "permissions": ["open", "read"]
            },
            "tools": {
                "label": "Tools Only",
                "defaultUrl": "https://github.example.com/",
                "origins": ["https://github.example.com"],
                "container": "work",
                "permissions": ["open", "webmcp"]
            }
        }
    })JSON"sv.bytes()));
    mf->close();

    auto manifest = MUST(WebView::AppManifest::load_from_file(manifest_path));
    auto reader_svc = manifest.find_service("reader"sv);
    auto tools_svc = manifest.find_service("tools"sv);
    EXPECT(reader_svc.has_value());
    EXPECT(tools_svc.has_value());
    EXPECT(reader_svc->allow_read);
    EXPECT(!reader_svc->allow_webmcp);
    EXPECT(!tools_svc->allow_read);
    EXPECT(tools_svc->allow_webmcp);

    auto ser_reader = WebView::AppManifest::serialize_service_config(*reader_svc).serialized();
    EXPECT(ser_reader.contains("\"read\""sv));
    EXPECT(!ser_reader.contains("\"webmcp\""sv));

    auto server = MUST(WebView::AppModeServer::create({
        .manifest = move(manifest),
        .data_directory = data_dir,
        .disable_sql_database = true,
        .enable_webmcp_shim = false,
        .on_service_list = []() -> Vector<WebView::AppServiceTabState> { return {}; },
        .on_service_read_markdown = [](StringView service_id, Optional<String> root_selector, bool main_only, bool include_links, bool include_images, size_t max_chars, Function<void(Optional<JsonValue>, Optional<String>)> on_complete) {
            JsonObject out;
            out.set("markdown"sv, MUST(String::formatted("# Markdown from {}", service_id)));
            out.set("mainContentOnly"sv, main_only);
            out.set("includeLinks"sv, include_links);
            out.set("includeImages"sv, include_images);
            out.set("maxChars"sv, static_cast<u64>(max_chars));
            if (root_selector.has_value())
                out.set("rootSelector"sv, *root_selector);
            on_complete(JsonValue { move(out) }, {});
        },
        .on_service_list_tools = [](StringView service_id, bool include_declarative, Function<void(Optional<JsonValue>, Optional<String>)> on_complete) {
            JsonObject out;
            JsonArray tools;
            JsonObject tool;
            tool.set("name"sv, "get_issue"sv);
            tools.must_append(move(tool));
            out.set("tools"sv, move(tools));
            out.set("includeDeclarative"sv, include_declarative);
            out.set("fromService"sv, service_id);
            on_complete(JsonValue { move(out) }, {});
        },
        .on_service_call_tool = [](StringView service_id, String tool_name, JsonObject arguments, Function<void(Optional<JsonValue>, Optional<String>)> on_complete) {
            JsonObject out;
            out.set("ok"sv, true);
            out.set("called"sv, tool_name);
            out.set("argId"sv, arguments.get_i64("id"sv).value_or(0));
            out.set("fromService"sv, service_id);
            on_complete(JsonValue { move(out) }, {});
        },
    }));

    auto port = server->local_port();
    auto origin = server->origin();
    auto token = server->session_token();

    auto list_res = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.list","args":{}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(list_res.status_code, 200);
    EXPECT(list_res.body.contains("\"read\":true"sv));
    EXPECT(list_res.body.contains("\"webmcp\":true"sv));

    // tabs.readMarkdown permitted on 'reader', forbidden on 'tools'
    auto ok_md = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.readMarkdown","args":{"serviceId":"reader","rootSelector":"main","maxChars":2500}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(ok_md.status_code, 200);
    EXPECT(ok_md.body.contains("# Markdown from reader"sv));
    EXPECT(ok_md.body.contains("\"serviceId\":\"reader\""sv));

    auto denied_md = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.readMarkdown","args":{"serviceId":"tools"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(denied_md.status_code, 403);

    // tabs.listTools & tabs.callTool permitted on 'tools', forbidden on 'reader'
    auto ok_tools = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.listTools","args":{"serviceId":"tools"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(ok_tools.status_code, 200);
    EXPECT(ok_tools.body.contains("\"get_issue\""sv));

    auto denied_tools = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.listTools","args":{"serviceId":"reader"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(denied_tools.status_code, 403);

    auto ok_call = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.callTool","args":{"serviceId":"tools","toolName":"get_issue","arguments":{"id":42}}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(ok_call.status_code, 200);
    EXPECT(ok_call.body.contains("\"argId\":42"sv));
    EXPECT(ok_call.body.contains("\"toolName\":\"get_issue\""sv));

    auto denied_call = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.callTool","args":{"serviceId":"reader","toolName":"get_issue"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(denied_call.status_code, 403);

    auto bad_call = send_app_http(port, "POST"sv, "/__ladybird_ipc"sv, R"({"command":"tabs.callTool","args":{"serviceId":"tools"}})"sv, origin.bytes_as_string_view(), token.bytes_as_string_view());
    EXPECT_EQ(bad_call.status_code, 400);

    auto bridge = send_app_http(port, "GET"sv, "/__ladybird_bridge.js"sv);
    EXPECT_EQ(bridge.status_code, 200);
    EXPECT(bridge.body.contains("readMarkdown:"sv));
    EXPECT(bridge.body.contains("listTools:"sv));
    EXPECT(bridge.body.contains("callTool:"sv));
}
