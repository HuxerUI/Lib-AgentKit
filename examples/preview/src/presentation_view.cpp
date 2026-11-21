#include <app_resources.h>

#include "presentation_view.h"

#include "general_workflow.h"
#include "presentation_tools.h"
#include "presentation_workflow.h"
#include "preview_theme.h"
#include "workspace_evidence.h"

#include <huxerui/huxerui.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <sstream>

#ifdef __EMSCRIPTEN__
#include "browser_workspace.h"
#include "code_workspace.h"
#include <emscripten.h>
#endif

namespace agentkit_preview {
namespace {
using namespace huxerui;
namespace kit = huxerui::agentkit;
using kit::JsonValue;

#ifdef __EMSCRIPTEN__
EM_JS_DEPS(agentkit_download_helpers, "$UTF8ToString");
EM_JS(bool, DownloadPowerPoint, (const std::byte* data, std::size_t size, const char* name), {
  if (navigator.userActivation?.isActive === false) return false;
  try {
    const bytes = HEAPU8.slice(data, data + size);
    const type = 'application/vnd.openxmlformats-officedocument.presentationml.presentation';
    const url = URL.createObjectURL(new Blob([bytes], {type}));
    const link = document.createElement('a');
    link.href = url;
    link.download = UTF8ToString(name);
    document.body.appendChild(link);
    try {
      link.click();
    } finally {
      link.remove();
      setTimeout(() => URL.revokeObjectURL(url), 60000);
    }
    return true;
  } catch {
    return false;
  }
});

[[huxerui::composable]]
View BrowserExportCard(File file, kit::ArtifactRef artifact, std::weak_ptr<kit::AgentRuntime> runtime) {
  const auto tasks = UseTaskScope();
  const auto error = UseState(std::string{});
  const auto downloading = UseState(false);
  constexpr auto max_size = 32 << 20;
  std::vector<View> body{Row{Text(file.Name()), Button(::app::strings::ui_download_powerpoint).OnClick([tasks, file, artifact, runtime, error, downloading] {
    if (downloading.Get()) return;
    downloading = true;
    error = "";
    tasks.Launch([file, artifact, runtime, error, downloading]() -> Task<void> {
      const auto owner = runtime.lock();
      if (owner) {
        auto allowed = co_await owner->Artifacts().ReadAsync(artifact);
        if (allowed.Succeeded() && allowed.Value().metadata.type == PresentationArtifactType) {
          const auto info = file.Stat();
          if (info.Succeeded() && info.Value().size > 0 && info.Value().size <= max_size) {
            auto bytes = file.ReadBytes();
            if (bytes.Succeeded() && bytes.Value().size() == info.Value().size) {
              if (!DownloadPowerPoint(bytes.Value().data(), bytes.Value().size(), file.Name().c_str()))
                error = "Click Download PowerPoint again to start the browser download.";
              downloading = false;
              co_return;
            }
          }
        }
      }
      error = "The approved PowerPoint export is unavailable.";
      downloading = false;
    });
  }).With(Enabled(!downloading.Get()))}.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center))};
  if (!error->empty()) body.push_back(Text(error.Get(), TextRole::Label));
  return Column(std::move(body)).With(Spacing(6.0F));
}
#endif

struct EditableSlide {
  TextEditingValue title;
  TextEditingValue bullets;
  bool operator==(const EditableSlide&) const = default;
};

struct FormDraft {
  std::map<std::string, TextEditingValue> fields;
  std::vector<EditableSlide> slides;
  std::size_t page = 0;
  std::string error;
  bool submitting = false;
  bool operator==(const FormDraft&) const = default;
};

using Drafts = std::map<kit::InteractionId, FormDraft>;

[[huxerui::composable]]
View ScreenshotPreview(std::shared_ptr<kit::AgentRuntime> runtime, kit::ArtifactRef ref) {
  const auto tasks = UseTaskScope();
  const auto image = UseState(std::shared_ptr<const ImageAsset>{});
  const auto error = UseState(std::string{});
  Lifecycle([tasks, runtime, ref, image, error] {
    auto load = tasks.Launch([runtime, ref, image, error]() -> Task<void> {
      auto opened = co_await LoadVerificationScreenshot(runtime, ref);
      if (opened.Succeeded()) image = std::make_shared<const ImageAsset>(std::move(opened).Value());
      else error = opened.Error().message;
    });
    return [load] { load.Cancel(); };
  }, runtime, ref);
  if (!error->empty()) return Text(error.Get());
  if (const auto asset = image.Get()) {
    return Image(*asset).Fit(ImageFit::Contain).With(Frame{.height = 220.0F},
        Semantics{.label = ::app::strings::ui_implementation_screenshot});
  }
  return Text(::app::strings::ui_loading_implementation_screenshot, TextRole::Label);
}

std::optional<File> PictureReviewDirectory(const ApplicationHandle& application) {
  try {
    return application.Directories().temporary_directory;
  } catch (const std::logic_error&) {
#ifdef AGENTKIT_PREVIEW_TEST
    std::error_code error;
    const auto directory = std::filesystem::temp_directory_path(error);
    if (!error) return File(directory.u8string());
#endif
    return {};
  }
}

[[huxerui::composable]]
View SlideScreenshot(std::shared_ptr<kit::AgentRuntime> runtime, kit::ArtifactRef artifact, std::size_t page,
    std::function<void()> select) {
  const auto tasks = UseTaskScope();
  const auto temporary = PictureReviewDirectory(UseApplication());
  const auto screenshot = UseState(std::shared_ptr<const PresentationScreenshot>{});
  const auto error = UseState(std::string{});
  Lifecycle([tasks, runtime, artifact, page, temporary, screenshot, error] {
    auto load = tasks.Launch([runtime, artifact, page, temporary, screenshot, error]() -> Task<void> {
      if (!temporary) {
        error = "The temporary directory for picture review is unavailable";
        co_return;
      }
      auto opened = co_await LoadPresentationScreenshot(runtime, artifact, page, *temporary);
      if (opened.Succeeded()) screenshot = std::make_shared<const PresentationScreenshot>(std::move(opened).Value());
      else error = opened.Error().message;
    });
    return [load] { load.Cancel(); };
  }, runtime, artifact, page);
  if (!error->empty()) return Text(error.Get());
  if (const auto picture = screenshot.Get()) {
    std::vector<View> body{Image(picture->image).Fit(ImageFit::Contain).With(Frame{.height = 220.0F},
        Semantics{.label = std::string(PresentationScreenshotLabel)})};
    if (picture->selectable) body.push_back(Button(::app::strings::ui_select_implementation_screenshot).OnClick(std::move(select)));
    return Column(std::move(body)).With(Spacing(6.0F));
  }
  return Text(::app::strings::ui_loading_implementation_screenshot, TextRole::Label);
}

const std::string& ReadString(const JsonValue::Object& object, const char* key) {
  return std::get<std::string>(object.at(key).value);
}

FormDraft InitialDraft(const PresentationOutline& outline) {
  FormDraft draft;
  draft.fields.emplace("title", TextEditingValue::FromText(outline.title));
  for (const auto& slide : outline.slides) {
    std::string bullets;
    for (const auto& item : slide.bullets) {
      if (!bullets.empty()) bullets += '\n';
      bullets += item;
    }
    draft.slides.push_back({TextEditingValue::FromText(slide.title), TextEditingValue::FromText(std::move(bullets))});
  }
  return draft;
}

PresentationOutline EditedOutline(const FormDraft& draft) {
  PresentationOutline outline{.title = draft.fields.at("title").text};
  for (const auto& editor : draft.slides) {
    SlideOutline slide{.title = editor.title.text};
    std::istringstream lines(editor.bullets.text);
    for (std::string line; std::getline(lines, line);) {
      if (!line.empty()) slide.bullets.push_back(std::move(line));
    }
    outline.slides.push_back(std::move(slide));
  }
  return outline;
}

ButtonStyle PresentationNavigationStyle(ButtonStyle action, Color ink) {
  action.background = Color::Transparent();
  action.disabled_background = Color::Transparent();
  action.label_style = {Font::System(12.0F).WithWeight(FontWeight::Medium), ink};
  action.padding = EdgeInsets::Symmetric(6.0F, 4.0F);
  action.minimum_height = 28.0F;
  return action;
}

View SlideStepper(std::size_t page, std::size_t count, const std::function<void(std::size_t)>& select, const ButtonStyle& action) {
  return ProvideEnvironment(action, Row{
      Button(::app::strings::ui_previous_slide).OnClick([select, page] { if (page > 0) select(page - 1); }).With(Enabled(page > 0)),
      Text(std::to_string(page + 1) + " / " + std::to_string(count), TextRole::Label),
      Button(::app::strings::ui_next_slide).OnClick([select, page, count] { if (page + 1 < count) select(page + 1); })
          .With(Enabled(page + 1 < count)),
  }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)));
}

class OutlineColumns final : public Layout<OutlineColumns> {
public:
  using Layout::Layout;

