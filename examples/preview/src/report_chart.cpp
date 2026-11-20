#include "report_chart.h"

#include "preview_json.h"

#include <huxerui/huxerui.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <numbers>
#include <set>
#include <span>

namespace agentkit_preview {
namespace {

using namespace huxerui;
namespace kit = huxerui::agentkit;
using Json = nlohmann::json;
template<class T> using Result = kit::AgentResult<T>;
using Completion = Result<kit::RunCompletion>;

template<class T>
Result<T> Fail(kit::ErrorCode code, std::string message) {
  return Result<T>::Failure({.code = code, .message = std::move(message)});
}

struct Category {
  std::string label;
  double value;
};

struct Chart {
  std::string title;
  std::vector<Category> categories;
};

kit::JsonSchema Schema(std::string_view text) {
  return {DecodeJson(Json::parse(text))};
}

kit::JsonSchema ChartSchema() {
  return Schema(R"({
    "type":"object","additionalProperties":false,"required":["title","categories"],
    "properties":{
      "title":{"type":"string","minLength":1,"maxLength":100},
      "categories":{"type":"array","minItems":2,"maxItems":8,"items":{
        "type":"object","additionalProperties":false,"required":["label","value"],
        "properties":{"label":{"type":"string","minLength":1,"maxLength":64},
                      "value":{"type":"number","minimum":0,"maximum":1000000000000}}
      }}
    }
  })");
}

kit::JsonSchema SelectionSchema() {
  return Schema(R"({
    "type":"object","additionalProperties":false,"required":["file_name","indices"],
    "properties":{"file_name":{"type":"string","minLength":5,"maxLength":100},
                  "indices":{"type":"array","minItems":1,"maxItems":8,"items":{"type":"integer","minimum":0,"maximum":7}}}
  })");
}

bool CleanLabel(std::string_view text) {
  return text.find_first_not_of(' ') != std::string_view::npos &&
      std::ranges::none_of(text, [](unsigned char byte) { return byte < 32 || byte == 127; });
}

Result<Chart> ReadChart(const Json& fields) {
  try {
    Chart chart{fields.at("title").get<std::string>()};
    const auto& categories = fields.at("categories");
    if (!CleanLabel(chart.title) || !categories.is_array() || categories.size() < 2 || categories.size() > 8)
      return Fail<Chart>(kit::ErrorCode::InvalidArgument, "Use a title and two to eight categories");
    std::set<std::string> labels;
    double total = 0;
    for (const auto& item : categories) {
      auto label = item.at("label").get<std::string>();
      const auto value = item.at("value").get<double>();
      if (!CleanLabel(label) || !labels.insert(label).second || !std::isfinite(value) || value < 0 || value > 1e12)
        return Fail<Chart>(kit::ErrorCode::InvalidArgument, "Chart categories need unique labels and nonnegative numbers");
      chart.categories.push_back({std::move(label), value});
      total += value;
    }
    if (total <= 0) return Fail<Chart>(kit::ErrorCode::InvalidArgument, "At least one category needs a positive value");
    return Result<Chart>::Success(std::move(chart));
  } catch (const Json::exception&) {
    return Fail<Chart>(kit::ErrorCode::InvalidArgument, "Invalid chart data");
  }
}

bool ValidFilename(std::string_view name) {
  if (name.size() <= 4 || name.size() > 240 || name.find_first_of("/\\:\"<>|?*") != std::string_view::npos || !CleanLabel(name)) {
    return false;
  }
  std::string lower(name);
  for (auto& ch : lower) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
  const auto stem = std::string_view(lower).substr(0, lower.find('.'));
  if (!lower.ends_with(".csv") || stem.empty() || stem.ends_with(' ') ||
      stem == "con" || stem == "nul" || stem == "aux" || stem == "prn") return false;
  if (!stem.starts_with("com") && !stem.starts_with("lpt")) return true;
  const auto suffix = stem.substr(3);
  return !((suffix.size() == 1 && suffix[0] >= '0' && suffix[0] <= '9') ||
           suffix == "\xc2\xb9" || suffix == "\xc2\xb2" || suffix == "\xc2\xb3");
}

