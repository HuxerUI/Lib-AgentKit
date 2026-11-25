#include <app_resources.h>

#include "release_skill.h"
#include "release_skill_data.h"

#include <huxerui/huxerui.h>

namespace agentkit_preview {
namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;
namespace data = release_skill_data;

kit::SkillRef ReleaseRef() { return {PreviewSkills, ReleasePresentationSkill, data::revision}; }

class PackagedSkills final : public kit::SkillSource {
public:
  kit::Async<std::vector<kit::SkillSummary>> ListAsync(kit::SkillSourceRequest) override {
    co_return kit::AgentResult<std::vector<kit::SkillSummary>>::Success(
        {{ReleaseRef(), std::string(data::name), std::string(data::description)}});
  }

  kit::Async<kit::SkillPackage> LoadAsync(kit::SkillSourceRequest, kit::SkillRef skill) override {
    using Result = kit::AgentResult<kit::SkillPackage>;
    if (skill != ReleaseRef()) co_return Result::Failure({kit::ErrorCode::NotFound, "The packaged Skill version is unavailable"});
    co_return Result::Success({std::move(skill), std::string(data::instructions),
        {{std::string(ReleaseTemplatePath), "application/json", data::outline.size()}}});
  }

  kit::Async<AsyncInputStream> OpenResourceAsync(kit::SkillSourceRequest, kit::SkillRef skill, std::string path) override {
    using Result = kit::AgentResult<AsyncInputStream>;
    if (skill != ReleaseRef() || path != ReleaseTemplatePath)
      co_return Result::Failure({kit::ErrorCode::NotFound, "The packaged Skill resource is unavailable"});
    auto bytes = std::as_bytes(std::span(data::outline.data(), data::outline.size()));
    auto asset = RawAsset::CopyBytes(bytes, "application/json");
    co_return Result::Success(co_await asset.OpenReadAsync());
  }
};

} // namespace

std::shared_ptr<kit::SkillSource> MakePreviewSkills() { return std::make_shared<PackagedSkills>(); }

[[huxerui::composable]]
View SkillPicker(std::shared_ptr<kit::AgentRuntime> runtime, kit::SkillCatalogQuery query,
                 State<std::vector<kit::SkillRef>> selection) {
  const auto menu = UseMenu();
  const auto anchor = UseViewHandle();
  const auto tasks = UseTaskScope();
  const auto error = UseState(std::string{});
  const auto selected = selection.Get();
  const auto choose = [runtime, query, selection, menu, anchor, tasks, error] {
    tasks.Launch([runtime, query, selection, menu, anchor, error]() -> Task<void> {
      auto available = co_await runtime->ListSkillsAsync(query);
      if (!available.Succeeded()) { error = available.Error().message; co_return; }
      error = "";
      std::vector<MenuEntry> items;
      for (auto& skill : available.Value()) {
        auto label = skill.ref.source == PreviewSkills && skill.ref.id == ReleasePresentationSkill ?
            StringVariant{::app::strings::ui_release_presentation} :
            StringVariant{std::move(skill.name)};
        items.emplace_back(MenuItem(std::move(label), [selection, ref = skill.ref] {
          selection = std::vector{ref};
        }).Checked(selection.Get() == std::vector{skill.ref}));
      }
      if (!selection->empty()) items.emplace_back(MenuItem(::app::strings::ui_remove_skill, [selection] {
        selection = std::vector<kit::SkillRef>{};
      }));
      if (items.empty()) { error = "No Skills are available in this conversation"; co_return; }
      menu.Show(anchor, std::move(items));
    });
  };
  const auto label = selected.empty() ? StringVariant{::app::strings::ui_skills} :
      selected.front().source == PreviewSkills && selected.front().id == ReleasePresentationSkill ?
      StringVariant{::app::strings::ui_skill_release_presentation} :
      StringVariant::Format(::app::strings::ui_skill_name, selected.front().id.value);
  auto button = Button(label).OnClick(choose).With(anchor).Key("composer-skills");
  if (error->empty()) return button;
  return Column{std::move(button), Text(error.Get(), TextRole::Label)}.With(Spacing(4.0F));
}

} // namespace agentkit_preview