  static LayoutResult Measure(LayoutContext& context, ViewNode& node, Constraints constraints) {
    const float width = constraints.ConstrainWidth(constraints.HasBoundedWidth() ? constraints.max_width : 620.0F);
    const bool split = width >= 500.0F;
    constexpr float gap = 12.0F;
    const float index_width = split ? std::min(198.0F, width * 0.31F) : width;
    auto& index = node.ChildAt(0);
    auto& editor = node.ChildAt(1);
    const auto index_size = context.Measure(index, {
        .min_width = index_width, .max_width = index_width, .max_height = split ? 198.0F : 95.0F});
    const float editor_width = split ? width - index_width - gap : width;
    const auto editor_size = context.Measure(editor, {.min_width = editor_width, .max_width = editor_width});
    const Point editor_origin = split ? Point{index_width + gap, 0.0F} : Point{0.0F, index_size.height + gap};
    return LayoutResult{}.Place(index, {}).Place(editor, editor_origin)
        .SetSize(constraints.Constrain({width, std::max(index_size.height, editor_origin.y + editor_size.height)}));
  }
};

View OutlineSlideIndex(const FormDraft& draft, std::function<void(std::size_t)> select, const ColorScheme& colors,
                       const std::string& untitled_slide) {
  std::vector<View> rows;
  for (std::size_t index = 0; index < draft.slides.size(); ++index) {
    const auto& title = draft.slides[index].title.text;
    const auto& label = title.empty() ? untitled_slide : title;
    const auto number = std::to_string(index + 1);
    const bool selected = index == draft.page;
    const auto open = [select, index] { select(index); };
    rows.push_back(Row{
        Text(number, TextRole::Label).Style({Font::System(11.0F), colors.on_surface_variant}).With(Frame{.width = 19.0F}),
        Text(label).Style({Font::System(12.0F).WithWeight(selected ? FontWeight::SemiBold : FontWeight::Regular),
            selected ? colors.on_primary_container : colors.on_surface}).LineLimit(2).Overflow(TextOverflow::Ellipsis).With(Grow()),
    }.OnClick(open).On<ViewEvents::KeyDown>([open](const KeyEvent& event) {
      if (event.key != Key::Enter && event.key != Key::Space) return false;
      if (!event.repeat) open();
      return true;
    }).With(Focusable{}, Semantics{.role = SemanticRole::Button,
        .label = StringVariant::Format(::app::strings::ui_slide_number_and_title, index + 1, label), .selected = selected},
        Frame{.min_height = 34.0F}, Padding(EdgeInsets::Symmetric(7.0F, 4.0F)), Spacing(5.0F),
        CrossAlign(CrossAxisAlignment::Center), CornerRadius(6.0F),
        Background(selected ? colors.primary_container : Color::Transparent())).Key("outline-slide/" + number));
  }
  return ScrollView(Column(std::move(rows)).With(Spacing(2.0F), Padding(4.0F), CrossAlign(CrossAxisAlignment::Stretch)))
      .With(ScrollBar(), CornerRadius(8.0F), Border{colors.outline}, Background(colors.surface)).Key("outline-slide-index");
}

View PresentationSlide(std::string deck, std::vector<View> content) {
  content.insert(content.begin(), Text(std::move(deck))
      .Style({Font::System(11.0F).WithWeight(FontWeight::SemiBold), Color::Rgb(22, 111, 98)}).LineLimit(2).Overflow(TextOverflow::Ellipsis));
  return Column(std::move(content)).With(Frame{.min_height = 176.0F}, Padding(20.0F), Spacing(9.0F),
      CrossAlign(CrossAxisAlignment::Stretch), CornerRadius(8.0F), Background(Color::Rgb(247, 248, 244)),
      Border{Color::Rgb(215, 223, 218)}).Key("presentation-slide");
}

View PreviewSlideControls(const PresentationOutline& outline, std::size_t page,
                          const std::function<void(std::size_t)>& select, const ButtonStyle& navigation) {
  if (outline.slides.size() == 1) return SlideStepper(page, 1, select, navigation);
  std::vector<StringVariant> options;
  for (std::size_t index = 0; index < outline.slides.size(); ++index)
    options.push_back(StringVariant::Format(::app::strings::ui_slide_number_and_title, index + 1, outline.slides[index].title));
  return Flow{
      Select(std::move(options), page, [](const StringVariant& label) {
        return Text(label).LineLimit(1).Overflow(TextOverflow::Ellipsis);
      }).Label(::app::strings::ui_slide_title).OnChanged(select).With(Frame{.width = 265.0F}).Key("presentation-slide-picker"),
      SlideStepper(page, outline.slides.size(), select, navigation),
  }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center));
}

kit::PartId SlidePart(std::size_t index) { return {"slide-" + std::to_string(index + 1)}; }

bool HasLaterVersion(const kit::SessionSnapshot& session, const kit::ArtifactRef& deck) {
  return std::ranges::any_of(session.artifacts, [&](const auto& ref) {
    return ref.id == deck.id && ref.version > deck.version;
  });
}

struct AffectedPresentation {
  std::string title;
  bool approximate;
  std::vector<std::pair<PresentationLocation, std::string>> destinations;
};

kit::ArtifactImpactQuery ReportImpact(kit::ProjectId project, kit::ArtifactRef report) {
  return {std::move(project), {{{std::move(report)}, {}, kit::SourceChangeKind::Modified}}};
}

kit::Async<std::vector<AffectedPresentation>> ListAffectedPresentations(
    std::shared_ptr<kit::AgentRuntime> runtime, kit::ArtifactImpactQuery query) {
  using Result = kit::AgentResult<std::vector<AffectedPresentation>>;
  auto impact = co_await runtime->Artifacts().InspectImpactAsync(query);
  if (!impact.Succeeded()) co_return Result::Failure(impact.Error());
  std::vector<AffectedPresentation> decks;
  for (const auto& affected : impact.Value().affected) {
    auto deck = co_await runtime->Artifacts().ReadAsync(affected.affected);
    if (!deck.Succeeded() || deck.Value().metadata.type != "presentation/pptx") continue;
    auto owner = co_await runtime->Sessions().OpenAsync(deck.Value().session);
    if (!owner.Succeeded()) continue;
    const auto current = owner.Value()->Snapshot();
    if (!current || current->summary.project != query.project || HasLaterVersion(*current, affected.affected)) continue;
    AffectedPresentation entry{deck.Value().metadata.name + " v" + std::to_string(affected.affected.version),
        affected.approximate};
    if (affected.parts.empty()) {
      entry.destinations.emplace_back(PresentationLocation{deck.Value().session, affected.affected}, "Review presentation");
    } else {
      for (const auto& part : deck.Value().parts) {
        if (std::ranges::find(affected.parts, part.id) == affected.parts.end()) continue;
        const auto page = PresentationPartPage(part.id);
        if (!page) continue;
        entry.destinations.emplace_back(PresentationLocation{deck.Value().session, affected.affected, part.id},
            "Review slide " + std::to_string(*page) + " / " + part.label);
      }
    }
    if (!entry.destinations.empty()) decks.push_back(std::move(entry));
  }
  co_return Result::Success(std::move(decks));
}

[[huxerui::composable]]
View VerificationReportSummary(kit::ArtifactPreview report, std::shared_ptr<kit::Conversation> conversation,
    std::shared_ptr<kit::AgentRuntime> runtime, OpenPresentationLocation open) {
  const auto tasks = UseTaskScope();
  const auto results = UseState(std::shared_ptr<const std::vector<AffectedPresentation>>{});
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  std::vector<View> body{Text(::app::strings::ui_verification_report, TextRole::Label), Text(report.summary)};
  const auto current = conversation->Snapshot();
  if (!open || !current || !current->summary.project) return Column(std::move(body)).With(Spacing(6.0F));
  const auto query = ReportImpact(*current->summary.project, report.artifact);
  body.push_back(Button(results.Get() ? ::app::strings::ui_refresh_affected_slides : ::app::strings::ui_find_affected_slides)
      .OnClick([tasks, runtime, query, results, busy, error] {
        if (busy.Get()) return;
        busy = true;
        error = "";
        tasks.Launch([runtime, query, results, busy, error]() -> Task<void> {
          auto impact = co_await ListAffectedPresentations(runtime, query);
          busy = false;
          if (impact.Succeeded())
            results = std::make_shared<const std::vector<AffectedPresentation>>(std::move(impact).Value());
          else {
            results = std::shared_ptr<const std::vector<AffectedPresentation>>{};
            error = impact.Error().message;
          }
        });
      }).With(Enabled(!busy.Get())));
  if (const auto found = results.Get()) {
    std::vector<View> rows;
    for (const auto& entry : *found) {
      rows.push_back(Text(entry.title, TextRole::Label));
      if (entry.approximate) rows.push_back(Text(::app::strings::ui_affected_scope_needs_review, TextRole::Label));
      for (const auto& [location, title] : entry.destinations) {
        rows.push_back(Button(title).OnClick([tasks, query, location, runtime, open, results, busy, error] {
          if (busy.Get()) return;
          busy = true;
          tasks.Launch([query, location, runtime, open, results, busy, error]() -> Task<void> {
            auto current = co_await runtime->Artifacts().InspectImpactAsync(query);
            busy = false;
            auto link = current.Succeeded() ? std::ranges::find(current.Value().affected, location.deck, &kit::ImpactItem::affected)
                                              : std::vector<kit::ImpactItem>::iterator{};
            if (!current.Succeeded() || link == current.Value().affected.end() ||
                (location.part && std::ranges::find(link->parts, *location.part) == link->parts.end()) ||
                (!location.part && !link->parts.empty())) {
              results = std::shared_ptr<const std::vector<AffectedPresentation>>{};
              error = current.Succeeded() ? "This presentation is no longer an available impact." : current.Error().message;
              co_return;
            }
            error = "";
            open(location);
          });
        }).With(Enabled(!busy.Get())).Key(location.deck.id.value + "/" +
            std::to_string(location.deck.version) + "/" + (location.part ? location.part->value : "deck")));
      }
    }
    if (rows.empty()) body.push_back(Text(::app::strings::ui_no_current_presentations_use_this_report));
    else body.push_back(ScrollView(Column(std::move(rows)).With(Spacing(6.0F)))
        .With(Frame{.max_height = 145.0F}, ScrollBar()));
  }
  if (!error->empty()) body.push_back(Text(error.Get(), TextRole::Label));
  return Column(std::move(body)).With(Spacing(6.0F));
}

