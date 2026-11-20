#include "presentation.h"

#include <huxerui/archive.h>

#include <algorithm>
#include <array>
#include <set>
#include <stdexcept>

namespace agentkit_preview {
namespace {
namespace kit = huxerui::agentkit;
using kit::JsonValue;
using archive = huxerui::archive::ArchiveWriter;

constexpr std::string_view presentation_ns = "http://schemas.openxmlformats.org/presentationml/2006/main";
constexpr std::string_view drawing_ns = "http://schemas.openxmlformats.org/drawingml/2006/main";
constexpr std::string_view office_rel_ns = "http://schemas.openxmlformats.org/officeDocument/2006/relationships";
constexpr std::string_view package_rel_ns = "http://schemas.openxmlformats.org/package/2006/relationships";
constexpr std::string_view content_type_ns = "http://schemas.openxmlformats.org/package/2006/content-types";
constexpr std::string_view deck_type = "application/vnd.openxmlformats-officedocument.presentationml.";

struct InvalidOutline final : std::runtime_error {
  using std::runtime_error::runtime_error;
};

template<class T>
kit::AgentResult<T> Fail(kit::ErrorCode code, std::string message) {
  return kit::AgentResult<T>::Failure({.code = code, .message = std::move(message)});
}

std::string Escape(std::string_view text) {
  std::string result;
  result.reserve(text.size());
  for (std::size_t pos = 0; pos < text.size();) {
    const auto first = static_cast<unsigned char>(text[pos]);
    const std::size_t length = first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
    if ((length > 1 && first < 0xc2) || first > 0xf4 || pos + length > text.size()) {
      throw InvalidOutline("Slide text contains invalid UTF-8");
    }
    auto point = static_cast<std::uint32_t>(length == 1 ? first : first & (0x7f >> length));
    for (std::size_t offset = 1; offset < length; ++offset) {
      const auto byte = static_cast<unsigned char>(text[pos + offset]);
      if ((byte & 0xc0) != 0x80) throw InvalidOutline("Slide text contains invalid UTF-8");
      point = (point << 6) | (byte & 0x3f);
    }
    constexpr std::array<std::uint32_t, 4> minimum{0, 0x80, 0x800, 0x10000};
    if (point < minimum[length - 1] || point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff) ||
        point == 0xfffe || point == 0xffff || (point < 32 && point != 9 && point != 10 && point != 13)) {
      throw InvalidOutline("Slide text contains an invalid XML character");
    }
    switch (point) {
    case '&': result += "&amp;"; break;
    case '<': result += "&lt;"; break;
    case '>': result += "&gt;"; break;
    case '"': result += "&quot;"; break;
    case '\'': result += "&apos;"; break;
    default: result.append(text.substr(pos, length)); break;
    }
    pos += length;
  }
  return result;
}

void ValidateText(const std::string& value, std::size_t limit, std::string_view name) {
  if (value.size() > limit || value.find_first_not_of(" \t\r\n") == std::string::npos) {
    throw InvalidOutline(std::string(name) + " is empty or too long");
  }
  static_cast<void>(Escape(value));
}

void Validate(const PresentationOutline& outline) {
  ValidateText(outline.title, 360, "Presentation title");
  if (outline.slides.empty() || outline.slides.size() > 30) throw InvalidOutline("Expected 1 to 30 slides");
  for (const auto& slide : outline.slides) {
    ValidateText(slide.title, 360, "Slide title");
    if (slide.bullets.size() > 6) throw InvalidOutline("Use up to six bullets per slide");
    for (const auto& bullet : slide.bullets) ValidateText(bullet, 720, "Slide bullet");
  }
}

JsonValue StringSchema() {
  return {JsonValue::Object{{"type", JsonValue{std::string("string")}}}};
}

const JsonValue& Field(const JsonValue& value, std::string_view name) {
  const auto* object = std::get_if<JsonValue::Object>(&value.value);
  if (!object) throw InvalidOutline("Expected an outline object");
  auto found = object->find(name);
  if (found == object->end()) throw InvalidOutline("Outline is missing " + std::string(name));
  return found->second;
}

const std::string& AsString(const JsonValue& value) {
  const auto* text = std::get_if<std::string>(&value.value);
  if (!text) throw InvalidOutline("Expected outline text");
  return *text;
}

const JsonValue::Array& AsArray(const JsonValue& value) {
  const auto* list = std::get_if<JsonValue::Array>(&value.value);
  if (!list) throw InvalidOutline("Expected an outline list");
  return *list;
}

std::string Namespace(std::string_view prefix, std::string_view url) {
  return " xmlns" + std::string(prefix.empty() ? "" : ":") + std::string(prefix) + "=\"" + std::string(url) + '"';
}

std::string PresentationNamespaces() {
  return Namespace("p", presentation_ns) + Namespace("a", drawing_ns) + Namespace("r", office_rel_ns);
}

struct Link { std::string id; std::string type; std::string target; };

std::string Relationships(std::initializer_list<Link> links) {
  std::string xml = "<Relationships" + Namespace("", package_rel_ns) + '>';
  for (const auto& link : links) {
    xml += "<Relationship Id=\"" + link.id + "\" Type=\"" + std::string(office_rel_ns) + "/" + link.type +
           "\" Target=\"" + link.target + "\"/>";
  }
  return xml + "</Relationships>";
}

std::string Override(std::string_view path, std::string_view type) {
  return "<Override PartName=\"/" + std::string(path) + "\" ContentType=\"" + std::string(type) + "\"/>";
}

std::string SpTreePrefix() {
  return "<p:spTree><p:nvGrpSpPr><p:cNvPr id=\"1\" name=\"\"/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>"
         "<p:grpSpPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"0\" cy=\"0\"/>"
         "<a:chOff x=\"0\" y=\"0\"/><a:chExt cx=\"0\" cy=\"0\"/></a:xfrm></p:grpSpPr>";
}

std::string Solid(std::string_view color) {
  return "<a:solidFill><a:srgbClr val=\"" + std::string(color) + "\"/></a:solidFill>";
}

std::string Geometry(int x, int y, int width, int height) {
  return "<a:xfrm><a:off x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) + "\"/>"
         "<a:ext cx=\"" + std::to_string(width) + "\" cy=\"" + std::to_string(height) + "\"/></a:xfrm>"
         "<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom>";
}

std::string Paragraph(std::string_view text, int font_size, std::string_view color, bool bold, bool bullet) {
  const auto style = bullet
      ? "<a:pPr marL=\"282000\" indent=\"-236000\"><a:spcAft><a:spcPts val=\"1100\"/></a:spcAft>"
        "<a:buChar char=\"&#8226;\"/></a:pPr>"
      : "<a:pPr><a:buNone/></a:pPr>";
  return "<a:p>" + std::string(style) + "<a:r><a:rPr lang=\"en-US\" sz=\"" + std::to_string(font_size) +
      "\" b=\"" + (bold ? "1" : "0") + "\">" + Solid(color) + "<a:latin typeface=\"Aptos\"/>"
      "</a:rPr><a:t>" + Escape(text) + "</a:t></a:r></a:p>";
}

std::string Shape(int id, const char* name, int x, int y, int w, int h, std::string text,
                  std::string_view fill = {}) {
  return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"" + name +
      "\"/><p:cNvSpPr txBox=\"" + (text.empty() ? "0" : "1") + "\"/><p:nvPr/></p:nvSpPr><p:spPr>" +
      Geometry(x, y, w, h) + (fill.empty() ? "<a:noFill/>" : Solid(fill)) + "<a:ln><a:noFill/></a:ln></p:spPr>" +
      (text.empty() ? "" : "<p:txBody><a:bodyPr wrap=\"square\" lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\">"
          "<a:normAutofit/></a:bodyPr><a:lstStyle/>" + std::move(text) + "</p:txBody>") + "</p:sp>";
}

std::string Screenshot(const huxerui::ImageAsset& image) {
  constexpr int width = 6390000;
  constexpr int height = 3970000;
  const auto scale = std::min(static_cast<double>(width) / image.PixelWidth(),
                              static_cast<double>(height) / image.PixelHeight());
  const auto fitted_width = static_cast<int>(image.PixelWidth() * scale);
  const auto fitted_height = static_cast<int>(image.PixelHeight() * scale);
  return "<p:pic><p:nvPicPr><p:cNvPr id=\"6\" name=\"Implementation screenshot\" descr=\"Verified workspace implementation\"/>"
      "<p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr>"
      "<p:blipFill><a:blip r:embed=\"rId2\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" +
      Geometry(4870000 + (width - fitted_width) / 2, 2440000 + (height - fitted_height) / 2, fitted_width, fitted_height) +
      "</p:spPr></p:pic>";
}

std::string Slide(const PresentationOutline& outline, const SlideOutline& slide, const huxerui::ImageAsset* screenshot) {
  std::string bullets;
  for (const auto& text : slide.bullets) bullets += Paragraph(text, 2300, "2A3841", false, true);
  return "<p:sld" + PresentationNamespaces() + "><p:cSld name=\"" + Escape(slide.title) + "\">"
      "<p:bg><p:bgPr>" + Solid("F5F7F6") + "<a:effectLst/></p:bgPr></p:bg>" + SpTreePrefix() +
      Shape(2, "Accent", 750000, 565000, 78000, 315000, {}, "166F62") +
      Shape(3, "Presentation", 960000, 575000, 10300000, 430000, Paragraph(outline.title, 1300, "166F62", true, false)) +
      Shape(4, "Title", 960000, 1080000, 10300000, 1380000, Paragraph(slide.title, 3500, "192830", true, false)) +
      Shape(5, "Body", 960000, 2740000, screenshot ? 3650000 : 10100000, 3560000, std::move(bullets)) +
      (screenshot ? Screenshot(*screenshot) : std::string{}) +
      "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sld>";
}

std::string Theme() {
  const std::array colors{
      std::pair{"dk1", "192830"}, std::pair{"lt1", "FFFFFF"}, std::pair{"dk2", "2A3841"}, std::pair{"lt2", "F5F7F6"},
      std::pair{"accent1", "166F62"}, std::pair{"accent2", "5081A0"}, std::pair{"accent3", "B37943"},
      std::pair{"accent4", "7465A1"}, std::pair{"accent5", "427A7B"}, std::pair{"accent6", "AA606A"},
      std::pair{"hlink", "176E8C"}, std::pair{"folHlink", "7465A1"}};
  std::string palette = "<a:clrScheme name=\"AgentKit\">";
  for (auto [tag, color] : colors) {
    palette += "<a:" + std::string(tag) + "><a:srgbClr val=\"" + color + "\"/></a:" + tag + ">";
  }
  palette += "</a:clrScheme>";
  std::string fonts = "<a:fontScheme name=\"Aptos\">";
  for (auto name : {"majorFont", "minorFont"}) {
    fonts += "<a:" + std::string(name) + "><a:latin typeface=\"Aptos\"/><a:ea typeface=\"\"/>"
             "<a:cs typeface=\"\"/></a:" + name + ">";
  }
  fonts += "</a:fontScheme>";
  const std::string fill = "<a:solidFill><a:schemeClr val=\"phClr\"/></a:solidFill>";
  const std::string line = "<a:ln w=\"12700\">" + fill + "<a:prstDash val=\"solid\"/></a:ln>";
  const std::string effect = "<a:effectStyle><a:effectLst/></a:effectStyle>";
  return "<a:theme" + Namespace("a", drawing_ns) + " name=\"AgentKit\"><a:themeElements>" + palette + fonts +
      "<a:fmtScheme name=\"AgentKit\"><a:fillStyleLst>" + fill + fill + fill + "</a:fillStyleLst><a:lnStyleLst>" +
      line + line + line + "</a:lnStyleLst><a:effectStyleLst>" + effect + effect + effect + "</a:effectStyleLst>"
      "<a:bgFillStyleLst>" + fill + fill + fill + "</a:bgFillStyleLst></a:fmtScheme></a:themeElements></a:theme>";
}

void AddPart(archive& writer, std::string_view path, std::string content) {
  constexpr std::string_view declaration = "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>";
  content.insert(0, declaration);
  auto result = writer.AddEntry(path, std::as_bytes(std::span(content)));
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
}

void AddPresentation(archive& writer, const PresentationOutline& outline,
    const std::optional<huxerui::ImageAsset>& screenshot) {
  std::string types = "<Types" + Namespace("", content_type_ns) + ">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
  if (screenshot) {
    types += "<Default Extension=\"png\" ContentType=\"image/png\"/>";
    auto packed = writer.AddEntry("ppt/media/implementation.png", screenshot->EncodedBytes());
    if (!packed.Succeeded()) throw std::runtime_error(packed.Error().message);
  }
  types += Override("ppt/presentation.xml", std::string(deck_type) + "presentation.main+xml");
  types += Override("ppt/presProps.xml", std::string(deck_type) + "presProps+xml");
  types += Override("ppt/slideMasters/slideMaster1.xml", std::string(deck_type) + "slideMaster+xml");
  types += Override("ppt/slideLayouts/slideLayout1.xml", std::string(deck_type) + "slideLayout+xml");
  types += Override("ppt/theme/theme1.xml", "application/vnd.openxmlformats-officedocument.theme+xml");

  AddPart(writer, "_rels/.rels", Relationships({{"rId1", "officeDocument", "ppt/presentation.xml"}}));
  AddPart(writer, "ppt/presProps.xml", "<p:presentationPr" + PresentationNamespaces() + "/>");
  AddPart(writer, "ppt/slideMasters/_rels/slideMaster1.xml.rels", Relationships({
      {"rId1", "slideLayout", "../slideLayouts/slideLayout1.xml"}, {"rId2", "theme", "../theme/theme1.xml"}}));
  AddPart(writer, "ppt/slideMasters/slideMaster1.xml", "<p:sldMaster" + PresentationNamespaces() +
      "><p:cSld>" + SpTreePrefix() + "</p:spTree></p:cSld><p:clrMap bg1=\"lt1\" tx1=\"dk1\" bg2=\"lt2\" tx2=\"dk2\""
      " accent1=\"accent1\" accent2=\"accent2\" accent3=\"accent3\" accent4=\"accent4\" accent5=\"accent5\""
      " accent6=\"accent6\" hlink=\"hlink\" folHlink=\"folHlink\"/><p:sldLayoutIdLst>"
      "<p:sldLayoutId id=\"2147483649\" r:id=\"rId1\"/></p:sldLayoutIdLst>"
      "<p:txStyles><p:titleStyle/><p:bodyStyle/><p:otherStyle/></p:txStyles></p:sldMaster>");
  AddPart(writer, "ppt/slideLayouts/_rels/slideLayout1.xml.rels",
      Relationships({{"rId1", "slideMaster", "../slideMasters/slideMaster1.xml"}}));
  AddPart(writer, "ppt/slideLayouts/slideLayout1.xml", "<p:sldLayout" + PresentationNamespaces() +
      " type=\"blank\" preserve=\"1\"><p:cSld name=\"Blank\">" + SpTreePrefix() +
      "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr></p:sldLayout>");
  AddPart(writer, "ppt/theme/theme1.xml", Theme());

  auto links = Relationships({{"rId1", "slideMaster", "slideMasters/slideMaster1.xml"},
                              {"rId2", "presProps", "presProps.xml"}});
  links.resize(links.size() - std::string_view("</Relationships>").size());
  std::string slides;
  for (std::size_t i = 0; i < outline.slides.size(); ++i) {
    auto stem = "slide" + std::to_string(i + 1) + ".xml";
    auto id = "rId" + std::to_string(i + 3);
    slides += "<p:sldId id=\"" + std::to_string(256 + i) + "\" r:id=\"" + id + "\"/>";
    links += "<Relationship Id=\"" + id + "\" Type=\"" + std::string(office_rel_ns) + "/slide\" Target=\"slides/" + stem + "\"/>";
    types += Override("ppt/slides/" + stem, std::string(deck_type) + "slide+xml");
    const auto* image = screenshot && i + 1 == outline.slides.size() ? &*screenshot : nullptr;
    AddPart(writer, "ppt/slides/" + stem, Slide(outline, outline.slides[i], image));
    AddPart(writer, "ppt/slides/_rels/" + stem + ".rels", image ? Relationships({
        {"rId1", "slideLayout", "../slideLayouts/slideLayout1.xml"}, {"rId2", "image", "../media/implementation.png"}}) :
        Relationships({{"rId1", "slideLayout", "../slideLayouts/slideLayout1.xml"}}));
  }
  AddPart(writer, "ppt/_rels/presentation.xml.rels", std::move(links) + "</Relationships>");
  AddPart(writer, "ppt/presentation.xml", "<p:presentation" + PresentationNamespaces() + "><p:sldMasterIdLst>"
      "<p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst><p:sldIdLst>" + slides +
      "</p:sldIdLst><p:sldSz cx=\"12192000\" cy=\"6858000\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/>"
      "</p:presentation>");
  AddPart(writer, "[Content_Types].xml", std::move(types) + "</Types>");
}
} // namespace

