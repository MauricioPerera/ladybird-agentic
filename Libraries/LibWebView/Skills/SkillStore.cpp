/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Hex.h>
#include <AK/QuickSort.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCrypto/Hash/Hash.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Skills/SkillStore.h>

namespace WebView {

static ByteString normalize_path_slashes(ByteString const& path)
{
    auto normalized = LexicalPath::canonicalized_path(path).replace("\\"sv, "/"sv, ReplaceMode::All);
    while (normalized.length() > 1 && normalized.ends_with('/'))
        normalized = normalized.substring(0, normalized.length() - 1);
    return normalized;
}

static bool is_path_inside_root(ByteString const& root, ByteString const& candidate)
{
    auto norm_root = normalize_path_slashes(root);
    auto norm_candidate = normalize_path_slashes(candidate);
    auto prefix = norm_root == "/"sv ? ByteString { "/" } : ByteString::formatted("{}/", norm_root);
#if defined(AK_OS_WINDOWS)
    return norm_candidate.to_lowercase() == norm_root.to_lowercase()
        || norm_candidate.to_lowercase().starts_with(prefix.to_lowercase());
#else
    return norm_candidate == norm_root || norm_candidate.starts_with(prefix);
#endif
}

static ErrorOr<ByteString> resolve_safe_skill_entry_path(ByteString const& skill_root, StringView rel_path, bool expect_directory)
{
    if (skill_root.is_empty() || !FileSystem::is_directory(skill_root))
        return Error::from_string_literal("Skill directory does not exist");

    if (!rel_path.is_empty() && !is_safe_skill_relative_path(rel_path))
        return Error::from_string_literal("Unsafe skill relative path");

    ByteString current = skill_root;
    if (!rel_path.is_empty()) {
        for (auto segment : rel_path.split_view('/', SplitBehavior::Nothing)) {
            current = LexicalPath::join(current, segment).string();
            if (FileSystem::is_link(current))
                return Error::from_string_literal("Symbolic links are not allowed inside skills");
        }
    }

    if (!FileSystem::exists(current))
        return Error::from_string_literal("Skill resource not found");

    auto canonical_root = TRY(FileSystem::real_path(skill_root));
    auto canonical_target = TRY(FileSystem::real_path(current));
    if (!is_path_inside_root(canonical_root, canonical_target))
        return Error::from_string_literal("Skill resource escapes skill directory");

    if (expect_directory) {
        if (!FileSystem::is_directory(canonical_target))
            return Error::from_string_literal("Expected directory URI");
    } else {
        if (!FileSystem::is_regular_file(canonical_target))
            return Error::from_string_literal("Expected regular file resource");
    }

    return canonical_target;
}

static String compute_sha256_prefixed(ReadonlyBytes bytes)
{
    auto digest = Crypto::Hash::digest(Crypto::Hash::HashKind::SHA256, bytes);
    return MUST(String::formatted("sha256:{}", encode_hex(digest.bytes())));
}

static ErrorOr<void> write_utf8_file(ByteString const& path, StringView contents)
{
    auto parent = LexicalPath { path }.parent();
    TRY(Core::Directory::create(parent, Core::Directory::CreateDirectories::Yes));
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
    TRY(file->write_until_depleted(contents.bytes()));
    return {};
}

static ErrorOr<void> collect_skill_files_recursive(
    ByteString const& canonical_root,
    ByteString const& dir_path,
    StringView rel_prefix,
    StringView skill_name,
    Vector<SkillFileEntry>& out_files,
    u64& total_bytes)
{
    return Core::Directory::for_each_entry(dir_path, Core::DirIterator::SkipParentAndBaseDir, [&](Core::DirectoryEntry const& entry, Core::Directory const&) -> ErrorOr<IterationDecision> {
        auto full_path = LexicalPath::join(dir_path, entry.name).string();
        if (FileSystem::is_link(full_path))
            return IterationDecision::Continue;

        auto rel_path = rel_prefix.is_empty()
            ? MUST(String::from_utf8(entry.name.view()))
            : MUST(String::formatted("{}/{}", rel_prefix, entry.name));

        if (!is_safe_skill_relative_path(rel_path))
            return IterationDecision::Continue;

        if (entry.type == Core::DirectoryEntry::Type::Directory) {
            TRY(collect_skill_files_recursive(canonical_root, full_path, rel_path, skill_name, out_files, total_bytes));
            return IterationDecision::Continue;
        }

        if (entry.type != Core::DirectoryEntry::Type::File)
            return IterationDecision::Continue;

        auto canonical_file = TRY(FileSystem::real_path(full_path));
        if (!is_path_inside_root(canonical_root, canonical_file))
            return IterationDecision::Continue;

        if (out_files.size() >= MAX_SKILL_FILES)
            return Error::from_string_literal("Skill exceeds maximum file limit (512)");

        auto file = TRY(Core::File::open(canonical_file, Core::File::OpenMode::Read));
        auto bytes = TRY(file->read_until_eof());
        total_bytes += bytes.size();
        if (total_bytes > MAX_SKILL_TOTAL_BYTES)
            return Error::from_string_literal("Skill exceeds maximum size limit (16 MiB)");

        out_files.append({
            .relative_path = rel_path,
            .uri = skill_uri(skill_name, rel_path),
            .mime_type = guess_skill_mime_type(rel_path),
            .digest = compute_sha256_prefixed(bytes.bytes()),
            .size_bytes = bytes.size(),
        });
        return IterationDecision::Continue;
    });
}

SkillStore::SkillStore(ByteString storage_directory)
    : m_storage_directory(move(storage_directory))
{
    reload();
}

SkillStore::~SkillStore() = default;

ByteString SkillStore::skill_directory_path(StringView name) const
{
    return LexicalPath::join(m_storage_directory, name).string();
}

Optional<SkillDefinition> SkillStore::load_skill_from_directory(StringView name)
{
    if (!is_valid_skill_name(name))
        return {};

    auto dir_path = skill_directory_path(name);
    if (!FileSystem::is_directory(dir_path) || FileSystem::is_link(dir_path))
        return {};

    auto canonical_root_or = FileSystem::real_path(dir_path);
    if (canonical_root_or.is_error())
        return {};
    auto canonical_root = canonical_root_or.release_value();

    auto skill_md_path_or = resolve_safe_skill_entry_path(canonical_root, "SKILL.md"sv, false);
    if (skill_md_path_or.is_error())
        return {};

    auto file_or = Core::File::open(skill_md_path_or.value(), Core::File::OpenMode::Read);
    if (file_or.is_error()) {
        warnln("Could not open skill {}: {}", skill_md_path_or.value(), file_or.error());
        return {};
    }

    auto bytes_or = file_or.value()->read_until_eof();
    if (bytes_or.is_error()) {
        warnln("Could not read skill {}: {}", skill_md_path_or.value(), bytes_or.error());
        return {};
    }

    auto raw_text = String::from_utf8(StringView { bytes_or.value().bytes() });
    if (raw_text.is_error()) {
        warnln("Invalid UTF-8 in {}: {}", skill_md_path_or.value(), raw_text.error());
        return {};
    }

    auto parsed = parse_skill_markdown(raw_text.value(), name);
    if (parsed.is_error()) {
        warnln("Skipping invalid skill {}: {}", skill_md_path_or.value(), parsed.error());
        return {};
    }

    auto skill = parsed.release_value();
    u64 total_bytes = 0;
    if (auto walk = collect_skill_files_recursive(canonical_root, canonical_root, ""sv, skill.name, skill.resources, total_bytes); walk.is_error()) {
        warnln("Failed indexing files for skill {}: {}", skill.name, walk.error());
        return {};
    }

    quick_sort(skill.resources, [](SkillFileEntry const& a, SkillFileEntry const& b) {
        return a.relative_path < b.relative_path;
    });

    if (auto stat_or = file_or.value()->stat(); !stat_or.is_error()) {
        auto mtime_ms = static_cast<i64>(stat_or.value().st_mtime) * 1000;
        skill.updated_at_ms = mtime_ms;
        if (skill.created_at_ms <= 0)
            skill.created_at_ms = mtime_ms;
    }

    return skill;
}

void SkillStore::reload()
{
    m_skills.clear();

    if (!FileSystem::is_directory(m_storage_directory))
        return;

    auto result = Core::Directory::for_each_entry(m_storage_directory, Core::DirIterator::SkipParentAndBaseDir, [&](Core::DirectoryEntry const& entry, Core::Directory const&) -> ErrorOr<IterationDecision> {
        if (entry.type != Core::DirectoryEntry::Type::Directory)
            return IterationDecision::Continue;
        if (auto skill = load_skill_from_directory(entry.name); skill.has_value()) {
            auto skill_value = skill.release_value();
            m_skills.set(skill_value.name, move(skill_value));
        }
        return IterationDecision::Continue;
    });
    if (result.is_error())
        warnln("Failed scanning skill storage directory {}: {}", m_storage_directory, result.error());
}

Vector<SkillDefinition> SkillStore::list_skills(bool include_disabled) const
{
    Vector<SkillDefinition> skills;
    skills.ensure_capacity(m_skills.size());
    for (auto const& [_, skill] : m_skills) {
        if (include_disabled || skill.enabled)
            skills.unchecked_append(skill);
    }

    quick_sort(skills, [](SkillDefinition const& a, SkillDefinition const& b) {
        return a.name < b.name;
    });
    return skills;
}

Vector<SkillDefinition> SkillStore::matching_skills_for_url(URL::URL const& url) const
{
    Vector<SkillDefinition> matches;
    for (auto const& [_, skill] : m_skills) {
        if (skill_matches_url(skill, url))
            matches.append(skill);
    }

    quick_sort(matches, [](SkillDefinition const& a, SkillDefinition const& b) {
        return a.name < b.name;
    });
    return matches;
}

Optional<SkillDefinition> SkillStore::get_skill(StringView name, bool include_disabled) const
{
    if (!is_valid_skill_name(name))
        return {};
    auto found = m_skills.get(name);
    if (!found.has_value())
        return {};
    if (!include_disabled && !found->enabled)
        return {};
    return *found;
}

Optional<SkillDefinition> SkillStore::get_skill_by_uri(StringView uri, bool include_disabled) const
{
    auto parsed = parse_skill_uri(uri, false);
    if (!parsed.has_value() || parsed->relative_path != "SKILL.md"sv)
        return {};
    return get_skill(parsed->name, include_disabled);
}

ErrorOr<Vector<SkillSupportingFile>> SkillStore::read_supporting_files(StringView name) const
{
    auto skill = get_skill(name, true);
    if (!skill.has_value())
        return Error::from_string_literal("Skill not found");

    Vector<SkillSupportingFile> files;
    auto dir_path = skill_directory_path(name);
    for (auto const& res : skill->resources) {
        if (res.relative_path == "SKILL.md"sv)
            continue;
        auto file_path = TRY(resolve_safe_skill_entry_path(dir_path, res.relative_path, false));
        auto file = TRY(Core::File::open(file_path, Core::File::OpenMode::Read));
        auto bytes = TRY(file->read_until_eof());
        auto text = TRY(String::from_utf8(StringView { bytes.bytes() }));
        files.append({
            .relative_path = res.relative_path,
            .content = move(text),
        });
    }
    return files;
}

ErrorOr<SkillResourceContent> SkillStore::read_skill_resource(StringView uri, bool include_disabled) const
{
    auto parsed = parse_skill_uri(uri, false);
    if (!parsed.has_value())
        return Error::from_string_literal("Invalid skill resource URI");

    auto skill = get_skill(parsed->name, include_disabled);
    if (!skill.has_value())
        return Error::from_string_literal("Unknown or disabled skill");

    Optional<SkillFileEntry> manifest_entry;
    for (auto const& res : skill->resources) {
        if (res.relative_path == parsed->relative_path) {
            manifest_entry = res;
            break;
        }
    }
    if (!manifest_entry.has_value())
        return Error::from_string_literal("Resource URI is not in the skill manifest");

    auto full_path = TRY(resolve_safe_skill_entry_path(skill_directory_path(skill->name), parsed->relative_path, false));
    auto file = TRY(Core::File::open(full_path, Core::File::OpenMode::Read));
    auto bytes = TRY(file->read_until_eof());
    auto actual_digest = compute_sha256_prefixed(bytes.bytes());
    u64 actual_size = bytes.size();
    if (actual_digest != manifest_entry->digest || actual_size != manifest_entry->size_bytes)
        return Error::from_string_literal("Skill resource changed on disk; digest mismatch against manifest");

    auto text = TRY(String::from_utf8(StringView { bytes.bytes() }));
    return SkillResourceContent {
        .uri = manifest_entry->uri,
        .mime_type = manifest_entry->mime_type,
        .text = move(text),
        .digest = move(actual_digest),
        .size_bytes = actual_size,
    };
}

ErrorOr<Vector<SkillDirectoryEntry>> SkillStore::read_skill_directory(StringView uri, bool include_disabled) const
{
    if (uri.ends_with('/'))
        return Error::from_string_literal("Directory URIs must not have a trailing slash");

    auto parsed = parse_skill_uri(uri, true);
    if (!parsed.has_value())
        return Error::from_string_literal("Invalid skill directory URI");

    auto skill = get_skill(parsed->name, include_disabled);
    if (!skill.has_value())
        return Error::from_string_literal("Unknown or disabled skill");

    auto dir_path = TRY(resolve_safe_skill_entry_path(skill_directory_path(skill->name), parsed->relative_path, true));

    Vector<SkillDirectoryEntry> entries;
    TRY(Core::Directory::for_each_entry(dir_path, Core::DirIterator::SkipParentAndBaseDir, [&](Core::DirectoryEntry const& entry, Core::Directory const&) -> ErrorOr<IterationDecision> {
        auto full_entry_path = LexicalPath::join(dir_path, entry.name).string();
        if (FileSystem::is_link(full_entry_path))
            return IterationDecision::Continue;

        auto child_rel = parsed->relative_path.is_empty()
            ? MUST(String::from_utf8(entry.name.view()))
            : MUST(String::formatted("{}/{}", parsed->relative_path, entry.name));

        if (!is_safe_skill_relative_path(child_rel))
            return IterationDecision::Continue;

        if (entry.type == Core::DirectoryEntry::Type::Directory) {
            entries.append({
                .uri = skill_uri(skill->name, child_rel),
                .name = MUST(String::from_utf8(entry.name.view())),
                .mime_type = "inode/directory"_string,
            });
        } else if (entry.type == Core::DirectoryEntry::Type::File) {
            entries.append({
                .uri = skill_uri(skill->name, child_rel),
                .name = MUST(String::from_utf8(entry.name.view())),
                .mime_type = guess_skill_mime_type(child_rel),
            });
        }
        return IterationDecision::Continue;
    }));

    quick_sort(entries, [](SkillDirectoryEntry const& a, SkillDirectoryEntry const& b) {
        return a.name < b.name;
    });
    return entries;
}

ErrorOr<SkillDefinition> SkillStore::save_skill(SkillDefinition skill, Optional<Vector<SkillSupportingFile>> supporting_files)
{
    if (!is_valid_skill_name(skill.name))
        return Error::from_string_literal("Invalid skill 'name'");
    if (skill.description.bytes_as_string_view().trim(" \t\r\n"sv).is_empty())
        return Error::from_string_literal("Skill 'description' must be non-empty");

    auto now_ms = UnixDateTime::now().milliseconds_since_epoch();
    if (auto existing = m_skills.get(skill.name); existing.has_value() && existing->created_at_ms > 0)
        skill.created_at_ms = existing->created_at_ms;
    else if (skill.created_at_ms <= 0)
        skill.created_at_ms = now_ms;
    skill.updated_at_ms = now_ms;

    if (supporting_files.has_value()) {
        if (supporting_files->size() + 1 > MAX_SKILL_FILES)
            return Error::from_string_literal("Too many files in skill (max 512)");
        for (auto const& f : *supporting_files) {
            if (!is_safe_skill_relative_path(f.relative_path) || f.relative_path == "SKILL.md"sv)
                return Error::from_string_literal("Invalid supporting file relative path");
        }
    }

    auto dir_path = skill_directory_path(skill.name);
    TRY(Core::Directory::create(dir_path, Core::Directory::CreateDirectories::Yes));

    if (supporting_files.has_value()) {
        if (auto existing = m_skills.get(skill.name); existing.has_value()) {
            for (auto const& old_res : existing->resources) {
                if (old_res.relative_path == "SKILL.md"sv)
                    continue;
                bool kept = false;
                for (auto const& next_f : *supporting_files) {
                    if (next_f.relative_path == old_res.relative_path) {
                        kept = true;
                        break;
                    }
                }
                if (!kept) {
                    if (auto old_full = resolve_safe_skill_entry_path(dir_path, old_res.relative_path, false); !old_full.is_error())
                        (void)FileSystem::remove(old_full.value(), FileSystem::RecursionMode::Disallowed);
                }
            }
        }

        for (auto const& f : *supporting_files) {
            ByteString dest = dir_path;
            for (auto seg : f.relative_path.bytes_as_string_view().split_view('/', SplitBehavior::Nothing))
                dest = LexicalPath::join(dest, seg).string();
            TRY(write_utf8_file(dest, f.content));
        }
    }

    auto rendered_md = render_skill_markdown(skill);
    auto skill_md_path = LexicalPath::join(dir_path, "SKILL.md"sv).string();
    TRY(write_utf8_file(skill_md_path, rendered_md));

    auto reloaded = load_skill_from_directory(skill.name);
    if (!reloaded.has_value())
        return Error::from_string_literal("Saved skill could not be reloaded from disk");

    auto saved_skill = reloaded.release_value();
    saved_skill.created_at_ms = skill.created_at_ms;
    saved_skill.updated_at_ms = skill.updated_at_ms;
    m_skills.set(saved_skill.name, saved_skill);
    notify_skills_changed();
    return saved_skill;
}

ErrorOr<SkillDefinition> SkillStore::set_skill_enabled(StringView name, bool enabled)
{
    auto existing = get_skill(name, true);
    if (!existing.has_value())
        return Error::from_string_literal("Skill not found");

    auto updated = *existing;
    updated.enabled = enabled;
    return save_skill(move(updated), OptionalNone {});
}

ErrorOr<void> SkillStore::delete_skill(StringView name)
{
    if (!is_valid_skill_name(name))
        return Error::from_string_literal("Invalid skill 'name'");

    auto dir_path = skill_directory_path(name);
    bool existed_on_disk = FileSystem::exists(dir_path);
    if (existed_on_disk)
        TRY(FileSystem::remove(dir_path, FileSystem::RecursionMode::Allowed));

    bool removed_in_memory = m_skills.remove(name);
    if (!existed_on_disk && !removed_in_memory)
        return Error::from_string_literal("Skill not found");

    notify_skills_changed();
    return {};
}

void SkillStore::add_observer(SkillStoreObserver& observer)
{
    m_observers.append(observer);
}

void SkillStore::remove_observer(SkillStoreObserver& observer)
{
    m_observers.remove_first_matching([&](auto const& entry) {
        return &entry == &observer;
    });
}

void SkillStore::notify_skills_changed()
{
    for (auto& observer : m_observers)
        observer.skills_changed();
}

}