struct ReportVersion {
  kit::SourceRef ref;
  std::string name;
  std::string conversation;
  std::string conclusion;

  kit::Revision Version() const {
    if (const auto* artifact = std::get_if<kit::ArtifactRef>(&ref)) return artifact->version;
    return std::get<kit::SharedContextRef>(ref).version;
  }
};

struct ReworkChoices {
  std::vector<ReportVersion> previous;
  std::vector<ReportVersion> replacement;
};

Task<std::optional<ReportVersion>> ReadReportVersion(std::shared_ptr<kit::AgentRuntime> runtime,
    kit::SourceRef ref, std::string conversation) {
  if (const auto* shared = std::get_if<kit::SharedContextRef>(&ref)) {
    auto item = co_await runtime->Projects().ReadContextAsync(*shared);
    if (!item.Succeeded()) co_return std::nullopt;
    auto report = ReadImportedReport(item.Value());
    if (!report) co_return std::nullopt;
    co_return ReportVersion{std::move(ref), std::move(report->name), "Imported report", std::move(report->summary)};
  }
  const auto* artifact = std::get_if<kit::ArtifactRef>(&ref);
  if (!artifact) co_return std::nullopt;
  auto record = co_await runtime->Artifacts().ReadAsync(*artifact);
  if (!record.Succeeded() || record.Value().metadata.type != VerificationEvidence) co_return std::nullopt;
  auto preview = co_await runtime->Artifacts().PreviewAsync(*artifact);
  if (!preview.Succeeded()) co_return std::nullopt;
  co_return ReportVersion{ref, record.Value().metadata.name, std::move(conversation), std::move(preview).Value().summary};
}

kit::Async<ReworkChoices> ReadReworkChoices(std::shared_ptr<kit::AgentRuntime> runtime, kit::SessionSummary session,
    std::vector<kit::ArtifactRef> local, kit::ArtifactRef deck, kit::PartId page) {
  using Result = kit::AgentResult<ReworkChoices>;
  auto record = co_await runtime->Artifacts().ReadAsync(deck);
  if (!record.Succeeded()) co_return Result::Failure(record.Error());
  if (record.Value().session != session.id) co_return Result::Failure({.code = kit::ErrorCode::PermissionDenied,
      .message = "Open the conversation that owns this presentation to rework it."});

  std::vector<kit::SourceRef> inputs;
  for (const auto& edge : record.Value().dependencies) {
    if (edge.output_part == page && edge.input.parts.empty() && !edge.input.text_range &&
        std::ranges::find(inputs, edge.input.source) == inputs.end()) inputs.push_back(edge.input.source);
  }
  ReworkChoices choices;
  for (const auto& input : inputs) {
    auto report = co_await ReadReportVersion(runtime, input, {});
    if (report) choices.previous.push_back(std::move(*report));
  }
  if (choices.previous.empty()) co_return Result::Success(std::move(choices));

  std::vector<std::pair<kit::SourceRef, std::string>> candidates;
  for (auto& ref : local) candidates.emplace_back(std::move(ref), session.title);
  if (session.project) {
    auto project = co_await runtime->Projects().ReadAsync(*session.project);
    if (!project.Succeeded()) co_return Result::Failure(project.Error());
    for (const auto& report : project.Value().shared_context) candidates.emplace_back(report, std::string{});
    std::optional<kit::PageCursor> cursor;
    do {
      auto list = co_await runtime->Sessions().ListAsync({.project = session.project}, cursor);
      if (!list.Succeeded()) co_return Result::Failure(list.Error());
      for (const auto& member : list.Value().sessions) {
        if (member.id == session.id) continue;
        auto opened = co_await runtime->Sessions().OpenAsync(member.id);
        if (!opened.Succeeded()) continue;
        if (auto current = opened.Value()->Snapshot()) {
          for (const auto& ref : current->artifacts) candidates.emplace_back(ref, member.title);
        }
      }
      cursor = list.Value().next;
    } while (cursor);
  }
  for (auto& [ref, title] : candidates) {
    if (std::ranges::find(choices.replacement, ref, &ReportVersion::ref) != choices.replacement.end()) continue;
    auto report = co_await ReadReportVersion(runtime, ref, std::move(title));
    if (report) choices.replacement.push_back(std::move(*report));
  }
  co_return Result::Success(std::move(choices));
}

View ReportChoice(const ReportVersion& report, bool selected, std::function<void()> choose) {
  const auto* artifact = std::get_if<kit::ArtifactRef>(&report.ref);
  const auto revision = report.Version();
  auto name = report.name + " v" + std::to_string(revision);
  if (!report.conversation.empty()) name += "  /  " + report.conversation;
  return Column{
      Button(StringVariant::Format(selected ? ::app::strings::ui_selected_0 : ::app::strings::ui_use_0, name)).OnClick(std::move(choose)),
      Text(report.conclusion),
  }.With(Spacing(4.0F)).Key((artifact ? artifact->id.value : std::get<kit::SharedContextRef>(report.ref).item.value) +
      "/" + std::to_string(revision));
}

std::optional<View> WorkspaceReportPicker([[maybe_unused]] const std::shared_ptr<CodeWorkspaces>& workspaces,
    [[maybe_unused]] const kit::SessionSummary& session, [[maybe_unused]] std::shared_ptr<kit::AgentRuntime> runtime,
    [[maybe_unused]] std::function<void(ImportedPresentationReport)> imported) {
#ifdef __EMSCRIPTEN__
  if (workspaces && session.workspace && session.project) {
    auto folder = workspaces->Find(*session.workspace);
    if (folder && !folder->browser_name.empty()) {
      return BrowserWorkspaceReportControl(std::move(runtime), *session.project, workspaces->BrowserStore(),
          folder->id, folder->browser_name, std::move(imported));
    }
  }
#endif
  return {};
}