kit::JsonSchema PresentationOutlineSchema() {
  const auto object = [](JsonValue::Object properties, JsonValue::Array required) -> JsonValue {
    return {JsonValue::Object{{"type", JsonValue{std::string("object")}}, {"properties", JsonValue{std::move(properties)}},
        {"required", JsonValue{std::move(required)}}, {"additionalProperties", JsonValue{false}}}};
  };
  const auto array = [](JsonValue item) -> JsonValue {
    return {JsonValue::Object{{"type", JsonValue{std::string("array")}}, {"items", std::move(item)}}};
  };
  auto slide = object({{"title", StringSchema()}, {"bullets", array(StringSchema())}},
                      {JsonValue{std::string("title")}, JsonValue{std::string("bullets")}});
  return {object({{"title", StringSchema()}, {"slides", array(std::move(slide))}},
                 {JsonValue{std::string("title")}, JsonValue{std::string("slides")}})};
}

kit::AgentResult<PresentationOutline> ReadPresentationOutline(const JsonValue& value) {
  try {
    PresentationOutline outline{.title = AsString(Field(value, "title"))};
    for (const auto& page : AsArray(Field(value, "slides"))) {
      SlideOutline slide{.title = AsString(Field(page, "title"))};
      for (const auto& bullet : AsArray(Field(page, "bullets"))) slide.bullets.push_back(AsString(bullet));
      outline.slides.push_back(std::move(slide));
    }
    Validate(outline);
    return kit::AgentResult<PresentationOutline>::Success(std::move(outline));
  } catch (const InvalidOutline& error) {
    return Fail<PresentationOutline>(kit::ErrorCode::InvalidArgument, error.what());
  }
}