Result<std::vector<std::size_t>> ReadIndices(const Json& value, const Chart& chart) {
  std::vector<std::size_t> indices;
  double total = 0;
  if (!value.is_array()) return Fail<std::vector<std::size_t>>(kit::ErrorCode::InvalidArgument, "Select chart categories");
  for (const auto& item : value) {
    const auto index = item.is_number_integer() ? item.get<std::int64_t>() : -1;
    if (index < 0 || static_cast<std::size_t>(index) >= chart.categories.size() ||
        (!indices.empty() && indices.back() >= static_cast<std::size_t>(index))) {
      return Fail<std::vector<std::size_t>>(kit::ErrorCode::InvalidArgument, "Invalid category selection");
    }
    indices.push_back(static_cast<std::size_t>(index));
    total += chart.categories[indices.back()].value;
  }
  if (total <= 0) return Fail<std::vector<std::size_t>>(kit::ErrorCode::InvalidArgument, "Select a category with a positive value");
  return Result<std::vector<std::size_t>>::Success(std::move(indices));
}

std::string Number(double value) {
  char buffer[48];
  const auto result = std::to_chars(std::begin(buffer), std::end(buffer), value);
  return {buffer, result.ptr};
}

std::string CsvLabel(std::string_view label) {
  std::string result = "\"";
  const auto first = label.find_first_not_of(' ');
  if (first != std::string_view::npos && std::string_view("=+-@").find(label[first]) != std::string_view::npos) result += '\'';
  for (const char ch : label) {
    if (ch == '"') result += '"';
    result += ch;
  }
  result += '"';
  return result;
}

struct ChartExport {
  File target;
  std::string csv;
  std::string selection;
};

Result<ChartExport> ReadExport(const File& directory, const kit::JsonValue& arguments) {
  const auto fields = EncodeJson(arguments);
  if (!fields.is_object() || !fields.contains("chart") || !fields.contains("file_name") || !fields["file_name"].is_string() ||
      !ValidFilename(fields["file_name"].get<std::string>()) || !fields.contains("indices")) {
    return Fail<ChartExport>(kit::ErrorCode::InvalidArgument, "Enter a .csv file name in the export folder");
  }
  auto chart = ReadChart(fields["chart"]);
  if (!chart.Succeeded()) return Result<ChartExport>::Failure(chart.Error());
  auto indices = ReadIndices(fields["indices"], chart.Value());
  if (!indices.Succeeded()) return Result<ChartExport>::Failure(indices.Error());
  ChartExport output{directory.Child(fields["file_name"].get<std::string>()), "Category,Value\r\n"};
  for (auto index : indices.Value()) {
    const auto& item = chart.Value().categories[index];
    const auto value = Number(item.value);
    output.csv += CsvLabel(item.label) + "," + value + "\r\n";
    if (!output.selection.empty()) output.selection += "\n";
    output.selection += item.label + "  " + value;
  }
  return Result<ChartExport>::Success(std::move(output));
}

Result<kit::ToolExecutionResult> WriteCsv(ChartExport output, std::stop_token stop) {
  using Write = Result<kit::ToolExecutionResult>;
  if (stop.stop_requested()) return Write::Success({.outcome = kit::ToolOutcome::Cancelled});
  const kit::ResourceId resource{output.target.Path()};
  auto file = output.target.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew);
  if (!file.Succeeded()) {
    const bool exists = file.Error().code == IoErrorCode::AlreadyExists;
    return Write::Success({.outcome = kit::ToolOutcome::Failed,
        .effects = {{resource, kit::ResourceEffectState::Unchanged}},
        .reason = kit::AgentError{.code = exists ? kit::ErrorCode::Conflict : kit::ErrorCode::ProviderFailure,
            .message = exists ? "Export target already exists" : "CSV could not be created"}});
  }
  auto written = file.Value().WriteAt(0, std::as_bytes(std::span(output.csv)));
  auto closed = file.Value().Close();
  if (!written.Succeeded() || !closed.Succeeded()) {
    return Write::Success({.outcome = kit::ToolOutcome::PartiallyApplied,
        .effects = {{resource, kit::ResourceEffectState::Unknown}},
        .reason = kit::AgentError{.code = kit::ErrorCode::ResultUnknown, .message = "Export may contain a partial CSV"}});
  }
  return Write::Success({.outcome = kit::ToolOutcome::Succeeded,
      .blocks = {{{}, kit::TextBlock{"Saved " + output.target.Path(), false}}},
      .effects = {{resource, kit::ResourceEffectState::Applied}}});
}