[[huxerui::composable]]
View ReportReworkDialog(DialogContext dialog, std::shared_ptr<kit::AgentRuntime> runtime,
                       std::shared_ptr<kit::Conversation> conversation, kit::ArtifactRef deck, kit::PartId page,
                       std::size_t slide, std::shared_ptr<CodeWorkspaces> workspaces) {
  const auto tasks = UseTaskScope();
  const auto choices = UseState(std::shared_ptr<const ReworkChoices>{});
  const auto original = UseState(std::size_t{0});
  const auto replacement = UseState(std::optional<kit::SourceRef>{});
  const auto imports = UseState(std::uint64_t{0});
  const auto request = UseState(std::optional<kit::CommandId>{});
  const auto busy = UseState(false);
  const auto accepted = UseState(false);
  const auto error = UseState(std::string{});
  Lifecycle([tasks, runtime, conversation, deck, page, choices, error] {
    auto loading = tasks.Launch([runtime, conversation, deck, page, choices, error]() -> Task<void> {
      auto current = conversation->Snapshot();
      if (!current) { error = "The conversation is unavailable."; co_return; }
      auto result = co_await ReadReworkChoices(runtime, current->summary, current->artifacts, deck, page);
      if (!result.Succeeded()) { error = result.Error().message; co_return; }
      choices = std::make_shared<const ReworkChoices>(std::move(result).Value());
    });
    return [loading] { loading.Cancel(); };
  }, conversation, deck, page, imports.Get());

  std::vector<View> body{Text(::app::strings::ui_replace_verification_report, TextRole::Title),
      Text(StringVariant::Format(::app::strings::ui_slide_0_presentation_v_1, slide, deck.version), TextRole::Label)};
  const auto available = choices.Get();
  if (accepted.Get()) {
    body.push_back(Text(::app::strings::ui_return_to_the_conversation_to_review_the_bfe8e7));
  } else if (available && !available->previous.empty()) {
    const auto& before = available->previous[original.Get()];
    body.push_back(Text(::app::strings::ui_current_source, TextRole::Label));
    if (available->previous.size() == 1) {
    body.push_back(Text(before.name + " v" + std::to_string(before.Version())));
      body.push_back(Text(before.conclusion));
    } else {
      for (std::size_t i = 0; i < available->previous.size(); ++i) {
        body.push_back(ReportChoice(available->previous[i], i == original.Get(), [original, replacement, busy, request, i] {
          if (busy.Get()) return;
          original = i;
          replacement = std::nullopt;
          request = std::nullopt;
        }));
      }
    }
    std::vector<View> reports;
    for (const auto& report : available->replacement) {
      if (report.ref == before.ref) continue;
      reports.push_back(ReportChoice(report, replacement.Get() == report.ref, [replacement, busy, request, ref = report.ref] {
        if (busy.Get() || replacement.Get() == ref) return;
        replacement = ref;
        request = std::nullopt;
      }));
    }
    if (auto current = conversation->Snapshot(); current && current->summary.project && !busy.Get()) {
      auto use_report = [imports, replacement, request](ImportedPresentationReport report) {
        replacement = kit::SourceRef{std::move(report.ref)};
        request = std::nullopt;
        imports = imports.Get() + 1;
      };
      if (auto folder = WorkspaceReportPicker(workspaces, current->summary, runtime, use_report))
        body.push_back(std::move(*folder));
      body.push_back(ImportReportControl(runtime, *current->summary.project, std::move(use_report)));
    }
    if (reports.empty()) body.push_back(Text(::app::strings::ui_no_other_report_is_available_in_this));
    else {
      body.push_back(Text(::app::strings::ui_replacement_source, TextRole::Label));
      body.push_back(ScrollView(Column(std::move(reports)).With(Spacing(10.0F)))
          .With(Frame{.max_height = 200.0F}, ScrollBar()));
    }
  } else if (available) {
    body.push_back(Text(::app::strings::ui_this_slide_has_no_available_verification_report));
  } else if (error->empty()) {
    body.push_back(Text(::app::strings::ui_loading_verification_reports, TextRole::Label));
  }
  if (!error->empty()) body.push_back(Text(error.Get(), TextRole::Label));

  std::vector<View> actions{Spacer(), Button(accepted.Get() ? ::app::strings::ui_done : ::app::strings::ui_cancel).OnClick([dialog] { dialog.Dismiss(); })};
  if (replacement.Get() && !accepted.Get()) {
    actions.push_back(Button(::app::strings::ui_prepare_slide_change).OnClick([runtime, tasks, conversation, deck, page, choices, original,
        replacement, request, busy, accepted, error] {
      if (busy.Get() || !replacement.Get()) return;
      const auto current = conversation->Snapshot();
      if (!current) { error = "The conversation is unavailable."; return; }
      if (!request.Get()) request = kit::NewCommandId();
      kit::ReworkRequest plan{.session = current->summary.id, .baseline = deck, .parts = {page},
          .replacements = {{{choices.Get()->previous[original.Get()].ref}, {*replacement.Get()}}}};
      busy = true;
      error = "";
      tasks.Launch([runtime, plan = std::move(plan), command = *request.Get(), busy, accepted, error]() mutable -> Task<void> {
        auto result = co_await runtime->Artifacts().PrepareReworkAsync(command, std::move(plan));
        busy = false;
        if (result.Succeeded()) accepted = true;
        else error = result.Error().message;
      });
    }).With(Enabled(!busy.Get())));
  }
  body.push_back(Row(std::move(actions)).With(Spacing(8.0F)));
  return Column(std::move(body)).With(Frame{.width = 520.0F}, Padding(20.0F), Spacing(10.0F));
}

[[huxerui::composable]]
View StagedExportDialog(DialogContext dialog, std::shared_ptr<kit::Conversation> conversation, kit::ArtifactRef artifact) {
  const auto tasks = UseTaskScope();
  const auto name = UseState(TextEditingValue::FromText(UseString(StringVariant::Format(::app::strings::ui_presentation_v_0_pptx, artifact.version))));
  const auto command = UseState(std::optional<kit::CommandId>{});
  const auto busy = UseState(false);
  const auto accepted = UseState(false);
  const auto error = UseState(std::string{});
  const auto submit = [tasks, name, command, busy, accepted, error, conversation, artifact] {
    if (busy.Get() || accepted.Get()) return;
    auto valid = ValidatePresentationFilename(name->text);
    if (!valid.Succeeded()) { error = valid.Error().message; return; }
    if (!command.Get()) command = kit::NewCommandId();
    busy = true;
    error = "";
    tasks.Launch([conversation, artifact, id = *command.Get(), file_name = name->text,
                  busy, accepted, error]() -> Task<void> {
      kit::ArtifactAction action{artifact, std::string(ExportStagedPresentation), JsonValue{file_name}};
      auto result = co_await conversation->DispatchArtifactActionAsync(id, std::move(action));
      busy = false;
      if (result.Succeeded()) accepted = true;
      else error = result.Error().message;
    });
  };
  const auto colors = UseTheme().colors;
  const auto field_style = PreviewFormFieldStyle(UseEnvironment<TextFieldStyle>(), colors);
  auto navigation = PresentationNavigationStyle(UseEnvironment<ButtonStyle>(), colors.on_surface_variant);
  navigation.minimum_height = 34.0F;
  std::vector<View> body{Row{Text(::app::strings::ui_export_powerpoint, TextRole::Title).With(Grow()),
      Text(StringVariant::Format(::app::strings::ui_version_0, artifact.version), TextRole::Label)}
      .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center))};
  if (accepted.Get()) {
    body.push_back(Text(::app::strings::ui_return_to_the_conversation_to_review_the_9860c0));
  } else {
    body.push_back(ProvideEnvironment(field_style, TextField(name).Label(::app::strings::ui_file_name).Variant(TextFieldVariant::Outlined)
        .OnChanged([name, command, busy, error](const TextEditingValue& value) {
          if (busy.Get()) return;
          if (name->text != value.text) command = std::nullopt;
          name = value;
          error = "";
        }).OnSubmitted(submit).With(Enabled(!busy.Get())).Key("ppt-review-export-name")));
  }
  if (!error->empty()) body.push_back(Text(error.Get(), TextRole::Label).Style({Font::System(12.0F), colors.error}));
  auto dismiss = Button(accepted.Get() ? ::app::strings::ui_done : ::app::strings::ui_cancel).OnClick([dialog] { dialog.Dismiss(); }).With(Enabled(!busy.Get()));
  std::vector<View> actions{Spacer(), accepted.Get() ? std::move(dismiss) : ProvideEnvironment(navigation, std::move(dismiss))};
  if (!accepted.Get())
    actions.push_back(Button(::app::strings::ui_review_export).OnClick(submit).With(Enabled(!busy.Get() && !name->text.empty())));
  body.push_back(Row(std::move(actions)).With(Spacing(8.0F)));
  return Column(std::move(body)).With(Frame{.width = 440.0F}, Padding(18.0F), Spacing(12.0F),
      CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View StagedPresentationReview(PresentationOutline outline, std::optional<std::size_t> screenshot_page, kit::ArtifactRef artifact,
                              std::shared_ptr<kit::Conversation> conversation, std::shared_ptr<kit::AgentRuntime> runtime,
                              State<kit::AgentChatDraftSources> draft,
                              State<PresentationReviewPages> pages, std::shared_ptr<CodeWorkspaces> workspaces) {
  const auto selection = UseState(std::optional<kit::SourceSlice>{});
  const auto dialogs = UseDialog();
  const auto version = std::pair{artifact.id, artifact.version};
  const auto& saved = pages.Get();
  const auto page = saved.find(version);
  std::size_t index = 0;
  if (page != saved.end()) {
    const auto number = PresentationPartPage(page->second);
    if (!number || *number > outline.slides.size()) return Text(::app::strings::ui_this_slide_could_not_be_found_in);
    index = *number - 1;
  }
  const auto part = SlidePart(index);
  const auto& slide = outline.slides[index];
  const auto select_field = [selection, artifact, part](std::size_t offset, std::size_t size) {
    selection = kit::SourceSlice{artifact, {}, kit::SourceTextRange{part, offset, offset + size}};
  };
  const auto colors = UseTheme().colors;
  const auto navigation = PresentationNavigationStyle(UseEnvironment<ButtonStyle>(), colors.on_surface_variant);
  auto slide_title = UseEnvironment<ButtonStyle>();
  slide_title.background = Color::Transparent();
  slide_title.label_style = {Font::System(23.0F).WithWeight(FontWeight::SemiBold), Color::Rgb(25, 40, 48)};
  slide_title.padding = {};
  slide_title.minimum_height = 32.0F;
  auto slide_bullet = slide_title;
  slide_bullet.label_style = {Font::System(15.0F), Color::Rgb(42, 56, 65)};
  slide_bullet.minimum_height = 25.0F;
  std::vector<View> slide_body{
      ProvideEnvironment(slide_title, Row{
          Button(slide.title).OnClick([select_field, size = slide.title.size()] { select_field(0, size); }).Key("slide-title"),
          Spacer()}.With(CrossAlign(CrossAxisAlignment::Center))),
  };
  auto offset = slide.title.size() + 1;
  for (std::size_t i = 0; i < slide.bullets.size(); ++i) {
    const auto& bullet = slide.bullets[i];
    slide_body.push_back(ProvideEnvironment(slide_bullet, Row{
        Text("\xe2\x80\xa2").Style(slide_bullet.label_style),
        Button(bullet).OnClick([select_field, offset, size = bullet.size()] { select_field(offset, size); })
            .Key("slide-bullet-" + std::to_string(i)),
        Spacer(),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center))));
    offset += bullet.size() + 1;
  }
  if (screenshot_page == index + 1) {
    const auto picture = PresentationScreenshotPart(index + 1);
    slide_body.push_back(SlideScreenshot(runtime, artifact, index + 1, [selection, artifact, picture] {
      selection = kit::SourceSlice{artifact, {picture}};
    }).Key(picture.value));
  }
  const auto current = conversation->Snapshot();
  const bool owns_deck = current && std::ranges::find(current->artifacts, artifact) != current->artifacts.end();
  std::vector<View> actions{
      ProvideEnvironment(navigation, Button(::app::strings::ui_select_slide)
          .OnClick([selection, artifact, part] { selection = kit::SourceSlice{artifact, {part}}; })),
  };
  if (owns_deck) {
    actions.push_back(ProvideEnvironment(navigation,
        Button(::app::strings::ui_replace_report_on_slide).OnClick([dialogs, runtime, conversation, artifact, part, index, workspaces] {
      ShowPreviewDialog(dialogs, DialogFactory{[runtime, conversation, artifact, part, index, workspaces](DialogContext dialog) {
        return ReportReworkDialog(dialog, runtime, conversation, artifact, part, index + 1, workspaces);
      }});
    })));
  }
  if (owns_deck && (current->summary.agent.id == PptAgent || current->summary.agent.id == GeneralAgent)) {
    actions.push_back(ProvideEnvironment(PreviewSecondaryButton(UseEnvironment<ButtonStyle>(), colors),
        Button(::app::strings::ui_export_powerpoint).OnClick([dialogs, conversation, artifact] {
      ShowPreviewDialog(dialogs, DialogFactory{[conversation, artifact](DialogContext dialog) {
        return StagedExportDialog(dialog, conversation, artifact);
      }});
    })));
  }
  std::vector<View> content{
      PreviewSlideControls(outline, index, [pages, version, selection](std::size_t value) {
        selection = std::nullopt;
        auto updated = pages.Get();
        updated.insert_or_assign(version, SlidePart(value));
        pages = std::move(updated);
      }, navigation),
      PresentationSlide(outline.title, std::move(slide_body)),
      Divider(),
      Flow(std::move(actions)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
  };
  if (const auto selected = selection.Get()) {
    const auto key = selected->text_range ? part.value + "/" + std::to_string(selected->text_range->begin_utf8)
                                          : selected->parts.front().value;
    content.push_back(kit::AgentChatSourceActions(conversation, *selected, draft).Key(key));
  }
  return ScrollView(Column(std::move(content)).With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)))
      .With(Grow(), ScrollBar());
}

