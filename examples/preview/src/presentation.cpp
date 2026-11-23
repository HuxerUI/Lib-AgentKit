#include "presentation.h"
#include "preview_json.h"

#include <huxerui/archive.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdio>
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
constexpr std::string_view chart_ns = "http://schemas.openxmlformats.org/drawingml/2006/chart";
constexpr std::string_view sheet_ns = "http://schemas.openxmlformats.org/spreadsheetml/2006/main";
constexpr std::array layout_names{
    std::pair{SlideLayout::Cover, "cover"}, std::pair{SlideLayout::List, "list"},
    std::pair{SlideLayout::Cards, "cards"}, std::pair{SlideLayout::Flow, "flow"},
    std::pair{SlideLayout::Comparison, "comparison"}, std::pair{SlideLayout::Image, "image"},
    std::pair{SlideLayout::Chart, "chart"}, std::pair{SlideLayout::Relationship, "relationship"},
    std::pair{SlideLayout::Editorial, "editorial"}};

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
    if (slide.media && !slide.media->empty()) ValidateText(*slide.media, 1600, "Slide media");
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

struct Link { std::string id; std::string type; std::string target; bool external = false; };

std::string Relationships(const std::vector<Link>& links) {
  std::string xml = "<Relationships" + Namespace("", package_rel_ns) + '>';
  for (const auto& link : links) {
    xml += "<Relationship Id=\"" + link.id + "\" Type=\"" + std::string(office_rel_ns) + "/" + link.type +
           "\" Target=\"" + Escape(link.target) + "\"" + (link.external ? " TargetMode=\"External\"" : "") + "/>";
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

std::string Geometry(int x, int y, int width, int height, std::string_view preset = "rect") {
  const auto adjustment = preset == "roundRect" ? "<a:avLst><a:gd name=\"adj\" fmla=\"val 6500\"/></a:avLst>" : "<a:avLst/>";
  return "<a:xfrm><a:off x=\"" + std::to_string(x) + "\" y=\"" + std::to_string(y) + "\"/>"
         "<a:ext cx=\"" + std::to_string(width) + "\" cy=\"" + std::to_string(height) + "\"/></a:xfrm>"
         "<a:prstGeom prst=\"" + std::string(preset) + "\">" + adjustment + "</a:prstGeom>";
}

std::string Paragraph(std::string_view text, int font_size, std::string_view color, bool bold, bool bullet,
    bool centered = false, bool link = false) {
  const auto style = bullet
      ? "<a:pPr marL=\"282000\" indent=\"-236000\"><a:spcAft><a:spcPts val=\"1100\"/></a:spcAft>"
        "<a:buChar char=\"&#8226;\"/></a:pPr>"
      : centered ? "<a:pPr algn=\"ctr\" eaLnBrk=\"1\" hangingPunct=\"1\"><a:buNone/></a:pPr>" :
          "<a:pPr eaLnBrk=\"1\" hangingPunct=\"1\"><a:buNone/></a:pPr>";
  return "<a:p>" + std::string(style) + "<a:r><a:rPr lang=\"en-US\" sz=\"" + std::to_string(font_size) +
      "\" b=\"" + (bold ? "1" : "0") + "\">" + Solid(color) + "<a:latin typeface=\"Aptos\"/>"
      + (link ? "<a:hlinkClick r:id=\"rIdSource\"/>" : "") + "</a:rPr><a:t>" + Escape(text) + "</a:t></a:r></a:p>";
}

std::string Shape(int id, const char* name, int x, int y, int w, int h, std::string text,
                  std::string_view fill = {}, std::string_view preset = "rect", bool card = false) {
  return "<p:sp><p:nvSpPr><p:cNvPr id=\"" + std::to_string(id) + "\" name=\"" + name +
      "\"/><p:cNvSpPr txBox=\"" + (text.empty() || card ? "0" : "1") + "\"/><p:nvPr/></p:nvSpPr><p:spPr>" +
      Geometry(x, y, w, h, preset) + (fill.empty() ? "<a:noFill/>" : Solid(fill)) + "<a:ln><a:noFill/></a:ln></p:spPr>" +
      (text.empty() ? "" : "<p:txBody>" + std::string(card
          ? "<a:bodyPr wrap=\"square\" anchor=\"ctr\" lIns=\"205000\" tIns=\"130000\" rIns=\"205000\" bIns=\"130000\">"
          : "<a:bodyPr wrap=\"square\" lIns=\"0\" tIns=\"0\" rIns=\"0\" bIns=\"0\">") +
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

std::string FlowAnimation(std::size_t steps) {
  std::string xml = "<p:timing><p:tnLst><p:par><p:cTn id=\"1\" dur=\"indefinite\" restart=\"never\" nodeType=\"tmRoot\">"
      "<p:childTnLst><p:seq concurrent=\"1\" nextAc=\"seek\"><p:cTn id=\"2\" dur=\"indefinite\" nodeType=\"mainSeq\">"
      "<p:childTnLst><p:par><p:cTn id=\"3\" fill=\"hold\"><p:stCondLst><p:cond delay=\"indefinite\"/>"
      "<p:cond evt=\"onBegin\" delay=\"0\"><p:tn val=\"2\"/></p:cond></p:stCondLst><p:childTnLst>"
      "<p:par><p:cTn id=\"4\" fill=\"hold\"><p:stCondLst><p:cond delay=\"0\"/></p:stCondLst><p:childTnLst>";
  std::string builds;
  for (std::size_t i = 0; i < steps; ++i) {
    const auto node = 5 + i * 3;
    const auto shape = std::to_string(10 + i);
    const auto target = "<p:tgtEl><p:spTgt spid=\"" + shape + "\"/></p:tgtEl>";
    xml += "<p:par><p:cTn id=\"" + std::to_string(node) +
        "\" presetID=\"10\" presetClass=\"entr\" presetSubtype=\"0\" fill=\"hold\" grpId=\"0\" nodeType=\"withEffect\">"
        "<p:stCondLst><p:cond delay=\"" + std::to_string(i * 160) +
        "\"/></p:stCondLst><p:childTnLst><p:set><p:cBhvr><p:cTn id=\"" + std::to_string(node + 1) +
        "\" dur=\"1\" fill=\"hold\"><p:stCondLst><p:cond delay=\"0\"/></p:stCondLst></p:cTn>" + target +
        "<p:attrNameLst><p:attrName>style.visibility</p:attrName></p:attrNameLst></p:cBhvr>"
        "<p:to><p:strVal val=\"visible\"/></p:to></p:set><p:animEffect transition=\"in\" filter=\"fade\">"
        "<p:cBhvr><p:cTn id=\"" + std::to_string(node + 2) + "\" dur=\"300\"/>" + target +
        "</p:cBhvr></p:animEffect></p:childTnLst></p:cTn></p:par>";
    builds += "<p:bldP spid=\"" + shape + "\" grpId=\"0\" animBg=\"1\"/>";
  }
  return xml + "</p:childTnLst></p:cTn></p:par></p:childTnLst></p:cTn></p:par></p:childTnLst></p:cTn>"
      "<p:prevCondLst><p:cond evt=\"onPrev\" delay=\"0\"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:prevCondLst>"
      "<p:nextCondLst><p:cond evt=\"onNext\" delay=\"0\"><p:tgtEl><p:sldTgt/></p:tgtEl></p:cond></p:nextCondLst>"
      "</p:seq></p:childTnLst></p:cTn></p:par></p:tnLst><p:bldLst>" + builds + "</p:bldLst></p:timing>";
}

std::string Diagram(const SlideOutline& slide, SlideLayout layout) {
  const auto count = static_cast<int>(slide.bullets.size());
  const bool flow = layout == SlideLayout::Flow;
  const int columns = flow ? count : 2;
  const int gap = flow ? 380000 : 290000;
  const int width = (10432000 - gap * (columns - 1)) / columns;
  const int height = !flow && count > 2 ? 1490000 : 2070000;
  std::string shapes;
  for (int i = 0; i < count; ++i) {
    const int x = 880000 + (i % columns) * (width + gap);
    const int y = 2740000 + (i / columns) * (height + gap);
    const auto& point = slide.bullets[i];
    const bool compact = point.size() > 72;
    const bool alternate = layout == SlideLayout::Comparison && i == 1;
    const auto ink = alternate ? "273B5D" : "163C36";
    const auto fill = alternate ? "E8EEF8" : flow ? "E4EFEB" : "EAF1EE";
    const auto text = Paragraph(point, columns > 2 ? (compact ? 1640 : 1950) : (compact ? 1900 : 2200), ink, !compact, false);
    shapes += Shape(10 + i, flow ? "Flow step" : "Content card", x, y, width, height, text, fill, "roundRect", true);
    if (!flow)
      shapes += Shape(30 + i, "Panel accent", x + 185000, y + 115000, 280000, 46000, {}, alternate ? "5374AC" : "288779");
    if (flow && i + 1 < count)
      shapes += Shape(20 + i, "Step connector", x + width + 70000, y + height / 2 - 61000,
          240000, 122000, {}, "288779", "rightArrow");
  }
  return shapes;
}

std::string Slide(const PresentationOutline& outline, std::size_t page, const huxerui::ImageAsset* screenshot) {
  const auto& slide = outline.slides[page];
  const auto layout = ResolveSlideLayout(outline, page, screenshot != nullptr);
  const bool cover = layout == SlideLayout::Cover;
  const auto accent = cover ? "8DE2C6" : "166F62";
  const auto title_ink = cover ? "F6FBF9" : "172B30";
  const auto body_ink = cover ? "C9E2D9" : "33474B";
  std::string bullets;
  for (const auto& text : slide.bullets)
    bullets += Paragraph(text, cover ? 2130 : screenshot ? 1750 : 2080, body_ink, false, !cover);
  const auto content = layout == SlideLayout::List || cover
      ? Shape(5, "Body", 880000, cover ? 3690000 : 2680000, screenshot ? 3650000 : cover ? 7020000 : 10100000,
              cover ? 1600000 : 3560000, std::move(bullets))
      : Diagram(slide, layout);
  std::string artwork;
  if (cover) {
    artwork = Shape(8, "Cover accent", 8350000, 1870000, 2870000, 2870000, {}, "1D4B48", "ellipse") +
        Shape(9, "Cover ring", 8670000, 2190000, 2230000, 2230000, {}, "102C2C", "ellipse") +
        Shape(10, "Cover center", 9330000, 2850000, 910000, 910000, {}, "265A51", "ellipse") +
        Shape(11, "Cover highlight", 10590000, 2120000, 460000, 460000, {}, accent, "ellipse");
  } else {
    artwork = Shape(8, "Title separator", 880000, 2350000, 10432000, 18000, {}, "D1DEDA");
  }
  return "<p:sld" + PresentationNamespaces() + "><p:cSld name=\"" + Escape(slide.title) + "\">"
      "<p:bg><p:bgPr>" + Solid(cover ? "102C2C" : "F7F9F7") + "<a:effectLst/></p:bgPr></p:bg>" + SpTreePrefix() +
      Shape(2, "Accent", 880000, 587000, 46000, 250000, {}, accent) + artwork +
      Shape(3, "Presentation", 1050000, 573000, 10200000, 430000, Paragraph(outline.title, 1250, accent, true, false)) +
      Shape(4, "Title", 880000, cover ? 2020000 : 1110000, cover ? 7250000 : 10432000, cover ? 1550000 : 1170000,
          Paragraph(slide.title, cover ? 4600 : 3330, title_ink, true, false)) + content +
      (screenshot ? Screenshot(*screenshot) : std::string{}) +
      "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>"
      "<p:transition spd=\"fast\"><p:fade/></p:transition>" +
      (layout == SlideLayout::Flow ? FlowAnimation(slide.bullets.size()) : std::string{}) + "</p:sld>";
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

constexpr int emu_per_unit = PresentationWidth / 1280;

std::string Hex(unsigned color) {
  char buffer[7];
  std::snprintf(buffer, sizeof(buffer), "%06X", color);
  return buffer;
}

std::string Illustration(const SlideRect& rect, const huxerui::ImageAsset& image) {
  const auto image_ratio = static_cast<double>(image.PixelWidth()) / image.PixelHeight();
  const auto frame_ratio = static_cast<double>(rect.width) / rect.height;
  const auto x_crop = std::to_string(static_cast<int>(50000 * std::max(0.0, 1 - frame_ratio / image_ratio)));
  const auto y_crop = std::to_string(static_cast<int>(50000 * std::max(0.0, 1 - image_ratio / frame_ratio)));
  return "<p:pic><p:nvPicPr><p:cNvPr id=\"6\" name=\"Generated illustration\"/>"
      "<p:cNvPicPr><a:picLocks noChangeAspect=\"1\"/></p:cNvPicPr><p:nvPr/></p:nvPicPr>"
      "<p:blipFill><a:blip r:embed=\"rIdImage\"/><a:srcRect l=\"" + x_crop + "\" r=\"" + x_crop +
      "\" t=\"" + y_crop + "\" b=\"" + y_crop + "\"/><a:stretch><a:fillRect/></a:stretch></p:blipFill><p:spPr>" +
      Geometry(rect.x * emu_per_unit, rect.y * emu_per_unit, rect.width * emu_per_unit, rect.height * emu_per_unit) +
      "</p:spPr></p:pic>";
}

std::string ChartFrame(const SlideRect& rect) {
  return "<p:graphicFrame><p:nvGraphicFramePr><p:cNvPr id=\"7\" name=\"Editable data chart\"/>"
      "<p:cNvGraphicFramePr/><p:nvPr/></p:nvGraphicFramePr><p:xfrm><a:off x=\"" + std::to_string(rect.x * emu_per_unit) +
      "\" y=\"" + std::to_string(rect.y * emu_per_unit) + "\"/><a:ext cx=\"" + std::to_string(rect.width * emu_per_unit) +
      "\" cy=\"" + std::to_string(rect.height * emu_per_unit) + "\"/></p:xfrm><a:graphic><a:graphicData uri=\"" +
      std::string(chart_ns) + "\"><c:chart" + Namespace("c", chart_ns) +
      " r:id=\"rIdChart\"/></a:graphicData></a:graphic></p:graphicFrame>";
}

std::string SceneSlide(const PresentationOutline& outline, std::size_t page, const huxerui::ImageAsset* image) {
  const auto& slide = outline.slides[page];
  const auto scene = ComposeSlide(outline, page);
  std::string body;
  if (image && scene.image) body += Illustration(*scene.image, *image);
  if (scene.chart) body += ChartFrame(*scene.chart);
  constexpr std::array presets{"rect", "rect", "ellipse", "rightArrow", "leftArrow", "upArrow", "downArrow"};
  int next_id = 50;
  for (const auto& element : scene.elements) {
    const auto& rect = element.bounds;
    const auto color = Hex(element.color);
    const bool text = element.shape == SlideShape::Text;
    const auto id = element.excerpt ? (*element.excerpt == 0 ? 4 : 9 + static_cast<int>(*element.excerpt)) : next_id++;
    const auto name = element.excerpt ? "GanWork text " + std::to_string(*element.excerpt) :
        text ? "Annotation" : element.shape >= SlideShape::RightArrow ? "Flow connector" : "Decoration";
    const bool source = scene.chart && text && !element.excerpt;
    body += Shape(id, name.c_str(), rect.x * emu_per_unit, rect.y * emu_per_unit, rect.width * emu_per_unit,
        rect.height * emu_per_unit, text ? Paragraph(element.text, element.size * 75, color, element.bold, false,
            element.centered, source) : std::string{}, text ? std::string_view{} : color,
        presets[static_cast<std::size_t>(element.shape)]);
  }
  return "<p:sld" + PresentationNamespaces() + "><p:cSld name=\"" + Escape(slide.title) + "\"><p:bg><p:bgPr>" +
      Solid(Hex(scene.background)) + "<a:effectLst/></p:bgPr></p:bg>" + SpTreePrefix() + body +
      "</p:spTree></p:cSld><p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>"
      "<p:transition spd=\"fast\"><p:fade/></p:transition>" +
      (slide.layout == SlideLayout::Flow ? FlowAnimation(slide.bullets.size()) : std::string{}) + "</p:sld>";
}

std::string Number(double value) {
  char buffer[48];
  const auto end = std::to_chars(buffer, buffer + sizeof(buffer), value).ptr;
  return {buffer, end};
}

void AddChart(archive& writer, const huxerui::File& destination, std::string index, const PresentationChart& chart) {
  const auto scratch = destination.Parent()->Child("chart-" + kit::NewCommandId().value + ".xlsx");
  struct RemoveFile {
    const huxerui::File& file;
    ~RemoveFile() { static_cast<void>(file.Delete()); }
  } cleanup{scratch};
  auto workbook = archive::Create(scratch, huxerui::archive::ArchiveFormat::Zip);
  if (!workbook.Succeeded()) throw std::runtime_error(workbook.Error().message);
  auto& book = workbook.Value();
  const std::string sheet_type = "application/vnd.openxmlformats-officedocument.spreadsheetml.";
  AddPart(book, "[Content_Types].xml", "<Types" + Namespace("", content_type_ns) +
      "><Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>" +
      Override("xl/workbook.xml", sheet_type + "sheet.main+xml") +
      Override("xl/worksheets/sheet1.xml", sheet_type + "worksheet+xml") + "</Types>");
  AddPart(book, "_rels/.rels", Relationships({{"rId1", "officeDocument", "xl/workbook.xml"}}));
  AddPart(book, "xl/_rels/workbook.xml.rels", Relationships({{"rId1", "worksheet", "worksheets/sheet1.xml"}}));
  AddPart(book, "xl/workbook.xml", "<workbook" + Namespace("", sheet_ns) + Namespace("r", office_rel_ns) +
      "><sheets><sheet name=\"Data\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>");
  auto rows = "<row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>Category</t></is></c>"
      "<c r=\"B1\" t=\"inlineStr\"><is><t>" + Escape(chart.unit) + "</t></is></c></row>";
  const auto count = "<c:ptCount val=\"" + std::to_string(chart.points.size()) + "\"/>";
  auto categories = count;
  auto values = "<c:formatCode>General</c:formatCode>" + count;
  for (std::size_t i = 0; i < chart.points.size(); ++i) {
    const auto& [label, value] = chart.points[i];
    const auto row = std::to_string(i + 2);
    const auto point = "<c:pt idx=\"" + std::to_string(i) + "\"><c:v>";
    rows += "<row r=\"" + row + "\"><c r=\"A" + row + "\" t=\"inlineStr\"><is><t>" + Escape(label) +
        "</t></is></c><c r=\"B" + row + "\"><v>" + Number(value) + "</v></c></row>";
    categories += point + Escape(label) + "</c:v></c:pt>";
    values += point + Number(value) + "</c:v></c:pt>";
  }
  AddPart(book, "xl/worksheets/sheet1.xml", "<worksheet" + Namespace("", sheet_ns) +
      "><sheetData>" + rows + "</sheetData></worksheet>");
  auto done = book.Finish();
  if (!done.Succeeded()) throw std::runtime_error(done.Error().message);
  auto bytes = scratch.ReadBytes();
  if (!bytes.Succeeded()) throw std::runtime_error("Could not read the chart workbook");
  const auto xlsx = "Chart" + index + ".xlsx";
  auto embedded = writer.AddEntry("ppt/embeddings/" + xlsx, bytes.Value());
  if (!embedded.Succeeded()) throw std::runtime_error(embedded.Error().message);
  const auto part = "chart" + index + ".xml";
  AddPart(writer, "ppt/charts/_rels/" + part + ".rels", Relationships({{"rId1", "package", "../embeddings/" + xlsx}}));
  const auto last = std::to_string(chart.points.size() + 1);
  const auto [minimum, maximum, step] = ChartAxis(chart);
  const auto value_axis = "<c:scaling><c:orientation val=\"minMax\"/><c:max val=\"" + Number(maximum) +
      "\"/><c:min val=\"" + Number(minimum) + "\"/></c:scaling><c:delete val=\"0\"/>";
  const auto axis_style = "<c:spPr><a:ln w=\"7620\">" + Solid("CCBFAF") + "</a:ln></c:spPr>";
  AddPart(writer, "ppt/charts/" + part, "<c:chartSpace" + Namespace("c", chart_ns) + Namespace("a", drawing_ns) +
      Namespace("r", office_rel_ns) + "><c:chart><c:autoTitleDeleted val=\"1\"/><c:plotArea><c:layout><c:manualLayout>"
      "<c:layoutTarget val=\"inner\"/><c:xMode val=\"edge\"/><c:yMode val=\"edge\"/>"
      "<c:wMode val=\"factor\"/><c:hMode val=\"factor\"/><c:x val=\"0.088\"/><c:y val=\"0.037\"/>"
      "<c:w val=\"0.879\"/><c:h val=\"0.849\"/></c:manualLayout></c:layout>"
      "<c:barChart><c:barDir val=\"col\"/><c:grouping val=\"clustered\"/><c:varyColors val=\"0\"/>"
      "<c:ser><c:idx val=\"0\"/><c:order val=\"0\"/><c:tx><c:strRef><c:f>Data!$B$1</c:f><c:strCache>"
      "<c:ptCount val=\"1\"/><c:pt idx=\"0\"><c:v>" + Escape(chart.unit) + "</c:v></c:pt></c:strCache></c:strRef></c:tx>"
      "<c:spPr>" + Solid("B54230") + "</c:spPr><c:cat><c:strRef><c:f>Data!$A$2:$A$" + last +
      "</c:f><c:strCache>" + categories + "</c:strCache></c:strRef></c:cat><c:val><c:numRef><c:f>Data!$B$2:$B$" +
      last + "</c:f><c:numCache>" + values + "</c:numCache></c:numRef></c:val></c:ser>"
      "<c:dLbls><c:dLblPos val=\"outEnd\"/><c:showLegendKey val=\"0\"/><c:showVal val=\"1\"/>"
      "<c:showCatName val=\"0\"/><c:showSerName val=\"0\"/><c:showPercent val=\"0\"/><c:showBubbleSize val=\"0\"/>"
      "</c:dLbls><c:gapWidth val=\"140\"/><c:axId val=\"10\"/><c:axId val=\"20\"/></c:barChart>"
      "<c:catAx><c:axId val=\"10\"/><c:scaling><c:orientation val=\"minMax\"/></c:scaling><c:delete val=\"0\"/>"
      "<c:axPos val=\"b\"/><c:majorTickMark val=\"none\"/><c:minorTickMark val=\"none\"/><c:tickLblPos val=\"low\"/>" +
      axis_style + "<c:crossAx val=\"20\"/><c:crosses val=\"autoZero\"/></c:catAx>"
      "<c:valAx><c:axId val=\"20\"/>" + value_axis + "<c:axPos val=\"l\"/><c:majorGridlines>" + axis_style +
      "</c:majorGridlines><c:majorTickMark val=\"none\"/><c:minorTickMark val=\"none\"/><c:tickLblPos val=\"nextTo\"/>" +
      axis_style + "<c:crossAx val=\"10\"/><c:crosses val=\"autoZero\"/><c:crossBetween val=\"between\"/>"
      "<c:majorUnit val=\"" + Number(step) + "\"/>"
      "</c:valAx></c:plotArea><c:plotVisOnly val=\"1\"/></c:chart><c:spPr><a:noFill/><a:ln><a:noFill/></a:ln></c:spPr>"
      "<c:externalData r:id=\"rId1\"><c:autoUpdate val=\"0\"/></c:externalData></c:chartSpace>");
}

void AddPresentation(archive& writer, const PresentationOutline& outline,
    const std::optional<huxerui::ImageAsset>& screenshot, const huxerui::File& destination,
    const std::map<std::size_t, huxerui::ImageAsset>& illustrations) {
  std::string types = "<Types" + Namespace("", content_type_ns) + ">"
      "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
      "<Default Extension=\"xml\" ContentType=\"application/xml\"/>";
  if (screenshot || !illustrations.empty()) types += "<Default Extension=\"png\" ContentType=\"image/png\"/>";
  if (!illustrations.empty()) types += "<Default Extension=\"jpg\" ContentType=\"image/jpeg\"/>";
  if (std::ranges::any_of(outline.slides, [](const auto& slide) { return slide.layout == SlideLayout::Chart; }))
    types += "<Default Extension=\"xlsx\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet\"/>";
  const auto visual = std::ranges::any_of(outline.slides, [](const auto& slide) {
    return slide.layout && UsesSlideScene(*slide.layout, slide);
  });
  if (visual) {
    auto packed = outline;
    for (std::size_t i = 0; i < packed.slides.size(); ++i) {
      if (packed.slides[i].layout == SlideLayout::Image)
        packed.slides[i].media = i == 0 ? "embedded-cover" : i + 1 == packed.slides.size() ? "embedded-close" : "embedded";
    }
    const auto data = EncodeJson(WritePresentationOutline(packed)).dump();
    auto wrote = writer.AddEntry("ppt/ganwork/plan.json", std::as_bytes(std::span(data)));
    if (!wrote.Succeeded()) throw std::runtime_error(wrote.Error().message);
    types += Override("ppt/ganwork/plan.json", "application/json");
  }
  if (screenshot) {
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
    const auto& slide = outline.slides[i];
    const auto layout = ResolveSlideLayout(outline, i, image != nullptr);
    std::vector<Link> slide_links{{"rId1", "slideLayout", "../slideLayouts/slideLayout1.xml"}};
    const auto number = std::to_string(i + 1);
    const auto illustration = illustrations.find(i);
    if (illustration != illustrations.end()) {
      const auto name = "slide-" + number + (illustration->second.MimeType() == "image/png" ? ".png" : ".jpg");
      auto wrote = writer.AddEntry("ppt/media/" + name, illustration->second.EncodedBytes());
      if (!wrote.Succeeded()) throw std::runtime_error(wrote.Error().message);
      slide_links.push_back({"rIdImage", "image", "../media/" + name});
    }
    if (layout == SlideLayout::Chart) {
      const auto& chart = *ChartFor(outline, slide);
      AddChart(writer, destination, number, chart);
      const auto part = "charts/chart" + number + ".xml";
      slide_links.push_back({"rIdChart", "chart", "../" + part});
      slide_links.push_back({"rIdSource", "hyperlink", chart.url, true});
      types += Override("ppt/" + part, "application/vnd.openxmlformats-officedocument.drawingml.chart+xml");
    }
    if (image) slide_links.push_back({"rId2", "image", "../media/implementation.png"});
    const auto* visual_image = illustration == illustrations.end() ? nullptr : &illustration->second;
    AddPart(writer, "ppt/slides/" + stem, UsesSlideScene(layout, slide)
        ? SceneSlide(outline, i, visual_image) : Slide(outline, i, image));
    AddPart(writer, "ppt/slides/_rels/" + stem + ".rels", Relationships(slide_links));
  }
  AddPart(writer, "ppt/_rels/presentation.xml.rels", std::move(links) + "</Relationships>");
  AddPart(writer, "ppt/presentation.xml", "<p:presentation" + PresentationNamespaces() + "><p:sldMasterIdLst>"
      "<p:sldMasterId id=\"2147483648\" r:id=\"rId1\"/></p:sldMasterIdLst><p:sldIdLst>" + slides +
      "</p:sldIdLst><p:sldSz cx=\"" + std::to_string(PresentationWidth) + "\" cy=\"" +
      std::to_string(PresentationHeight) + "\"/><p:notesSz cx=\"6858000\" cy=\"9144000\"/>"
      "</p:presentation>");
  AddPart(writer, "[Content_Types].xml", std::move(types) + "</Types>");
}
} // namespace

kit::JsonSchema PresentationOutlineSchema(SlideLayoutField layout, bool include_evidence) {
  const auto object = [](JsonValue::Object properties, JsonValue::Array required) -> JsonValue {
    return {JsonValue::Object{{"type", JsonValue{std::string("object")}}, {"properties", JsonValue{std::move(properties)}},
        {"required", JsonValue{std::move(required)}}, {"additionalProperties", JsonValue{false}}}};
  };
  const auto array = [](JsonValue item) -> JsonValue {
    return {JsonValue::Object{{"type", JsonValue{std::string("array")}}, {"items", std::move(item)}}};
  };
  JsonValue::Object fields{{"title", StringSchema()}, {"bullets", array(StringSchema())}};
  JsonValue::Array required{JsonValue{std::string("title")}, JsonValue{std::string("bullets")}};
  if (layout != SlideLayoutField::Omit) {
    JsonValue::Array names;
    for (const auto& [_, name] : layout_names) names.push_back(JsonValue{std::string(name)});
    fields.emplace("layout", JsonValue{JsonValue::Object{{"type", JsonValue{std::string("string")}}, {"enum", JsonValue{std::move(names)}}}});
    fields.emplace("media", StringSchema());
    if (layout == SlideLayoutField::Visual) {
      required.push_back(JsonValue{std::string("layout")});
      required.push_back(JsonValue{std::string("media")});
    }
  }
  auto slide = object(std::move(fields), std::move(required));
  JsonValue::Object deck{{"title", StringSchema()}, {"slides", array(std::move(slide))}};
  if (include_evidence) deck.emplace("evidence", StringSchema());
  return {object(std::move(deck), {JsonValue{std::string("title")}, JsonValue{std::string("slides")}})};
}

kit::AgentResult<PresentationOutline> ReadPresentationOutline(const JsonValue& value) {
  try {
    PresentationOutline outline{.title = AsString(Field(value, "title"))};
    for (const auto& page : AsArray(Field(value, "slides"))) {
      SlideOutline slide{.title = AsString(Field(page, "title"))};
      for (const auto& bullet : AsArray(Field(page, "bullets"))) slide.bullets.push_back(AsString(bullet));
      const auto& fields = std::get<JsonValue::Object>(page.value);
      if (const auto layout = fields.find("layout"); layout != fields.end()) {
        const auto& name = AsString(layout->second);
        const auto kind = std::ranges::find_if(layout_names, [&name](const auto& entry) { return name == entry.second; });
        if (kind == layout_names.end()) throw InvalidOutline("Unknown slide layout");
        slide.layout = kind->first;
      }
      if (const auto media = fields.find("media"); media != fields.end()) slide.media = AsString(media->second);
      outline.slides.push_back(std::move(slide));
    }
    const auto& fields = std::get<JsonValue::Object>(value.value);
    if (const auto evidence = fields.find("evidence"); evidence != fields.end()) {
      outline.supplied_data = ReadSuppliedChart(AsString(evidence->second));
      if (!outline.supplied_data) throw InvalidOutline("Invalid presentation evidence");
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
    JsonValue::Object fields{{"title", JsonValue{slide.title}}, {"bullets", JsonValue{std::move(bullets)}}};
    if (const auto layout = std::ranges::find(layout_names, slide.layout.value_or(SlideLayout::List),
        &decltype(layout_names)::value_type::first); slide.layout && layout != layout_names.end())
      fields.emplace("layout", JsonValue{std::string(layout->second)});
    if (slide.media) fields.emplace("media", JsonValue{*slide.media});
    slides.push_back(JsonValue{std::move(fields)});
  }
  JsonValue::Object deck{{"title", JsonValue{outline.title}}, {"slides", JsonValue{std::move(slides)}}};
  if (outline.supplied_data) {
    const auto& chart = *outline.supplied_data;
    std::string evidence = "Source: " + chart.url + "\n| Category | " + chart.unit + " |\n| --- | --- |\n";
    for (const auto& [label, value] : chart.points) {
      char number[48];
      const auto end = std::to_chars(number, number + sizeof(number), value).ptr;
      evidence += "| " + label + " | " + std::string(number, end) + " |\n";
    }
    deck.emplace("evidence", JsonValue{std::move(evidence)});
  }
  return {std::move(deck)};
}

SlideLayout ResolveSlideLayout(const PresentationOutline& outline, std::size_t page, bool has_screenshot) {
  const auto& slide = outline.slides[page];
  if (!slide.layout || has_screenshot) return SlideLayout::List;
  const auto count = slide.bullets.size();
  const auto short_labels = std::ranges::all_of(slide.bullets, [](const auto& text) { return text.size() <= 210; });
  switch (*slide.layout) {
  case SlideLayout::Cover:
    return count <= 2 && short_labels ? SlideLayout::Cover : SlideLayout::List;
  case SlideLayout::Cards:
  case SlideLayout::Flow:
    return count >= 2 && count <= 4 && short_labels ? *slide.layout : SlideLayout::List;
  case SlideLayout::Comparison:
    return count == 2 && short_labels ? SlideLayout::Comparison : SlideLayout::List;
  case SlideLayout::Image:
  case SlideLayout::Chart:
  case SlideLayout::Relationship:
  case SlideLayout::Editorial:
    return *slide.layout;
  case SlideLayout::List:
    return SlideLayout::List;
  }
  return SlideLayout::List;
}

kit::AgentResult<PresentationOutline> ReadPackedPresentationPlan(std::span<const std::byte> bytes) {
  try {
    const std::string_view json(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return ReadPresentationOutline(DecodeJson(nlohmann::json::parse(json)));
  } catch (const nlohmann::json::exception&) {
    return Fail<PresentationOutline>(kit::ErrorCode::InvalidArgument, "The staged presentation layout is invalid");
  }
}

kit::AgentResult<void> CreatePptx(const PresentationOutline& outline, const huxerui::File& destination,
    const std::optional<huxerui::ImageAsset>& screenshot,
    const std::map<std::size_t, huxerui::ImageAsset>& illustrations) {
  try {
    Validate(outline);
    for (std::size_t i = 0; i < outline.slides.size(); ++i) {
      const auto& slide = outline.slides[i];
      const auto layout = ResolveSlideLayout(outline, i, screenshot && i + 1 == outline.slides.size());
      if (layout == SlideLayout::Chart && !ChartFor(outline, slide))
        throw InvalidOutline("A chart requires a verified dataset");
      if (layout == SlideLayout::Relationship && slide.bullets.size() != 4)
        throw InvalidOutline("A relationship diagram requires four nodes");
      if (layout == SlideLayout::Image) {
        const auto image = illustrations.find(i);
        if (image == illustrations.end() || (image->second.MimeType() != "image/jpeg" &&
            image->second.MimeType() != "image/png") || !image->second.PixelWidth() || !image->second.PixelHeight())
          throw InvalidOutline("The illustration is missing or invalid");
      }
    }
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
    AddPresentation(writer, outline, screenshot, destination, illustrations);
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
    const bool named = xml.find("name=\"GanWork text 0\"") != xml.npos;
    std::size_t copied = 0, paragraphs = 0;
    for (auto open = xml.find("<a:t>"); open != xml.npos; open = xml.find("<a:t>", copied)) {
      const auto content = open + 5;
      const auto end = xml.find("</a:t>", content);
      if (end == xml.npos) return Fail<void>(kit::ErrorCode::Unsupported, "The selected slide has an unfamiliar text layout");
      output += xml.substr(copied, content - copied);
      const auto text = xml.substr(content, end - content);
      const auto found = replacements.find(std::string(text));
      const auto shape = xml.rfind("<p:sp>", open);
      const auto name = shape == xml.npos ? xml.npos : xml.find("name=\"GanWork text ", shape);
      const auto point = named ? name != xml.npos && name < open && xml[name + std::string_view("name=\"GanWork text ").size()] != '0' : ++paragraphs > 2;
      if (point && found != replacements.end()) {
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
  const auto metadata = reader.Value().ReadEntry("ppt/ganwork/plan.json");
  if (metadata.Succeeded()) {
    auto plan = ReadPackedPresentationPlan(metadata.Value());
    if (!plan.Succeeded()) return kit::AgentResult<void>::Failure(plan.Error());
    for (const auto& change : changes) {
      if (change.slide > plan.Value().slides.size())
        return Fail<void>(kit::ErrorCode::Conflict, "The staged plan does not contain the selected slide");
      auto& bullets = plan.Value().slides[change.slide - 1].bullets;
      std::ranges::replace(bullets, change.previous, change.replacement);
    }
    edited.emplace("ppt/ganwork/plan.json", EncodeJson(WritePresentationOutline(plan.Value())).dump());
  }
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
