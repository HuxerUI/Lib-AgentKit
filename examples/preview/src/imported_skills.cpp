#include "imported_skills.h"

#include <huxerui/huxerui.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace agentkit_preview {
namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;
namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr std::size_t package_limit = 1 << 20;
constexpr std::size_t registry_limit = 16 << 20;

template<class T>
kit::AgentResult<T> Failure(kit::ErrorCode code, std::string message) {
  return kit::AgentResult<T>::Failure({.code = code, .message = std::move(message)});
}

std::string Utf8Path(const fs::path& path) {
  const auto utf8 = path.generic_u8string();
  return {utf8.begin(), utf8.end()};
}

bool SkillName(std::string_view name) {
  return !name.empty() && name.size() <= 64 && !name.starts_with('-') && !name.ends_with('-') &&
      name.find("--") == name.npos && std::ranges::all_of(name, [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-';
      });
}

bool Linked(const fs::path& path) {
  if (fs::is_symlink(fs::symlink_status(path))) return true;
#ifdef _WIN32
  const auto attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
  return false;
#endif
}

std::string ReadText(const fs::path& path, std::size_t limit) {
  if (Linked(path) || !fs::is_regular_file(path) || fs::file_size(path) > limit)
    throw std::invalid_argument("Skill files must be regular files within the size limit");
  std::ifstream file(path, std::ios::binary);
  if (!file) throw std::runtime_error("A Skill file could not be opened");
  std::string result(std::min<std::size_t>(limit + 1, static_cast<std::size_t>(fs::file_size(path)) + 1), '\0');
  file.read(result.data(), static_cast<std::streamsize>(result.size()));
  if (file.bad() || !file.eof()) throw std::runtime_error("A Skill file changed while being imported");
  result.resize(static_cast<std::size_t>(file.gcount()));
  if (result.size() > limit || result.find('\0') != std::string::npos)
    throw std::invalid_argument("A Skill resource must contain bounded UTF-8 text");
  return result;
}

std::string_view Trim(std::string_view value) {
  const auto begin = value.find_first_not_of(" \t\r");
  if (begin == value.npos) return {};
  return value.substr(begin, value.find_last_not_of(" \t\r") - begin + 1);
}

std::string Scalar(std::string_view value) {
  value = Trim(value);
  if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
    return Json::parse(value).get<std::string>();
  if (value.size() >= 2 && value.front() == '\'' && value.back() == '\'') {
    value.remove_prefix(1);
    value.remove_suffix(1);
    std::string text;
    for (std::size_t i = 0; i < value.size(); ++i) {
      if (value[i] == '\'' && i + 1 < value.size() && value[i + 1] == '\'') ++i;
      text += value[i];
    }
    return text;
  }
  return std::string(Trim(value.substr(0, value.find(" #"))));
}

Json ReadPackage(std::string directory) {
  auto root = fs::u8path(directory).lexically_normal();
  if (root.filename().empty() && root.has_parent_path()) root = root.parent_path();
  if (Linked(root) || !fs::is_directory(root)) throw std::invalid_argument("Choose a local Skill folder");
  auto document = ReadText(root / "SKILL.md", 128 << 10);
  auto input = std::string_view(document);
  if (input.starts_with("\xef\xbb\xbf")) input.remove_prefix(3);
  const auto line = [&]() {
    const auto end = input.find('\n');
    const auto next = input.substr(0, end);
    input = end == input.npos ? std::string_view{} : input.substr(end + 1);
    return next;
  };
  if (Trim(line()) != "---") throw std::invalid_argument("SKILL.md must begin with YAML frontmatter");
  std::string name;
  std::string description;
  bool reading_description = false;
  bool closed = false;
  while (!input.empty()) {
    const auto item = line();
    const auto trimmed = Trim(item);
    if (trimmed == "---") { closed = true; break; }
    if (item.starts_with("name:")) name = Scalar(item.substr(5));
    if (item.starts_with("description:")) {
      const auto value = Trim(item.substr(12));
      reading_description = value == ">" || value == ">-" || value == ">+" ||
          value == "|" || value == "|-" || value == "|+";
      description = reading_description ? std::string{} : Scalar(value);
      continue;
    }
    if (!item.empty() && !item.starts_with(' ') && !item.starts_with('\t')) reading_description = false;
    if (reading_description && !trimmed.empty()) {
      if (!description.empty()) description += ' ';
      description += trimmed;
    }
  }
  if (!closed || !SkillName(name) || Utf8Path(root.filename()) != name || description.empty() ||
      description.size() > 1024 || input.find_first_not_of(" \t\r\n") == input.npos)
    throw std::invalid_argument("SKILL.md needs a name matching the folder, a description, and instructions");
  Json resources = Json::object();
  auto remaining = package_limit - document.size();
  for (const auto* folder : {"references", "assets"}) {
    const auto path = root / folder;
    if (Linked(path)) throw std::invalid_argument("A Skill folder may not contain linked resource directories");
    if (!fs::is_directory(path)) continue;
    for (fs::recursive_directory_iterator it(path), end; it != end; ++it) {
      if (Linked(it->path())) throw std::invalid_argument("A Skill resource may not be a linked file");
      if (!it->is_regular_file()) continue;
      const auto ext = it->path().extension();
      if (ext != ".md" && ext != ".txt" && ext != ".json" && ext != ".csv" && ext != ".yaml" && ext != ".yml") continue;
      auto key = Utf8Path(it->path().lexically_relative(root));
      if (resources.size() >= 128 || key.size() > 1024) throw std::invalid_argument("The Skill resource manifest is too large");
      auto text = ReadText(it->path(), remaining);
      remaining -= text.size();
      resources.emplace(std::move(key), std::move(text));
    }
  }
  Json result{{"name", name}, {"description", description}, {"instructions", input}, {"resources", std::move(resources)}};
  static_cast<void>(result.dump());
  return result;
}

} // namespace