const std::array<Color, 8> palette{
    Color::Rgb(36, 107, 197), Color::Rgb(25, 133, 114), Color::Rgb(213, 124, 47), Color::Rgb(130, 94, 190),
    Color::Rgb(185, 70, 98), Color::Rgb(103, 126, 52), Color::Rgb(95, 111, 135), Color::Rgb(180, 104, 155)};

View Pie(Chart chart, std::vector<bool> selected, double total) {
  return Canvas([chart = std::move(chart), selected = std::move(selected), total](PaintContext& paint, Size size) {
    if (total <= 0) return;
    const Point center{size.width / 2, size.height / 2};
    const auto radius = std::min(center.x, center.y) - 4.0F;
    if (radius <= 0) return;
    const auto point = [&](double angle) {
      return Point{center.x + radius * static_cast<float>(std::cos(angle)),
                   center.y + radius * static_cast<float>(std::sin(angle))};
    };
    double angle = -std::numbers::pi / 2;
    for (std::size_t i = 0; i < chart.categories.size(); ++i) {
      if (!selected[i] || chart.categories[i].value <= 0) continue;
      const auto sweep = 2 * std::numbers::pi * chart.categories[i].value / total;
      const Size radii{radius, radius};
      Path path;
      path.MoveTo(center).LineTo(point(angle))
          .ArcTo(radii, 0, ArcSize::Small, ArcDirection::Clockwise, point(angle + sweep / 2))
          .ArcTo(radii, 0, ArcSize::Small, ArcDirection::Clockwise, point(angle + sweep)).Close();
      paint.FillPath(std::move(path), palette[i]);
      angle += sweep;
    }
  }).With(Frame{.width = 160.0F, .height = 160.0F}).Key("chart-pie");
}

struct ChartSubmission {
  kit::CommandId command;
  kit::CustomBlockAction action;
};