std::optional<View> RenderForm(const kit::InteractionSnapshot& interaction,
                               std::shared_ptr<kit::Conversation> conversation, State<Drafts> drafts,
                               TaskScope tasks, const ButtonStyle& navigation, std::shared_ptr<kit::AgentRuntime> runtime,
                               const ColorScheme& colors, const TextFieldStyle& form_fields, const ButtonStyle& buttons,
                               const std::string& untitled_slide) {
  const auto& form = std::get<kit::FormRequest>(interaction.body);
  const auto* data = std::get_if<JsonValue::Object>(&form.presentation.value);
  if (!data || !data->contains("kind")) return {};
  const auto& kind = ReadString(*data, "kind");
  const bool brief = kind == PresentationBrief;
  const bool edit = kind == PresentationOutlineReview;
  const bool preview = kind == PresentationExport;
  if (!brief && !edit && !preview) return {};
  const auto fixed = data->find("slide_count");
  const auto* slide_count = fixed != data->end() ? std::get_if<std::int64_t>(&fixed->second.value) : nullptr;

  FormDraft draft;
  PresentationOutline outline;
  if (!brief) {
    auto read = ReadPresentationOutline(data->at("outline"));
    if (!read.Succeeded()) return Text(read.Error().message);
    outline = std::move(read).Value();
  }
  if (auto saved = drafts->find(interaction.id); saved != drafts->end()) {
    draft = saved->second;
  } else if (edit) {
    draft = InitialDraft(outline);
  } else if (preview) {
    draft.fields.emplace("file_name", TextEditingValue::FromText("Presentation.pptx"));
  }

  auto save = [drafts, id = interaction.id](FormDraft value) {
    auto copy = drafts.Get();
    copy.insert_or_assign(id, std::move(value));
    drafts = std::move(copy);
  };
  auto page = [save, draft](std::size_t value) mutable { draft.page = value; save(draft); };
  auto text_field = [save, draft](std::string key, StringVariant label, bool multiline = false) {
    const auto found = draft.fields.find(key);
    const auto view_key = "ppt-" + key;
    return TextField(found == draft.fields.end() ? TextEditingValue::FromText({}) : found->second).Label(label)
        .Variant(TextFieldVariant::Outlined)
        .LineLimits(multiline ? TextFieldLineLimits::MultiLine(2, 3) : TextFieldLineLimits::SingleLine())
        .OnChanged([save, draft, key = std::move(key)](const TextEditingValue& value) mutable {
          draft.fields[key] = value;
          draft.error.clear();
          save(draft);
        }).Key(view_key);
  };
  auto submit = [tasks, conversation, drafts, save, draft, id = interaction.id, version = interaction.version](
                    JsonValue answer) mutable {
    if (auto current = drafts->find(id); current != drafts->end() && current->second.submitting) return;
    draft.submitting = true;
    save(draft);
    tasks.Launch([conversation, drafts, id, version, data = std::move(answer)]() -> Task<void> {
      auto reply = co_await conversation->SubmitAsync(kit::NewCommandId(), {id, version, std::move(data)});
      auto current = drafts.Get();
      auto element = current.find(id);
      if (element == current.end()) co_return;
      if (reply.Succeeded()) current.erase(element);
      else {
        element->second.submitting = false;
        element->second.error = reply.Error().message;
      }
      drafts = std::move(current);
    });
  };

  std::vector<View> fields;
  std::vector<View> outline_actions;
  StringVariant action;
  JsonValue answer;
  if (edit) {
    fields.push_back(text_field("title", ::app::strings::ui_presentation_title));
    std::vector<View> editor{SlideStepper(draft.page, draft.slides.size(), page, navigation)};
    if (data->contains("includes_screenshot") && draft.page + 1 == draft.slides.size()) {
      editor.push_back(Text(::app::strings::ui_implementation_screenshot_will_appear_on_this_slide, TextRole::Label));
    }
    const auto& slide = draft.slides[draft.page];
    editor.push_back(TextField(slide.title).Label(::app::strings::ui_slide_title).Variant(TextFieldVariant::Outlined)
        .OnChanged([save, draft](const TextEditingValue& value) mutable {
          draft.slides[draft.page].title = value;
          draft.error.clear();
          save(draft);
        }).Key("slide-title-" + std::to_string(draft.page)));
    editor.push_back(TextField(slide.bullets).Label(::app::strings::ui_bullets_one_per_line).Variant(TextFieldVariant::Outlined)
        .LineLimits(TextFieldLineLimits::MultiLine(3, 6))
        .OnChanged([save, draft](const TextEditingValue& value) mutable {
          draft.slides[draft.page].bullets = value;
          draft.error.clear();
          save(draft);
        }).Key("slide-bullets-" + std::to_string(draft.page)));
    if (data->contains("report_summary")) {
      const auto& summary = ReadString(*data, "report_summary");
      auto bullets = EditedOutline(draft).slides[draft.page].bullets;
      if (bullets.size() < 6 && std::ranges::find(bullets, summary) == bullets.end()) {
        editor.push_back(ProvideEnvironment(navigation,
            Button(StringVariant::Format(::app::strings::ui_insert_summary_from_0, ReadString(*data, "report_name"))).OnClick([save, draft, summary]() mutable {
          auto text = draft.slides[draft.page].bullets.text;
          if (!text.empty() && !text.ends_with('\n')) text += '\n';
          text += summary;
          draft.slides[draft.page].bullets = TextEditingValue::FromText(std::move(text));
          save(draft);
        }).Key("ppt-insert-report-summary")));
      }
    }
    fields.push_back(OutlineColumns{
        OutlineSlideIndex(draft, page, colors, untitled_slide),
        Column(std::move(editor)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch)).Key("outline-slide-editor"),
    });
    if (!slide_count) {
      outline_actions.push_back(ProvideEnvironment(PreviewSecondaryButton(buttons, colors),
          Button(::app::strings::ui_add_slide).OnClick([save, draft, untitled_slide]() mutable {
          if (draft.slides.size() >= 30) return;
          draft.page = draft.slides.size();
          draft.slides.push_back({TextEditingValue::FromText(untitled_slide), TextEditingValue::FromText({})});
          save(draft);
        }).With(Enabled(draft.slides.size() < 30))));
      outline_actions.push_back(ProvideEnvironment(navigation,
          Button(::app::strings::ui_remove_slide).OnClick([save, draft]() mutable {
          if (draft.slides.size() <= 1) return;
          draft.slides.erase(draft.slides.begin() + static_cast<std::ptrdiff_t>(draft.page));
          draft.page = std::min(draft.page, draft.slides.size() - 1);
          save(draft);
        }).With(Enabled(draft.slides.size() > 1))));
    }
    action = ::app::strings::ui_confirm_outline;
    answer = WritePresentationOutline(EditedOutline(draft));
  } else if (preview) {
    const auto& slide = outline.slides[draft.page];
    std::vector<View> slide_content{Text(slide.title, TextRole::Title)
        .Style({Font::System(23.0F).WithWeight(FontWeight::SemiBold), Color::Rgb(25, 40, 48)})};
    for (const auto& bullet : slide.bullets)
      slide_content.push_back(Row{
          Text("\xe2\x80\xa2").Style({Font::System(14.0F), Color::Rgb(22, 111, 98)}),
          Text(bullet).Style({Font::System(14.0F), Color::Rgb(42, 56, 65)}).With(Grow()),
      }.With(Spacing(7.0F)));
    if (draft.page + 1 == outline.slides.size() && data->contains("screenshot_id") && data->contains("screenshot_version")) {
      kit::ArtifactRef ref{{ReadString(*data, "screenshot_id")},
          static_cast<kit::Revision>(std::get<std::int64_t>(data->at("screenshot_version").value))};
      slide_content.push_back(ScreenshotPreview(std::move(runtime), ref).Key(ref.id.value));
    }
    fields.push_back(PresentationSlide(outline.title, std::move(slide_content)));
    action = ::app::strings::ui_review_export;
  } else {
    fields.push_back(Text(::app::strings::ui_presentation_brief, TextRole::Title));
    fields.push_back(text_field("audience", ::app::strings::ui_audience));
    fields.push_back(text_field("style", ::app::strings::ui_style));
    if (slide_count) fields.push_back(Text(StringVariant::Format(::app::strings::ui_goal_requires_0_slides, *slide_count), TextRole::Label));
    else fields.push_back(text_field("length", ::app::strings::ui_target_slide_count));
    fields.push_back(text_field("reference_notes", ::app::strings::ui_reference_notes, true));
    action = ::app::strings::ui_draft_outline;
  }
  auto submit_button = Button(action).OnClick(
      [edit, brief, submit, save, draft, answer = std::move(answer)]() mutable {
        if (edit) {
          auto valid = ReadPresentationOutline(answer);
          if (!valid.Succeeded()) { draft.error = valid.Error().message; save(draft); return; }
        } else {
          JsonValue::Object fields;
          auto add = [&](const char* key) {
            auto found = draft.fields.find(key);
            fields.emplace(key, JsonValue{found == draft.fields.end() ? std::string{} : found->second.text});
          };
          if (brief) {
            add("audience");
            add("style");
            add("length");
            add("reference_notes");
          } else {
            auto valid = ValidatePresentationFilename(draft.fields.at("file_name").text);
            if (!valid.Succeeded()) { draft.error = valid.Error().message; save(draft); return; }
            add("file_name");
          }
          answer = JsonValue{std::move(fields)};
        }
        submit(answer);
      }).With(Enabled(!draft.submitting));
  auto primary_action = ProvideEnvironment(PreviewPrimaryButton(buttons, colors), std::move(submit_button));
  if (brief) {
    if (!draft.error.empty()) fields.push_back(Text(draft.error, TextRole::Label));
    auto content = Column(std::move(fields)).With(Padding(12.0F), Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch));
    return ProvideEnvironment(form_fields, Column{
        ScrollView(std::move(content)).With(Frame{.max_height = 264.0F}, ScrollBar()).Key("ppt-form-scroll"),
        Row{Spacer(), std::move(primary_action)}.With(Padding(EdgeInsets{.right = 12.0F, .bottom = 8.0F, .left = 12.0F})),
    }.With(CrossAlign(CrossAxisAlignment::Stretch), Border{colors.outline}, CornerRadius(8.0F),
           Background(colors.surface_container_high)));
  }
  const auto heading = edit ? ::app::strings::ui_edit_presentation_outline : ::app::strings::ui_presentation_preview;
  std::vector<View> header{Text(heading, TextRole::Title)
      .Style({Font::System(17.0F).WithWeight(FontWeight::SemiBold), colors.on_surface})};
  if (preview) header.push_back(PreviewSlideControls(outline, draft.page, page, navigation));
  auto scroll = ScrollView(Column(std::move(fields)).With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Stretch)))
      .With(Frame{.max_height = preview ? 308.0F : 318.0F}, ScrollBar()).Key("ppt-form-scroll");
  std::vector<View> footer;
  if (preview) {
    const auto folder = ReadString(*data, "directory");
    footer.push_back(Row{
        Text(::app::strings::ui_export_folder, TextRole::Label).Style({Font::System(12.0F), colors.on_surface_variant}),
        Text(folder, TextRole::Label).Style({Font::System(12.0F), colors.on_surface_variant})
            .LineLimit(1).Overflow(TextOverflow::Ellipsis).With(Grow(), Tooltip(folder)),
    }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)).Key("presentation-export-folder"));
    footer.push_back(Row{text_field("file_name", ::app::strings::ui_file_name).With(Grow()), std::move(primary_action)}
        .With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Center)));
  } else {
    footer.push_back(Row{Flow(std::move(outline_actions)).With(Grow(), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)),
        std::move(primary_action)}.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)));
  }
  if (!draft.error.empty()) footer.push_back(Text(draft.error, TextRole::Label).Style({Font::System(12.0F), colors.error}));
  return ProvideEnvironment(form_fields, Column{
      Column(std::move(header)).With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch), LayoutPriority(2.0F)),
      std::move(scroll),
      Column{Divider(), Column(std::move(footer)).With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch))}
          .With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Stretch), LayoutPriority(3.0F)).Key("presentation-form-actions"),
  }.With(Padding(12.0F), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch),
         ClipChildren{}, CornerRadius(10.0F), Border{colors.outline}, Background(colors.surface_container_low)));
}
} // namespace

