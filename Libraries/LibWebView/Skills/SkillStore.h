/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibURL/URL.h>
#include <LibWebView/Skills/SkillTypes.h>

namespace WebView {

class WEBVIEW_API SkillStoreObserver {
public:
    virtual ~SkillStoreObserver() = default;

    virtual void skills_changed() { }
};

class WEBVIEW_API SkillStore final : public Weakable<SkillStore> {
    AK_MAKE_NONCOPYABLE(SkillStore);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit SkillStore(ByteString storage_directory);
    ~SkillStore();

    void reload();
    void ensure_builtin_skills();

    Vector<SkillDefinition> list_skills(bool include_disabled = false) const;
    Vector<SkillDefinition> matching_skills_for_url(URL::URL const& url) const;
    Optional<SkillDefinition> get_skill(StringView name, bool include_disabled = true) const;
    Optional<SkillDefinition> get_skill_by_uri(StringView uri, bool include_disabled = false) const;

    ErrorOr<Vector<SkillSupportingFile>> read_supporting_files(StringView name) const;
    ErrorOr<SkillResourceContent> read_skill_resource(StringView uri, bool include_disabled = false) const;
    ErrorOr<Vector<SkillDirectoryEntry>> read_skill_directory(StringView uri, bool include_disabled = false) const;

    ErrorOr<SkillDefinition> save_skill(SkillDefinition skill, Optional<Vector<SkillSupportingFile>> supporting_files = {});
    ErrorOr<SkillDefinition> set_skill_enabled(StringView name, bool enabled);
    ErrorOr<void> delete_skill(StringView name);

    ByteString const& storage_directory() const { return m_storage_directory; }

    void add_observer(SkillStoreObserver&);
    void remove_observer(SkillStoreObserver&);

private:
    ByteString skill_directory_path(StringView name) const;
    Optional<SkillDefinition> load_skill_from_directory(StringView name);
    void notify_skills_changed();

    ByteString m_storage_directory;
    HashMap<String, SkillDefinition> m_skills;
    Vector<SkillStoreObserver&> m_observers;
};

}