ImportedSkillSource::ImportedSkillSource(File registry) : registry_(std::move(registry)) {}

kit::Async<void> ImportedSkillSource::Restore() {
  if (!registry_.Exists()) co_return kit::AgentResult<void>::Success();
  auto stat = co_await registry_.StatAsync();
  if (!stat.Succeeded() || stat.Value().size > registry_limit)
    co_return Failure<void>(kit::ErrorCode::PersistenceFailure, "The imported Skill registry is unavailable or too large");
  auto raw = co_await registry_.ReadStringAsync();
  if (!raw.Succeeded()) co_return Failure<void>(kit::ErrorCode::PersistenceFailure, "Could not read imported Skills");
  try {
    auto saved = Json::parse(raw.Value());
    Catalog next;
    if (!saved.is_object() || saved.size() > 64) throw std::invalid_argument("Invalid Skill registry");
    for (const auto& [id, record] : saved.items()) {
      Entry entry;
      entry.enabled = record.at("enabled").get<bool>();
      const auto& versions = record.at("versions");
      if (!SkillName(id) || !versions.is_array() || versions.empty()) throw std::invalid_argument("Invalid Skill entry");
      for (const auto& version : versions) {
        entry.versions.push_back({version.at("description").get<std::string>(),
            version.at("instructions").get<std::string>(), version.at("resources").get<decltype(Version::resources)>()});
      }
      next.emplace(kit::SkillId{id}, std::move(entry));
    }
    catalog_ = std::move(next);
  } catch (const std::exception&) {
    co_return Failure<void>(kit::ErrorCode::PersistenceFailure, "The imported Skill registry is invalid");
  }
  co_return kit::AgentResult<void>::Success();
}

kit::Async<void> ImportedSkillSource::Commit(Catalog next) {
  auto saved = Json::object();
  for (const auto& [id, entry] : next) {
    auto versions = Json::array();
    for (const auto& version : entry.versions) versions.push_back({{"description", version.description},
        {"instructions", version.instructions}, {"resources", version.resources}});
    saved[id.value] = {{"enabled", entry.enabled}, {"versions", std::move(versions)}};
  }
  auto data = saved.dump();
  if (data.size() > registry_limit || next.size() > 64)
    co_return Failure<void>(kit::ErrorCode::BudgetExceeded, "The imported Skill registry is full");
  auto pending = registry_.Parent()->Child(kit::NewCommandId().value + ".skills-pending");
  if (!co_await pending.WriteStringAsync(std::move(data)) || !co_await pending.MoveToAsync(registry_, true)) {
    static_cast<void>(co_await pending.DeleteAsync());
    co_return Failure<void>(kit::ErrorCode::PersistenceFailure, "Could not save the imported Skills");
  }
  catalog_ = std::move(next);
  co_return kit::AgentResult<void>::Success();
}

