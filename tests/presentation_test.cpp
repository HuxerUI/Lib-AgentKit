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
      {{"What changed", {"Approval links the plan to the working copy", "UTF-8 sample: \xe6\xb5\x8b\xe8\xaf\x95"}},
       {"Verified <export>", {"Use the edited outline", "Preserve quotes \" and apostrophes '"}}}};
  auto outlined = ReadPresentationOutline(WritePresentationOutline(approved));
  Check(outlined.Succeeded() && outlined.Value().slides.back().title == approved.slides.back().title,
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
  Check(first.find("AgentKit &amp; local execution") != std::string::npos &&
        first.find("UTF-8 sample: \xe6\xb5\x8b\xe8\xaf\x95") != std::string::npos &&
        second.find("Verified &lt;export&gt;") != std::string::npos &&
        second.find("Use the edited outline") != std::string::npos, "PPTX did not contain the approved text");
  Check(manifest.find("/ppt/theme/theme1.xml") != std::string::npos && manifest.find("/ppt/slides/slide2.xml") != std::string::npos,
        "required PresentationML parts were not declared");

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
        revised_slide.find("Preserve quotes") != revised_slide.npos &&
        second == Part(deck, "ppt/slides/slide2.xml"), "rework changed the original deck or kept stale selected text");
  updated = huxerui::archive::ArchiveReader::Open(destination);
  static_cast<void>(revised.Delete());

  approved.slides.front().bullets.front() = std::string(1, '\x01');
  auto invalid = CreatePptx(approved, destination);
  Check(!invalid.Succeeded() && invalid.Error().code == kit::ErrorCode::InvalidArgument,
        "invalid XML control characters were accepted");
}
} // namespace

int main(int argc, char** argv) {
  try {
    const bool keep = argc == 2;
    auto path = keep ? std::filesystem::path(argv[1]) : std::filesystem::temp_directory_path() /
        ("agentkit-pptx-" + kit::NewCommandId().value + ".pptx");
    const huxerui::File file(path.u8string());
    Verify(file);
    if (!keep) static_cast<void>(file.Delete());
    std::cout << "PPTX outline, parts, and no-overwrite behavior passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