kit::Async<std::shared_ptr<kit::Conversation>> OpenPresentationReviewAsync(
    std::shared_ptr<kit::AgentRuntime> runtime, PresentationLocation target) {
  using Result = kit::AgentResult<std::shared_ptr<kit::Conversation>>;
  auto opened = co_await runtime->Sessions().OpenAsync(target.session);
  if (!opened.Succeeded()) co_return opened;
  auto deck = co_await runtime->Artifacts().ReadAsync(target.deck);
  if (!deck.Succeeded()) co_return Result::Failure(deck.Error());
  const auto view = opened.Value()->Snapshot();
  if (!view || deck.Value().session != target.session || deck.Value().metadata.type != "presentation/pptx" ||
      std::ranges::find(view->artifacts, target.deck) == view->artifacts.end() ||
      (target.part && std::ranges::find(deck.Value().parts, *target.part, &kit::ArtifactPart::id) == deck.Value().parts.end())) {
    co_return Result::Failure({.code = kit::ErrorCode::NotFound, .message = "The selected presentation or slide is unavailable"});
  }
  if (HasLaterVersion(*view, target.deck))
    co_return Result::Failure({.code = kit::ErrorCode::Conflict, .message = "The presentation was revised. Refresh the affected slides."});
  co_return opened;
}

namespace {

struct SharedReportVersion {
  kit::SharedContextItem item;
  ImportedPresentationReport report;
};

std::string_view ReportVersionState(kit::SharedContextState state) {
  switch (state) {
  case kit::SharedContextState::Draft: return "Draft";
  case kit::SharedContextState::PendingReview: return "In review";
  case kit::SharedContextState::Published: return "Published";
  case kit::SharedContextState::Withdrawn: return "Withdrawn";
  }
  return {};
}

} // namespace