kit::Async<void> ImportedSkillSource::Import(File directory) {
  if (busy_) co_return Failure<void>(kit::ErrorCode::Busy, "A Skill update is in progress");
  busy_ = true;
  const auto reset = std::unique_ptr<bool, void(*)(bool*)>{&busy_, [](bool* busy) { *busy = false; }};
  try {
    const auto package = co_await RunWorker(ReadPackage, directory.Path());
    auto next = catalog_;
    auto& entry = next[kit::SkillId{package.at("name").get<std::string>()}];
    Version version{package.at("description").get<std::string>(), package.at("instructions").get<std::string>(),
        package.at("resources").get<decltype(Version::resources)>()};
    if (!entry.versions.empty() && entry.versions.back() == version) co_return kit::AgentResult<void>::Success();
    entry.versions.push_back(std::move(version));
    co_return co_await Commit(std::move(next));
  } catch (const std::exception& error) {
    co_return Failure<void>(kit::ErrorCode::InvalidArgument, error.what());
  }
}

kit::Async<void> ImportedSkillSource::SetEnabled(kit::SkillId id, bool enabled) {
  if (busy_) co_return Failure<void>(kit::ErrorCode::Busy, "A Skill update is in progress");
  auto next = catalog_;
  const auto found = next.find(id);
  if (found == next.end()) co_return Failure<void>(kit::ErrorCode::NotFound, "The imported Skill was not found");
  busy_ = true;
  const auto reset = std::unique_ptr<bool, void(*)(bool*)>{&busy_, [](bool* busy) { *busy = false; }};
  found->second.enabled = enabled;
  co_return co_await Commit(std::move(next));
}

std::vector<ImportedSkill> ImportedSkillSource::Entries() const {
  std::vector<ImportedSkill> result;
  for (const auto& [id, entry] : catalog_) {
    result.push_back({{{ImportedSkills, id, static_cast<kit::Revision>(entry.versions.size())}, id.value,
        entry.versions.back().description}, entry.enabled});
  }
  return result;
}

bool ImportedSkillSource::HasEnabled() const {
  return std::ranges::any_of(catalog_, [](const auto& entry) { return entry.second.enabled; });
}

const ImportedSkillSource::Version* ImportedSkillSource::Find(const kit::SkillRef& skill) const {
  if (skill.source != ImportedSkills || !skill.version) return nullptr;
  const auto found = catalog_.find(skill.id);
  if (found == catalog_.end() || !found->second.enabled || skill.version > found->second.versions.size()) return nullptr;
  return &found->second.versions[skill.version - 1];
}

kit::Async<std::vector<kit::SkillSummary>> ImportedSkillSource::ListAsync(kit::SkillSourceRequest request) {
  if (request.stop.stop_requested()) co_return Failure<std::vector<kit::SkillSummary>>(kit::ErrorCode::AlreadyClosed, "Skill discovery cancelled");
  std::vector<kit::SkillSummary> result;
  for (auto& entry : Entries()) if (entry.enabled) result.push_back(std::move(entry.summary));
  co_return kit::AgentResult<std::vector<kit::SkillSummary>>::Success(std::move(result));
}

kit::Async<kit::SkillPackage> ImportedSkillSource::LoadAsync(kit::SkillSourceRequest request, kit::SkillRef skill) {
  const auto* version = Find(skill);
  if (request.stop.stop_requested() || !version)
    co_return Failure<kit::SkillPackage>(kit::ErrorCode::NotFound, "The requested Skill version is disabled or unavailable");
  kit::SkillPackage package{std::move(skill), version->instructions};
  for (const auto& [path, text] : version->resources) package.resources.push_back({path, "text/plain", text.size()});
  co_return kit::AgentResult<kit::SkillPackage>::Success(std::move(package));
}

kit::Async<AsyncInputStream> ImportedSkillSource::OpenResourceAsync(kit::SkillSourceRequest request, kit::SkillRef skill, std::string path) {
  const auto* version = Find(skill);
  if (!request.stop.stop_requested() && version) {
    const auto resource = version->resources.find(path);
    if (resource != version->resources.end()) {
      auto asset = RawAsset::CopyBytes(std::as_bytes(std::span(resource->second.data(), resource->second.size())), "text/plain");
      co_return kit::AgentResult<AsyncInputStream>::Success(co_await asset.OpenReadAsync());
    }
  }
  co_return Failure<AsyncInputStream>(kit::ErrorCode::NotFound, "The requested Skill resource is unavailable");
}

} // namespace agentkit_preview