JsonValue WritePresentationOutline(const PresentationOutline& outline) {
  JsonValue::Array slides;
  for (const auto& slide : outline.slides) {
    JsonValue::Array bullets;
    for (const auto& bullet : slide.bullets) bullets.push_back(JsonValue{bullet});
    slides.push_back(JsonValue{JsonValue::Object{{"title", JsonValue{slide.title}}, {"bullets", JsonValue{std::move(bullets)}}}});
  }
  return {JsonValue::Object{{"title", JsonValue{outline.title}}, {"slides", JsonValue{std::move(slides)}}}};
}

kit::AgentResult<void> CreatePptx(const PresentationOutline& outline, const huxerui::File& destination,
    const std::optional<huxerui::ImageAsset>& screenshot) {
  try {
    Validate(outline);
    if (screenshot && (screenshot->MimeType() != "image/png" || !screenshot->PixelWidth() || !screenshot->PixelHeight())) {
      throw InvalidOutline("Expected a PNG implementation screenshot");
    }
  } catch (const InvalidOutline& error) {
    return Fail<void>(kit::ErrorCode::InvalidArgument, error.what());
  }
  auto opened = archive::Create(destination, huxerui::archive::ArchiveFormat::Zip);
  if (!opened.Succeeded()) {
    const auto& error = opened.Error();
    const auto code = error.io_error && error.io_error->code == huxerui::IoErrorCode::AlreadyExists
        ? kit::ErrorCode::Conflict : kit::ErrorCode::ProviderFailure;
    return Fail<void>(code, error.message);
  }
  try {
    auto writer = std::move(opened).Value();
    AddPresentation(writer, outline, screenshot);
    auto finished = writer.Finish();
    if (!finished.Succeeded()) throw std::runtime_error(finished.Error().message);
    return kit::AgentResult<void>::Success();
  } catch (const std::exception& error) {
    static_cast<void>(destination.Delete());
    return Fail<void>(kit::ErrorCode::ProviderFailure, error.what());
  }
}