[[huxerui::composable]]
View ChartCard(kit::RenderInput<kit::CustomBlock> input, Chart chart) {
  const auto tasks = UseTaskScope();
  const auto selected = UseState(std::vector<bool>(chart.categories.size(), true));
  const auto name = UseState(TextEditingValue::FromText("Report chart.csv"));
  const auto busy = UseState(false);
  const auto retry = UseState(std::optional<ChartSubmission>{});
  const auto error = UseState(std::string{});
  const auto visible = selected.Get();
  double total = 0;
  Json indices = Json::array();
  for (std::size_t i = 0; i < visible.size(); ++i) {
    if (visible[i]) {
      total += chart.categories[i].value;
      indices.push_back(i);
    }
  }
  std::vector<View> legend;
  for (std::size_t i = 0; i < chart.categories.size(); ++i) {
    const auto& category = chart.categories[i];
    const auto share = visible[i] && total > 0 ? "  " + std::to_string(std::lround(100 * category.value / total)) + "%" : "";
    legend.push_back(Row{
        Canvas([i](PaintContext& paint, Size bounds) {
          paint.DrawRect({0, 0, bounds.width, bounds.height}, palette[i]);
        }).With(Frame{.width = 8.0F, .height = 8.0F}),
        Checkbox(category.label, visible[i]).OnChanged([selected, i](bool checked) {
          auto next = selected.Get();
          next[i] = checked;
          selected = std::move(next);
        }).Key("chart-category-" + std::to_string(i)),
        Text(Number(category.value) + share, TextRole::Label),
    }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center)));
  }
  std::vector<View> card{Text(chart.title, TextRole::Title), Row{
      Pie(chart, visible, total), Column(std::move(legend)).With(Spacing(4.0F)),
  }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center))};
  const auto form = std::ranges::find_if(input.interactions, [](const auto& item) {
    return item.state == kit::InteractionState::Pending && std::holds_alternative<kit::FormRequest>(item.body);
  });
  if (form != input.interactions.end() && input.origin) {
    const auto submit = [tasks, busy, retry, error, dispatch = input.dispatch, origin = *input.origin, version = form->version](
                            std::string action, Json payload) {
      if (busy.Get()) return;
      const auto submission = retry.Get().value_or(ChartSubmission{kit::NewCommandId(),
          {origin, std::move(action), DecodeJson(Json{{"version", version}, {"value", std::move(payload)}})}});
      busy = true;
      tasks.Launch([busy, retry, error, dispatch, submission]() -> Task<void> {
        auto result = co_await dispatch(submission.command, submission.action);
        busy = false;
        retry = !result.Succeeded() && result.Error().code == kit::ErrorCode::CommitUncertain
            ? std::optional{submission} : std::nullopt;
        error = result.Succeeded() ? "" : result.Error().message;
      });
    };
    card.push_back(TextField(name).Label("CSV file name").LineLimits(TextFieldLineLimits::SingleLine())
        .OnChanged([name](const TextEditingValue& value) { name = value; }).With(Frame{.max_width = 300.0F}).Key("chart-file-name"));
    card.push_back(Row{
        Button(retry.Get() ? "Retry chart action" : "Export CSV").OnClick([submit, name, indices] {
          submit("export", {{"file_name", name->text}, {"indices", indices}});
        }).With(Enabled(!busy.Get() && (retry.Get().has_value() || total > 0))),
        Button("Done").OnClick([submit] { submit("dismiss", nullptr); }).With(Enabled(!busy.Get() && !retry.Get())),
    }.With(Spacing(8.0F)));
    if (!error.Get().empty()) card.push_back(Text(error.Get(), TextRole::Label));
  }
  return Column(std::move(card)).With(Spacing(8.0F), Padding(12.0F), CrossAlign(CrossAxisAlignment::Start));
}

kit::Async<kit::ContentActionPlan> ChartDecision(kit::ContentActionContext context, kit::JsonValue payload, bool export_csv) {
  const auto command = EncodeJson(payload);
  if (context.block->interactions.size() != 1)
    co_return Fail<kit::ContentActionPlan>(kit::ErrorCode::AlreadyClosed, "Chart export is no longer available in this conversation");
  const auto version = command.at("version").get<kit::Revision>();
  kit::InteractionReply reply{.id = context.block->interactions.front(), .expected_version = version};
  if (export_csv) {
    const auto& selection = command.at("value");
    auto chart = ReadChart(EncodeJson(context.block->data));
    if (!chart.Succeeded()) co_return Result<kit::ContentActionPlan>::Failure(chart.Error());
    if (!ValidFilename(selection.at("file_name").get<std::string>()))
      co_return Fail<kit::ContentActionPlan>(kit::ErrorCode::InvalidArgument, "Enter a .csv file name in the export folder");
    auto indices = ReadIndices(selection.at("indices"), chart.Value());
    if (!indices.Succeeded()) co_return Result<kit::ContentActionPlan>::Failure(indices.Error());
    reply.response = DecodeJson(selection);
  } else {
    reply.response = kit::DismissForm{};
  }
  co_return Result<kit::ContentActionPlan>::Success(std::move(reply));
}

} // namespace

