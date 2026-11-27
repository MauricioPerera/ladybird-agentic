/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/ContainerStore.h>
#include <LibWebView/Utilities.h>

namespace WebView {

bool is_valid_container_id(StringView id)
{
    if (id.is_empty() || id.length() > 64)
        return false;

    auto first = id[0];
    if (!((first >= 'a' && first <= 'z') || (first >= '0' && first <= '9')))
        return false;

    for (auto ch : id) {
        bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!ok)
            return false;
    }
    return true;
}

bool is_valid_container_color(StringView color)
{
    if (color.length() != 7 || color[0] != '#')
        return false;
    for (size_t i = 1; i < color.length(); ++i) {
        if (!is_ascii_hex_digit(color[i]))
            return false;
    }
    return true;
}

Vector<TabContainer> default_tab_containers()
{
    return {
        { "personal"_string, "Personal"_string, "#3b82f6"_string },
        { "work"_string, "Work"_string, "#f97316"_string },
        { "banking"_string, "Banking"_string, "#10b981"_string },
        { "shopping"_string, "Shopping"_string, "#ec4899"_string },
        { "agent"_string, "Agent"_string, "#8b5cf6"_string },
    };
}

static String slugify_container_name(StringView name)
{
    StringBuilder builder;
    bool pending_dash = false;
    for (auto byte : name.bytes()) {
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<u8>(byte + ('a' - 'A'));
        if ((byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9')) {
            if (pending_dash && !builder.is_empty())
                builder.append('-');
            builder.append(static_cast<char>(byte));
            pending_dash = false;
        } else if (!builder.is_empty()) {
            pending_dash = true;
        }
        if (builder.length() >= 56)
            break;
    }

    auto slug = MUST(builder.to_string());
    if (!is_valid_container_id(slug))
        return "container"_string;
    return slug;
}

ContainerStore::ContainerStore(ByteString persistence_path)
    : m_persistence_path(move(persistence_path))
{
    reload();
}

ContainerStore::~ContainerStore() = default;

void ContainerStore::reload()
{
    m_containers.clear();

    if (!FileSystem::exists(m_persistence_path)) {
        m_containers = default_tab_containers();
        (void)save();
        return;
    }

    auto json = read_json_file(m_persistence_path);
    if (json.is_error()) {
        warnln("Could not read container store {}: {}", m_persistence_path, json.error());
        m_containers = default_tab_containers();
        return;
    }

    auto containers = json.value().get_array("containers"sv);
    if (!containers.has_value())
        return;

    HashTable<String> seen_ids;
    for (auto const& value : containers->values()) {
        if (!value.is_object())
            continue;
        auto const& obj = value.as_object();
        auto id = obj.get_string("id"sv);
        auto name = obj.get_string("name"sv);
        auto color = obj.get_string("color"sv);
        if (!id.has_value() || !name.has_value() || !color.has_value())
            continue;
        if (!is_valid_container_id(*id) || !is_valid_container_color(*color))
            continue;
        auto trimmed_name = name->bytes_as_string_view().trim_whitespace();
        if (trimmed_name.is_empty() || trimmed_name.length() > 64)
            continue;
        if (seen_ids.contains(*id))
            continue;

        seen_ids.set(*id);
        m_containers.append({
            .id = *id,
            .name = MUST(String::from_utf8(trimmed_name)),
            .color = color->to_ascii_lowercase(),
        });
    }
}

Optional<TabContainer> ContainerStore::get_container(StringView id) const
{
    for (auto const& container : m_containers) {
        if (container.id == id)
            return container;
    }
    return {};
}

ErrorOr<TabContainer> ContainerStore::create_container(String name, String color, Optional<String> requested_id)
{
    auto trimmed = name.bytes_as_string_view().trim_whitespace();
    if (trimmed.is_empty() || trimmed.length() > 64)
        return Error::from_string_literal("Container name must be between 1 and 64 characters");
    auto clean_name = MUST(String::from_utf8(trimmed));

    auto clean_color = color.is_empty() ? "#3b82f6"_string : color.to_ascii_lowercase();
    if (!is_valid_container_color(clean_color))
        return Error::from_string_literal("Container color must be a #RRGGBB hex string");

    String id;
    if (requested_id.has_value() && !requested_id->is_empty()) {
        if (!is_valid_container_id(*requested_id))
            return Error::from_string_literal("Invalid container ID");
        if (get_container(*requested_id).has_value())
            return Error::from_string_literal("A container with that ID already exists");
        id = *requested_id;
    } else {
        auto base = slugify_container_name(trimmed);
        id = base;
        size_t suffix = 2;
        while (get_container(id).has_value()) {
            id = MUST(String::formatted("{}-{}", base, suffix++));
        }
    }

    TabContainer created {
        .id = move(id),
        .name = move(clean_name),
        .color = move(clean_color),
    };
    m_containers.append(created);
    if (auto res = save(); res.is_error()) {
        m_containers.take_last();
        return res.release_error();
    }
    notify_containers_changed();
    return created;
}

ErrorOr<void> ContainerStore::delete_container(StringView id)
{
    if (!is_valid_container_id(id))
        return Error::from_string_literal("Invalid container ID");

    Optional<size_t> index;
    for (size_t i = 0; i < m_containers.size(); ++i) {
        if (m_containers[i].id == id) {
            index = i;
            break;
        }
    }
    if (!index.has_value())
        return Error::from_string_literal("Container not found");

    auto removed = m_containers.take(*index);
    if (auto res = save(); res.is_error()) {
        m_containers.insert(*index, move(removed));
        return res.release_error();
    }
    notify_containers_changed();
    return {};
}

ErrorOr<void> ContainerStore::save() const
{
    JsonArray array;
    for (auto const& container : m_containers) {
        JsonObject obj;
        obj.set("id"sv, container.id);
        obj.set("name"sv, container.name);
        obj.set("color"sv, container.color);
        array.must_append(move(obj));
    }

    JsonObject root;
    root.set("schema_version"sv, 1);
    root.set("containers"sv, move(array));
    return write_json_file(m_persistence_path, JsonValue { move(root) });
}

void ContainerStore::add_observer(ContainerStoreObserver& observer)
{
    m_observers.append(observer);
}

void ContainerStore::remove_observer(ContainerStoreObserver& observer)
{
    m_observers.remove_first_matching([&](ContainerStoreObserver const& existing) { return &existing == &observer; });
}

void ContainerStore::notify_containers_changed()
{
    for (auto& observer : m_observers)
        observer.containers_changed();
}

}
