#pragma once

#include <huxerui/agentkit/skill.h>
#include <huxerui/file.h>

namespace agentkit_preview {

inline const huxerui::agentkit::SkillSourceId ImportedSkills{"ganwork.local-skills"};

struct ImportedSkill {
  huxerui::agentkit::SkillSummary summary;
  bool enabled;
};

class ImportedSkillSource final : public huxerui::agentkit::SkillSource {
public:
  explicit ImportedSkillSource(huxerui::File registry);

  huxerui::agentkit::Async<void> Restore();
  huxerui::agentkit::Async<void> Import(huxerui::File directory);
  huxerui::agentkit::Async<void> SetEnabled(huxerui::agentkit::SkillId id, bool enabled);
  std::vector<ImportedSkill> Entries() const;
  bool HasEnabled() const;

  huxerui::agentkit::Async<std::vector<huxerui::agentkit::SkillSummary>> ListAsync(huxerui::agentkit::SkillSourceRequest) override;
  huxerui::agentkit::Async<huxerui::agentkit::SkillPackage> LoadAsync(huxerui::agentkit::SkillSourceRequest,
      huxerui::agentkit::SkillRef skill) override;
  huxerui::agentkit::Async<huxerui::AsyncInputStream> OpenResourceAsync(huxerui::agentkit::SkillSourceRequest,
      huxerui::agentkit::SkillRef skill, std::string path) override;

private:
  struct Version {
    std::string description;
    std::string instructions;
    std::map<std::string, std::string, std::less<>> resources;
    bool operator==(const Version&) const = default;
  };
  struct Entry {
    bool enabled = true;
    std::vector<Version> versions;
  };
  using Catalog = std::map<huxerui::agentkit::SkillId, Entry>;

  huxerui::agentkit::Async<void> Commit(Catalog next);
  const Version* Find(const huxerui::agentkit::SkillRef& skill) const;
  huxerui::File registry_;
  Catalog catalog_;
  bool busy_ = false;
};

} // namespace agentkit_preview