kit::AgentResult<void> RegisterReportChart(const std::shared_ptr<kit::AgentRuntime>& runtime, File exports) {
  const auto decision_schema = [](Json value) {
    return kit::JsonSchema{DecodeJson(Json{{"type", "object"}, {"additionalProperties", false},
        {"required", {"version", "value"}}, {"properties", {
            {"version", {{"type", "integer"}, {"minimum", 1}}}, {"value", std::move(value)}}}})};
  };
  auto registered = runtime->RegisterContentComponent({ReportChart, ChartSchema(),
      [](kit::RenderInput<kit::CustomBlock> input) -> View {
        auto parsed = ReadChart(EncodeJson(input.value->data));
        if (!parsed.Succeeded()) return Text(input.value->fallback);
        return ChartCard(std::move(input), std::move(parsed).Value());
      },
      {{"export", decision_schema(EncodeJson(SelectionSchema().document)), [](auto context, auto payload) {
          return ChartDecision(std::move(context), std::move(payload), true);
        }},
       {"dismiss", decision_schema({{"type", "null"}}), [](auto context, auto payload) {
          return ChartDecision(std::move(context), std::move(payload), false);
        }}}});
  if (!registered.Succeeded()) return registered;
  const auto chart_schema = EncodeJson(ChartSchema().document);
  registered = runtime->RegisterTool({
      .id = PublishReportChart, .version = 1, .description = "Show the validated report data as a chart",
      .input_schema = {DecodeJson(Json{{"type", "object"}, {"additionalProperties", false},
          {"required", {"chart", "interaction"}}, {"properties", {{"chart", chart_schema},
              {"interaction", {{"type", "string"}, {"minLength", 1}}}}}})}, .effect = kit::ToolEffect::Read,
      .prepare = [](kit::ToolPrepareContext, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
        co_return Result<kit::PreparedChange>::Success({.execution_data = std::move(request.arguments)});
      },
      .execute = [](kit::ToolContext, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        const auto fields = EncodeJson(call.change.execution_data);
        auto chart = ReadChart(fields.at("chart"));
        if (!chart.Succeeded()) co_return Result<kit::ToolExecutionResult>::Failure(chart.Error());
        auto fallback = chart.Value().title;
        for (const auto& category : chart.Value().categories) fallback += "\n" + category.label + "  " + Number(category.value);
        co_return Result<kit::ToolExecutionResult>::Success({.outcome = kit::ToolOutcome::Succeeded,
            .blocks = {{{}, kit::CustomBlock{ReportChart, DecodeJson(fields.at("chart")), std::move(fallback),
                {{fields.at("interaction").get<std::string>()}}}}}});
      }});
  if (!registered.Succeeded()) return registered;
  auto export_schema = EncodeJson(SelectionSchema().document);
  export_schema["required"].push_back("chart");
  export_schema["properties"]["chart"] = chart_schema;
  return runtime->RegisterTool({
      .id = ExportReportChart, .version = 1, .description = "Write the approved category selection to CSV",
      .input_schema = {DecodeJson(export_schema)}, .effect = kit::ToolEffect::WriteResource,
      .prepare = [exports](kit::ToolPrepareContext, kit::ToolRequest request) -> kit::Async<kit::PreparedChange> {
        auto output = ReadExport(exports, request.arguments);
        if (!output.Succeeded()) co_return Result<kit::PreparedChange>::Failure(output.Error());
        co_return Result<kit::PreparedChange>::Success({
            .preview = {.title = "Export report chart", .summary = output.Value().selection},
            .targets = {{{output.Value().target.Path()}, {}}},
            .execution_data = std::move(request.arguments)});
      },
      .execute = [exports](kit::ToolContext context, kit::PreparedToolCall call) -> kit::Async<kit::ToolExecutionResult> {
        auto output = ReadExport(exports, call.change.execution_data);
        if (!output.Succeeded()) co_return Result<kit::ToolExecutionResult>::Failure(output.Error());
        const auto grants = context.Resources();
        if (grants.size() != 1 || grants.front().mode != kit::ResourceAccessMode::Write ||
            grants.front().resource.value != output.Value().target.Path()) {
          co_return Fail<kit::ToolExecutionResult>(kit::ErrorCode::PermissionDenied, "CSV target does not match its approved resource");
        }
        co_return co_await RunWorker(WriteCsv, std::move(output).Value(), context.StopToken());
      }});
}