kit::AgentResult<void> ReworkPptxText(const huxerui::File& source, const std::optional<huxerui::File>& destination,
                                    const std::vector<PptxTextChange>& changes, std::stop_token stop) {
  namespace packages = huxerui::archive;
  auto reader = packages::ArchiveReader::Open(source, {.max_entries = 256, .max_output_bytes = 32 << 20});
  if (!reader.Succeeded()) return Fail<void>(kit::ErrorCode::NotFound, "The staged PowerPoint package is unavailable");
  std::map<std::string, std::map<std::string, std::string>> by_entry;
  try {
    for (const auto& change : changes) {
      ValidateText(change.previous, 720, "Previous conclusion");
      ValidateText(change.replacement, 720, "Replacement conclusion");
      if (change.slide == 0 || change.slide > 30 || change.previous == change.replacement)
        throw InvalidOutline("Invalid slide conclusion replacement");
      auto& replacements = by_entry["ppt/slides/slide" + std::to_string(change.slide) + ".xml"];
      if (!replacements.emplace(Escape(change.previous), Escape(change.replacement)).second)
        throw InvalidOutline("A slide conclusion was selected twice");
    }
  } catch (const InvalidOutline& error) {
    return Fail<void>(kit::ErrorCode::InvalidArgument, error.what());
  }
  if (by_entry.empty()) return Fail<void>(kit::ErrorCode::InvalidArgument, "Select at least one conclusion");
  std::map<std::string, std::string> edited;
  for (const auto& [path, replacements] : by_entry) {
    if (stop.stop_requested()) return Fail<void>(kit::ErrorCode::AlreadyClosed, "Presentation rework cancelled");
    auto input = reader.Value().ReadEntry(path);
    if (!input.Succeeded()) return Fail<void>(kit::ErrorCode::NotFound, "The selected slide is unavailable");
    std::string_view xml(reinterpret_cast<const char*>(input.Value().data()), input.Value().size());
    auto& output = edited[path];
    std::set<std::string_view> applied;
    std::size_t copied = 0, paragraphs = 0;
    for (auto open = xml.find("<a:t>"); open != xml.npos; open = xml.find("<a:t>", copied)) {
      const auto content = open + 5;
      const auto end = xml.find("</a:t>", content);
      if (end == xml.npos) return Fail<void>(kit::ErrorCode::Unsupported, "The selected slide has an unfamiliar text layout");
      output += xml.substr(copied, content - copied);
      const auto text = xml.substr(content, end - content);
      const auto found = replacements.find(std::string(text));
      if (++paragraphs > 2 && found != replacements.end()) {
        output += found->second;
        applied.insert(found->first);
      } else {
        output += text;
      }
      copied = end;
    }
    output += xml.substr(copied);
    if (applied.size() != replacements.size())
      return Fail<void>(kit::ErrorCode::Conflict, "The old conclusion does not match the selected slide text");
  }
  if (!destination) return kit::AgentResult<void>::Success();
  auto entries = reader.Value().ListEntries();
  if (!entries.Succeeded()) return Fail<void>(kit::ErrorCode::InvalidState, "The presentation package could not be listed");
  auto opened = archive::Create(*destination, packages::ArchiveFormat::Zip);
  if (!opened.Succeeded()) return Fail<void>(kit::ErrorCode::Conflict, "Could not create a new staged presentation version");
  const auto copy = [&]() -> kit::AgentResult<void> {
    auto writer = std::move(opened).Value();
    for (const auto& entry : entries.Value()) {
      if (stop.stop_requested()) return Fail<void>(kit::ErrorCode::AlreadyClosed, "Presentation rework cancelled");
      const auto changed = edited.find(entry.path);
      if (changed != edited.end()) {
        const auto bytes = std::as_bytes(std::span(changed->second.data(), changed->second.size()));
        if (!writer.AddEntry(entry.path, bytes).Succeeded())
          return Fail<void>(kit::ErrorCode::ProviderFailure, "Could not write the updated slide");
        continue;
      }
      auto bytes = reader.Value().ReadEntry(entry.path);
      if (!bytes.Succeeded() || !writer.AddEntry(entry.path, bytes.Value()).Succeeded())
        return Fail<void>(kit::ErrorCode::ProviderFailure, "Could not copy an unchanged presentation entry");
    }
    if (!writer.Finish().Succeeded()) return Fail<void>(kit::ErrorCode::ProviderFailure, "Could not finish the updated PPTX");
    return kit::AgentResult<void>::Success();
  };
  auto result = copy();
  if (!result.Succeeded()) static_cast<void>(destination->Delete());
  return result;
}

} // namespace agentkit_preview
