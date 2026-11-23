#include "presentation.h"

#include <huxerui/agentkit/types.h>
#include <huxerui/archive.h>

#include <filesystem>
#include <iostream>

namespace {
namespace kit = huxerui::agentkit;

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

std::string Part(huxerui::archive::ArchiveReader& deck, std::string_view path) {
  auto result = deck.ReadEntry(path);
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  return {reinterpret_cast<const char*>(result.Value().data()), result.Value().size()};
}

void Verify(const huxerui::File& destination) {
  using namespace agentkit_preview;
  PresentationOutline approved{
      "AgentKit & local execution",
      {{"What changed", {"Approval links the plan to the working copy", "UTF-8 sample: \xe6\xb5\x8b\xe8\xaf\x95"}, SlideLayout::Cover},
       {"Verified <export>", {"Use the edited outline", "Preserve quotes \" and apostrophes '"}, SlideLayout::Flow}}};
  auto outlined = ReadPresentationOutline(WritePresentationOutline(approved));
  Check(outlined.Succeeded() && outlined.Value().slides == approved.slides,
        "approved outline changed during round trip");
  auto written = CreatePptx(outlined.Value(), destination);
  if (!written.Succeeded()) throw std::runtime_error(written.Error().message);
  auto reopened = huxerui::archive::ArchiveReader::Open(destination);
  Check(reopened.Succeeded(), "PPTX ZIP could not be reopened");
  auto& deck = reopened.Value();
  auto presentation = Part(deck, "ppt/presentation.xml");
  auto first = Part(deck, "ppt/slides/slide1.xml");
  auto second = Part(deck, "ppt/slides/slide2.xml");
  auto manifest = Part(deck, "[Content_Types].xml");
  Check(presentation.find("id=\"256\"") != std::string::npos && presentation.find("id=\"257\"") != std::string::npos &&
        presentation.find("id=\"258\"") == std::string::npos, "expected two ordered slides");
  Check(presentation.find("<p:sldSz cx=\"12192000\" cy=\"6858000\"/>") != presentation.npos, "expected widescreen slide dimensions");
  Check(first.find("AgentKit &amp; local execution") != std::string::npos &&
        first.find("UTF-8 sample: \xe6\xb5\x8b\xe8\xaf\x95") != std::string::npos &&
        second.find("Verified &lt;export&gt;") != std::string::npos &&
        second.find("Use the edited outline") != std::string::npos, "PPTX did not contain the approved text");
  Check(manifest.find("/ppt/theme/theme1.xml") != std::string::npos && manifest.find("/ppt/slides/slide2.xml") != std::string::npos,
        "required PresentationML parts were not declared");
  Check(first.find("name=\"Cover accent\"") != first.npos && first.find("<p:fade/>") != first.npos &&
        second.find("prst=\"roundRect\"") != second.npos && second.find("prst=\"rightArrow\"") != second.npos &&
        second.find("<p:timing>") != second.npos && second.find("spid=\"10\"") != second.npos && second.find("spid=\"11\"") != second.npos,
        "the cover, editable diagram and animation were not written");

  auto repeated = CreatePptx(approved, destination);
  Check(!repeated.Succeeded(), "generation unexpectedly overwrote existing PPTX");
  Check(second == Part(deck, "ppt/slides/slide2.xml"), "existing PPTX was modified");

  const auto revised = destination.Parent()->Child("reworked-" + kit::NewCommandId().value + ".pptx");
  const std::vector<PptxTextChange> changes{{2, "Use the edited outline", "Verified <8> cases & one screenshot"}};
  auto preview = ReworkPptxText(destination, {}, changes, {});
  Check(preview.Succeeded() && !revised.Stat().Succeeded(), "preparing the rework should not write a new deck");
  auto conflicting = ReworkPptxText(destination, revised, {{2, "Obsolete result", "Updated result"}}, {});
  Check(!conflicting.Succeeded() && conflicting.Error().code == kit::ErrorCode::Conflict && !revised.Stat().Succeeded(),
        "an outdated conclusion was accepted or created a staged deck");
  auto reworked = ReworkPptxText(destination, revised, changes, {});
  if (!reworked.Succeeded()) throw std::runtime_error(reworked.Error().message);
  auto updated = huxerui::archive::ArchiveReader::Open(revised);
  Check(updated.Succeeded(), "reworked PPTX could not be opened");
  const auto entries = deck.ListEntries();
  Check(entries.Succeeded(), "could not list the source presentation parts");
  for (const auto& entry : entries.Value()) {
    if (entry.path == "ppt/slides/slide2.xml") continue;
    Check(Part(deck, entry.path) == Part(updated.Value(), entry.path), "rework modified an unselected package entry");
  }
  auto revised_slide = Part(updated.Value(), "ppt/slides/slide2.xml");
  Check(revised_slide.find("Verified &lt;8&gt; cases &amp; one screenshot") != revised_slide.npos &&
        revised_slide.find("Use the edited outline") == revised_slide.npos &&
        revised_slide.find("Preserve quotes") != revised_slide.npos && revised_slide.find("<p:timing>") != revised_slide.npos &&
        second == Part(deck, "ppt/slides/slide2.xml"), "rework changed the original deck or kept stale selected text");
  updated = huxerui::archive::ArchiveReader::Open(destination);
  static_cast<void>(revised.Delete());

  approved.slides.front().bullets.front() = std::string(1, '\x01');
  auto invalid = CreatePptx(approved, destination);
  Check(!invalid.Succeeded() && invalid.Error().code == kit::ErrorCode::InvalidArgument,
        "invalid XML control characters were accepted");
}
void VerifyEditableVisuals(const huxerui::File& folder) {
  using namespace agentkit_preview;
  auto user_data = ReadSuppliedChart("Source: https://example.org/report\n| Quarter | Requests |\n| --- | --- |\n| Q1 | -12 |\n| Q2 | 24.5 |\n");
  Check(user_data && user_data->points.size() == 2 && user_data->points[0].second == -12, "sourced chart data was lost");
  Check(!ReadSuppliedChart("Source: https://example.org/report\n| Quarter | Requests |\n| --- | --- |\n| Q1 | -12 |\n| Q1 | 24.5 |\n"),
      "duplicate chart labels were accepted");
  PresentationOutline outline{"Evidence review", {
      {"Requests by quarter", {"A change of 36.5"}, SlideLayout::Chart, std::string(UserChartData)},
      {"Review loop", {"Read", "Explain", "Adjust", "Repeat"}, SlideLayout::Relationship, ""},
      {"Next steps", {"Inspect", "Agree", "Apply"}, SlideLayout::Flow, ""}}, user_data};
  const auto target = folder.Child("agentkit-visual-" + kit::NewCommandId().value + ".pptx");
  struct DeleteFile {
    const huxerui::File& file;
    ~DeleteFile() { static_cast<void>(file.Delete()); }
  } cleanup{target};
  auto missing = outline;
  missing.supplied_data.reset();
  Check(!CreatePptx(missing, target).Succeeded() && !target.Exists(), "an unsourced chart was exported");
  const auto written = CreatePptx(outline, target);
  if (!written.Succeeded()) throw std::runtime_error(written.Error().message);
  auto opened = huxerui::archive::ArchiveReader::Open(target);
  Check(opened.Succeeded(), "editable presentation could not be opened");
  auto& deck = opened.Value();
  const auto xml = Part(deck, "ppt/charts/chart1.xml");
  Check(xml.find("<c:externalData") != xml.npos && xml.find("Data!$B$2:$B$3") != xml.npos &&
      xml.find("<c:v>-12</c:v>") != xml.npos && xml.find("<c:v>24.5</c:v>") != xml.npos,
      "native chart values or workbook references were lost");
  Check(Part(deck, "ppt/embeddings/Chart1.xlsx").starts_with("PK") &&
      Part(deck, "ppt/slides/_rels/slide1.xml.rels").find("https://example.org/report") != xml.npos,
      "the editable workbook or source link was missing");
  Check(Part(deck, "ppt/slides/slide2.xml").find("prst=\"upArrow\"") != xml.npos &&
      Part(deck, "ppt/slides/slide3.xml").find("<p:timing>") != xml.npos, "editable connectors or flow animations were lost");
  auto plan = deck.ReadEntry("ppt/ganwork/plan.json");
  Check(plan.Succeeded(), "preview layout was not packaged with the presentation");
  auto restored = ReadPackedPresentationPlan(plan.Value());
  Check(restored.Succeeded() && restored.Value().slides == outline.slides && restored.Value().supplied_data == user_data,
      "the staged preview lost a layout or source");
  const std::vector<PptxTextChange> changed{{2, "Repeat", "Review again"}};
  Check(ReworkPptxText(target, {}, changed, {}).Succeeded(), "a native diagram text selection could not be reworked");
  Check(!ReworkPptxText(target, {}, {{1, "Requests by quarter", "Overwrite title"}}, {}).Succeeded(),
      "a conclusion edit could overwrite the slide title");
  const auto revised = folder.Child("agentkit-visual-reworked-" + kit::NewCommandId().value + ".pptx");
  DeleteFile revised_cleanup{revised};
  Check(ReworkPptxText(target, revised, changed, {}).Succeeded(), "a diagram rework did not create a new version");
  auto updated = huxerui::archive::ArchiveReader::Open(revised);
  Check(updated.Succeeded(), "could not open the reworked visual presentation");
  auto metadata = updated.Value().ReadEntry("ppt/ganwork/plan.json");
  Check(metadata.Succeeded(), "could not read the reworked slide plan");
  auto after = ReadPackedPresentationPlan(metadata.Value());
  Check(after.Succeeded() && after.Value().slides[1].bullets.back() == "Review again" &&
      Part(updated.Value(), "ppt/slides/slide2.xml").find(">Repeat</a:t>") == std::string::npos,
      "the reworked presentation kept obsolete text in the slide or packaged plan");
}

} // namespace

int main(int argc, char** argv) {
  try {
    const bool keep = argc == 2;
    auto path = keep ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() /
        ("agentkit-pptx-" + kit::NewCommandId().value + ".pptx");
    const huxerui::File file(path.u8string());
    Verify(file);
    VerifyEditableVisuals(*file.Parent());
    if (!keep) static_cast<void>(file.Delete());
    std::cout << "PPTX outline, parts, and no-overwrite behavior passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