kit::ToolResourceResolver ReportChartResources(File exports) {
  return [exports](kit::ToolInvocation invocation) -> kit::Async<std::vector<kit::ResourceAccess>> {
    using Grants = Result<std::vector<kit::ResourceAccess>>;
    if (invocation.phase == kit::ToolAccessPhase::Prepare) co_return Grants::Success({});
    auto output = ReadExport(exports, invocation.arguments);
    if (!output.Succeeded()) co_return Grants::Failure(output.Error());
    const auto& target = output.Value().target;
    if (invocation.prepared_targets.size() != 1 || invocation.prepared_targets.front().target.value != target.Path())
      co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::PermissionDenied, "CSV target does not match its approved plan");
    auto info = co_await target.StatAsync();
    if (info.Succeeded()) co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::Conflict, "Export target already exists");
    if (info.Error().code != IoErrorCode::NotFound)
      co_return Fail<std::vector<kit::ResourceAccess>>(kit::ErrorCode::ProviderFailure, "CSV target could not be checked");
    co_return Grants::Success({{{target.Path()}, {}, kit::ResourceAccessMode::Write, {}}});
  };
}

kit::Async<kit::RunCompletion> RunReportChart(kit::RunContext run, const kit::RunInput& input,
    std::optional<kit::RunCheckpoint> checkpoint) {
  kit::JsonValue data;
  if (checkpoint) {
    data = std::move(checkpoint->state);
  } else {
    auto drafted = co_await run.CompleteAsync({
        .instruction = "Create a pie chart from the category names and values supplied in this report. Preserve the values "
            "and their order. Use the report title when supplied.\n" + input.input.text, .output_schema = ChartSchema()});
    if (!drafted.Succeeded()) co_return Completion::Failure(drafted.Error());
    data = std::move(*drafted.Value().structured);
    auto validated = ReadChart(EncodeJson(data));
    if (!validated.Succeeded()) co_return Completion::Failure(validated.Error());
    auto saved = co_await run.SaveCheckpointAsync({.stage = std::string(ReportChartStage), .schema_version = 1, .state = data});
    if (!saved.Succeeded()) co_return Completion::Failure(saved.Error());
  }
  const auto command = [&](std::string_view step) {
    return kit::CommandId{"report-chart:" + run.Id().value + "/" + std::string(step)};
  };
  auto form = co_await run.RequestInputAsync(command("form"), {.title = "Chart export", .schema = SelectionSchema()});
  if (!form.Succeeded()) co_return Completion::Failure(form.Error());
  const auto binding = std::get<kit::InputRequested>(form.Value().outcome);
  auto published = co_await run.StartToolAsync(command("publish"), {PublishReportChart,
      DecodeJson(Json{{"chart", EncodeJson(data)}, {"interaction", binding.id.value}})});
  if (!published.Succeeded()) co_return Completion::Failure(published.Error());
  auto displayed = co_await run.WaitForToolAsync(std::get<kit::ToolCallAccepted>(published.Value().outcome).call);
  if (!displayed.Succeeded()) co_return Completion::Failure(displayed.Error());
  if (displayed.Value().outcome != kit::ToolOutcome::Succeeded) co_return Completion::Failure(*displayed.Value().reason);
  auto answer = co_await run.WaitForInputAsync(binding.id);
  if (!answer.Succeeded()) co_return Completion::Failure(answer.Error());
  if (answer.Value().state == kit::InteractionState::Cancelled) co_return Completion::Success({});
  if (answer.Value().state != kit::InteractionState::Submitted || !answer.Value().value)
    co_return Fail<kit::RunCompletion>(kit::ErrorCode::AlreadyClosed, "Chart export form closed");
  auto selection = EncodeJson(*answer.Value().value);
  selection["chart"] = EncodeJson(data);
  auto export_call = co_await run.StartToolAsync(command("export"), {ExportReportChart, DecodeJson(selection)});
  if (!export_call.Succeeded()) co_return Completion::Failure(export_call.Error());
  auto exported = co_await run.WaitForToolAsync(std::get<kit::ToolCallAccepted>(export_call.Value().outcome).call);
  if (!exported.Succeeded()) co_return Completion::Failure(exported.Error());
  if (exported.Value().outcome != kit::ToolOutcome::Succeeded) co_return Completion::Failure(*exported.Value().reason);
  co_return Completion::Success({});
}

} // namespace agentkit_preview