[[huxerui::composable]]
View SharedReportsDialog(DialogContext dialog, std::shared_ptr<kit::AgentRuntime> runtime, kit::ProjectId project) {
  const auto tasks = UseTaskScope();
  const auto versions = UseState(std::shared_ptr<const std::vector<SharedReportVersion>>{});
  const auto selected = UseState(std::optional<kit::SharedContextRef>{});
  const auto revision = UseState(std::optional<TextEditingValue>{});
  const auto withdrawing = UseState(false);
  const auto reload = UseState(0U);
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  Lifecycle([tasks, runtime, project, versions, error] {
    auto loading = tasks.Launch([runtime, project, versions, error]() -> Task<void> {
      auto projects = runtime->Projects();
      auto snapshot = co_await projects.ReadAsync(project);
      if (!snapshot.Succeeded()) { error = snapshot.Error().message; co_return; }
      std::vector<SharedReportVersion> reports;
      for (const auto& ref : snapshot.Value().shared_context) {
        auto item = co_await projects.ReadContextAsync(ref);
        if (!item.Succeeded()) continue;
        if (auto report = ReadImportedReportVersion(item.Value()))
          reports.push_back({std::move(item).Value(), std::move(*report)});
      }
      versions = std::make_shared<const std::vector<SharedReportVersion>>(std::move(reports));
    });
    return [loading] { loading.Cancel(); };
  }, project, reload.Get());

  const auto change = [tasks, runtime, project, selected, revision, withdrawing, versions, reload, busy, error](
                          kit::SharedContextRef ref, kit::ContextChange action) {
    if (busy.Get()) return;
    busy = true;
    error = "";
    const bool withdraw = std::holds_alternative<kit::WithdrawContext>(action);
    tasks.Launch([runtime, project, request = kit::ChangeSharedContext{std::move(ref), std::move(action)},
                     selected, revision, withdrawing, versions, reload, busy, error, withdraw]() mutable -> Task<void> {
      auto receipt = co_await runtime->Projects().UpdateContextAsync(project, kit::NewCommandId(), std::move(request));
      if (!receipt.Succeeded()) {
        error = receipt.Error().message;
      } else if (const auto* version = std::get_if<kit::SharedContextRef>(&receipt.Value().outcome)) {
        selected = withdraw ? std::nullopt : std::optional{*version};
        revision = std::nullopt;
        withdrawing = false;
      } else {
        error = "Review the pending request before continuing.";
      }
      versions = std::shared_ptr<const std::vector<SharedReportVersion>>{};
      reload = reload.Get() + 1;
      busy = false;
    });
  };

  std::vector<View> body{Text(::app::strings::ui_shared_reports, TextRole::Title)};
  auto available = versions.Get();
  const SharedReportVersion* current = nullptr;
  if (available && !available->empty()) {
    std::vector<View> rows;
    for (const auto& entry : *available) {
      const auto& [ref, name, summary] = entry.report;
      if (selected.Get() == ref) current = &entry;
      auto version = std::to_string(ref.version);
      rows.push_back(Button(name + " v" + version + " / " + std::string(ReportVersionState(entry.item.state)))
          .OnClick([selected, revision, withdrawing, error, ref] {
            selected = ref;
            revision = std::nullopt;
            withdrawing = false;
            error = "";
          }).With(Enabled(!busy.Get())).Key("shared-report/" + ref.item.value + "/" + version));
    }
    body.push_back(ScrollView(Column(std::move(rows)).With(Spacing(4.0F), CrossAlign(CrossAxisAlignment::Stretch)))
        .With(Frame{.max_height = 150.0F}, ScrollBar()));
  } else if (available) {
    body.push_back(Text(::app::strings::ui_no_shared_reports_in_this_project));
  } else if (error->empty()) {
    body.push_back(Text(::app::strings::ui_loading_reports, TextRole::Label));
  }

  if (current) {
    const auto& [item, report] = *current;
    const bool latest = std::ranges::none_of(*available, [&](const auto& entry) {
      return entry.item.ref.item == report.ref.item && entry.item.ref.version > report.ref.version;
    });
    if (withdrawing.Get()) {
      body.push_back(Text(StringVariant::Format(::app::strings::ui_withdraw_0_v_1, report.name, report.ref.version)));
      body.push_back(Button(::app::strings::ui_confirm_withdrawal).OnClick([change, ref = report.ref] {
        change(ref, kit::WithdrawContext{});
      }).With(Enabled(!busy.Get())));
    } else if (const auto edited = revision.Get()) {
      body.push_back(TextField(*edited).Label(::app::strings::ui_report_summary).LineLimits(TextFieldLineLimits::MultiLine(2, 5))
          .OnChanged([revision](const TextEditingValue& value) { revision = value; }).Key("shared-report-summary"));
      body.push_back(Button(::app::strings::ui_save_draft).OnClick([change, revision, error, item] {
        if (!revision.Get()) return;
        const auto& summary = revision.Get()->text;
        auto valid = ValidatePresentationReportSummary(summary);
        if (!valid.Succeeded()) { error = valid.Error().message; return; }
        auto draft = item.data;
        std::get<JsonValue::Object>(draft.content.value).insert_or_assign("summary", JsonValue{summary});
        change(item.ref, kit::EditSharedContext{std::move(draft)});
      }).With(Enabled(latest && !busy.Get() && edited->text != report.summary)));
    } else {
      body.push_back(Text(report.summary));
      std::vector<View> actions;
      if (latest) {
        if (item.state == kit::SharedContextState::Draft)
          actions.push_back(Button(::app::strings::ui_submit_for_review).OnClick([change, ref = report.ref] { change(ref, kit::RequestContextReview{}); }));
        if (item.state == kit::SharedContextState::PendingReview)
          actions.push_back(Button(::app::strings::ui_publish_version).OnClick([change, ref = report.ref] { change(ref, kit::PublishContext{}); }));
        actions.push_back(Button(::app::strings::ui_new_version).OnClick([revision, summary = report.summary] {
          revision = TextEditingValue::FromText(summary);
        }));
      }
      actions.push_back(Button(::app::strings::ui_withdraw_version).OnClick([withdrawing] { withdrawing = true; }));
      body.push_back(Column(std::move(actions)).With(Spacing(6.0F), Enabled(!busy.Get())));
    }
  }
  if (!error->empty()) body.push_back(Text(error.Get(), TextRole::Label));
  body.push_back(Row {Spacer(), Button(revision.Get() || withdrawing.Get() ? ::app::strings::ui_cancel : ::app::strings::ui_close)
      .OnClick([dialog, revision, withdrawing, error] {
        if (!revision.Get() && !withdrawing.Get()) { dialog.Dismiss(); return; }
        revision = std::nullopt;
        withdrawing = false;
        error = "";
      }).With(Enabled(!busy.Get()))}.With(Spacing(8.0F)));
  return Column(std::move(body)).With(Frame{.width = UseViewportClass() == ViewportClass::Compact ? 310.0F : 440.0F},
      Padding(20.0F), Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

[[huxerui::composable]]
View ImportReportControl(std::shared_ptr<kit::AgentRuntime> runtime, kit::ProjectId project,
    std::function<void(ImportedPresentationReport)> imported) {
  const auto picker = UseService<FilePicker>();
  const auto tasks = UseTaskScope();
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  if (!picker || !picker->CanOpenFiles()) return View{};
  std::vector<View> body{Button(::app::strings::ui_import_report_into_project).OnClick([picker, tasks, busy, error, runtime, project, imported] {
    if (busy.Get()) return;
    busy = true;
    error = "";
    tasks.Launch([picker, busy, error, runtime, project, imported]() -> Task<void> {
      auto selected = co_await picker->OpenFileAsync({.name = "JSON report", .extensions = {"json"}, .content_types = {"application/json"}});
      if (selected) {
        auto report = co_await ImportPresentationReport(runtime->Projects(), project, std::move(*selected));
        if (report.Succeeded()) imported(std::move(report).Value());
        else error = report.Error().message;
      }
      busy = false;
    });
  }).With(Enabled(!busy.Get()))};
  if (!error->empty()) body.push_back(Text(error.Get(), TextRole::Label));
  return Column(std::move(body)).With(Spacing(6.0F));
}

#ifdef __EMSCRIPTEN__
[[huxerui::composable]]
static View BrowserReportDialog(DialogContext dialog, std::shared_ptr<kit::AgentRuntime> runtime, kit::ProjectId project,
    File store, kit::WorkspaceId workspace, std::string folder, std::vector<BrowserWorkspaceEntry> initial,
    std::function<void(ImportedPresentationReport)> imported) {
  const auto tasks = UseTaskScope();
  const auto path = UseState(std::string{});
  const auto entries = UseState(std::move(initial));
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  const auto navigate = [tasks, store, workspace, path, entries, busy, error](std::string destination) {
    if (busy.Get()) return;
    busy = true;
    error = "";
    tasks.Launch([store, workspace, path, entries, busy, error, destination = std::move(destination)]() -> Task<void> {
      auto listed = co_await ListBrowserWorkspaceReports(store, workspace, destination);
      if (listed.Succeeded()) {
        path = std::move(destination);
        entries = std::move(listed).Value();
      } else {
        error = listed.Error().message;
      }
      busy = false;
    });
  };
  const auto current = path.Get();
  std::vector<View> body{Text(::app::strings::ui_choose_a_report, TextRole::Title),
      Text(current.empty() ? folder : folder + "/" + current, TextRole::Label)};
  if (!current.empty()) {
    const auto slash = current.find_last_of('/');
    auto parent = slash == std::string::npos ? std::string{} : current.substr(0, slash);
    body.push_back(Button(::app::strings::ui_parent_folder).OnClick([navigate, parent] { navigate(parent); }).With(Enabled(!busy.Get())));
  }
  std::vector<View> rows;
  for (const auto& entry : entries.Get()) {
    auto location = current.empty() ? entry.name : current + "/" + entry.name;
    if (entry.directory) {
      rows.push_back(Button(entry.name + "/").OnClick([navigate, location] { navigate(location); }).With(Enabled(!busy.Get())).Key(entry.name));
    } else {
      rows.push_back(Button(entry.name).OnClick([tasks, store, workspace, runtime, project, dialog, imported, busy, error, location] {
        if (busy.Get()) return;
        busy = true;
        error = "";
        tasks.Launch([store, workspace, runtime, project, dialog, imported, busy, error, location]() -> Task<void> {
          auto selected = co_await ReadBrowserWorkspaceReport(store, workspace, location);
          if (!selected.Succeeded()) {
            error = selected.Error().message;
            busy = false;
            co_return;
          }
          auto file = std::move(selected).Value();
          auto report = co_await ImportPresentationReport(runtime->Projects(), project, std::move(file.name), std::move(file.text));
          if (report.Succeeded()) {
            imported(std::move(report).Value());
            dialog.Dismiss();
          } else {
            error = report.Error().message;
            busy = false;
          }
        });
      }).With(Enabled(!busy.Get())).Key(entry.name));
    }
  }
  if (rows.empty()) body.push_back(Text(::app::strings::ui_no_json_reports_or_folders_here, TextRole::Label));
  else body.push_back(ScrollView(Column(std::move(rows)).With(Spacing(4.0F)))
      .With(Frame{.max_height = 240.0F}, ScrollBar()));
  if (!error->empty()) body.push_back(Text(error.Get(), TextRole::Label));
  body.push_back(Row{Spacer(), Button(::app::strings::ui_close).OnClick([dialog] { dialog.Dismiss(); })
      .With(Enabled(!busy.Get()))}.With(Spacing(8.0F)));
  return Column(std::move(body)).With(Frame{.width = 390.0F}, Padding(20.0F), Spacing(10.0F));
}

[[huxerui::composable]]
View BrowserWorkspaceReportControl(std::shared_ptr<kit::AgentRuntime> runtime, kit::ProjectId project, File store,
    kit::WorkspaceId workspace, std::string folder, std::function<void(ImportedPresentationReport)> imported) {
  const auto dialogs = UseDialog();
  const auto tasks = UseTaskScope();
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  std::vector<View> body{Button(::app::strings::ui_choose_report_from_folder).OnClick(
      [dialogs, tasks, runtime, project, store, workspace, folder, imported, busy, error] {
        if (busy.Get()) return;
        busy = true;
        error = "";
        tasks.Launch([dialogs, runtime, project, store, workspace, folder, imported, busy, error]() -> Task<void> {
          auto allowed = co_await AuthorizeBrowserWorkspace(store, workspace);
          if (!allowed.Succeeded()) {
            error = allowed.Error().message;
            busy = false;
            co_return;
          }
          auto listed = co_await ListBrowserWorkspaceReports(store, workspace, {});
          busy = false;
          if (!listed.Succeeded()) {
            error = listed.Error().message;
            co_return;
          }
          ShowPreviewDialog(dialogs, DialogFactory{[runtime, project, store, workspace, folder, imported,
                                      initial = std::move(listed).Value()](DialogContext dialog) {
            return BrowserReportDialog(dialog, runtime, project, store, workspace, folder, initial, imported);
          }});
        });
      }).With(Enabled(!busy.Get()))};
  if (!error->empty()) body.push_back(Text(error.Get(), TextRole::Label));
  return Column(std::move(body)).With(Spacing(6.0F));
}

kit::AgentResult<void> RegisterBrowserPresentationExport(
    const std::shared_ptr<kit::AgentRuntime>& runtime, File directory) {
  return runtime->RegisterContentComponent({BrowserPresentationExport,
      {JsonValue{JsonValue::Object{{"type", JsonValue{std::string("string")}}}}},
      [directory = std::move(directory), weak = std::weak_ptr<kit::AgentRuntime>(runtime)](
          kit::RenderInput<kit::CustomBlock> input) -> View {
        const auto* name = std::get_if<std::string>(&input.value->data.value);
        if (!name || !ValidatePresentationFilename(*name).Succeeded() || input.value->artifacts.size() != 1)
          return Text(input.value->fallback);
        return BrowserExportCard(directory.Child(*name), input.value->artifacts.front(), weak);
      }});
}
#endif

void ConfigurePreviewArtifactReview(kit::AgentChatOptions& options, std::shared_ptr<kit::AgentRuntime> runtime,
    State<PresentationReviewPages> pages, OpenPresentationLocation open, std::shared_ptr<CodeWorkspaces> workspaces) {
  options.render_artifact = [pages, runtime, workspaces = std::move(workspaces)](
      const kit::ArtifactPreview& preview, std::shared_ptr<kit::Conversation> session,
      State<kit::AgentChatDraftSources> draft) -> std::optional<View> {
    if (IsVerificationScreenshot(preview.artifact)) return ScreenshotPreview(runtime, preview.artifact).Key(preview.artifact.id.value);
    if (!preview.artifact.id.value.starts_with("presentation:") || !preview.structured) return {};
    auto outline = ReadPresentationOutline(*preview.structured);
    if (!outline.Succeeded()) return Text(outline.Error().message);
    const auto& fields = std::get<JsonValue::Object>(preview.structured->value);
    const auto screenshot = fields.find("screenshot_page");
    std::optional<std::size_t> page;
    if (screenshot != fields.end()) {
      const auto* number = std::get_if<std::int64_t>(&screenshot->second.value);
      if (number && *number > 0 && static_cast<std::uint64_t>(*number) <= outline.Value().slides.size())
        page = static_cast<std::size_t>(*number);
    }
    return StagedPresentationReview(std::move(outline).Value(), page, preview.artifact, std::move(session), runtime, draft, pages, workspaces)
        .Key(preview.artifact.id.value + "/" + std::to_string(preview.artifact.version));
  };
  options.render_artifact_summary = [runtime, open = std::move(open)](const kit::ArtifactPreview& preview,
      std::shared_ptr<kit::Conversation> session) -> std::optional<View> {
    const auto* data = preview.structured ? std::get_if<JsonValue::Object>(&preview.structured->value) : nullptr;
    if (!data) return {};
    const auto kind = data->find("kind");
    if (kind == data->end() || kind->second != JsonValue{std::string(VerificationEvidence)}) return {};
    return VerificationReportSummary(preview, std::move(session), runtime, open);
  };
}

[[huxerui::composable]]
View PresentationChat(std::shared_ptr<kit::Conversation> conversation, std::shared_ptr<kit::AgentRuntime> runtime,
                      kit::AgentChatOptions base_options) {
  const auto drafts = UseState(Drafts{});
  const auto review_pages = UseState(PresentationReviewPages{});
  const auto tasks = UseTaskScope();
  const auto colors = UseTheme().colors;
  const auto buttons = UseEnvironment<ButtonStyle>();
  const auto navigation = PresentationNavigationStyle(buttons, colors.on_surface_variant);
  const auto form_fields = PreviewFormFieldStyle(UseEnvironment<TextFieldStyle>(), colors);
  const auto untitled_slide = UseString(::app::strings::ui_untitled_slide);
  auto options = base_options;
  options.render_form = [drafts, tasks, runtime, navigation, colors, form_fields, buttons, untitled_slide, fallback = base_options.render_form](
      const kit::InteractionSnapshot& request, std::shared_ptr<kit::Conversation> session) -> std::optional<View> {
    if (auto rendered = RenderForm(request, session, drafts, tasks, navigation, runtime, colors, form_fields, buttons, untitled_slide)) return rendered;
    return fallback ? fallback(request, std::move(session)) : std::nullopt;
  };
  if (!options.render_artifact) ConfigurePreviewArtifactReview(options, runtime, review_pages);
  return kit::AgentChat(std::move(conversation), std::move(options));
}

} // namespace agentkit_preview
