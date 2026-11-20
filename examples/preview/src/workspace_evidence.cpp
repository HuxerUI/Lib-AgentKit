#include "workspace_evidence.h"

#include "preview_json.h"
#include <algorithm>

namespace agentkit_preview {
namespace {

namespace kit = huxerui::agentkit;
using huxerui::File;
using huxerui::AsyncInputStream;
constexpr std::string_view prefix = "workspace-evidence:";
constexpr std::string_view screenshot_prefix = "workspace-screenshot:";
constexpr std::size_t max_screenshot_bytes = 8 * 1024 * 1024;
constexpr std::size_t max_evidence_bytes = 2 * 1024 * 1024;

template<class T>
kit::AgentResult<T> Missing(std::string message = "Workspace evidence is unavailable") {
  return kit::AgentResult<T>::Failure({.code = kit::ErrorCode::NotFound, .message = std::move(message)});
}

bool ValidHandle(std::string_view handle) {
  return !handle.empty() && handle.size() <= 128 && std::ranges::all_of(handle, [](char ch) {
        return (ch >= '0' && ch <= '9') || ch == '-';
      });
}

std::optional<File> EvidenceFile(const File& directory, std::string_view handle) {
  if (!ValidHandle(handle)) return std::nullopt;
  return directory.Child(std::string(handle) + ".json");
}

std::optional<File> FileFor(const File& directory, const kit::ArtifactResolveRequest& request) {
  if (IsVerificationScreenshot(request.artifact) &&
      request.artifact.id.value.substr(screenshot_prefix.size()) == request.storage_handle) {
    return directory.Child(request.storage_handle + ".png");
  }
  if (request.artifact.version != 1 || request.artifact.id.value != std::string(prefix) + request.storage_handle) return {};
  return EvidenceFile(directory, request.storage_handle);
}

bool HasCompletePngData(std::span<const std::byte> bytes) {
  bool image_data = false;
  for (std::size_t offset = 8; offset + 12 <= bytes.size();) {
    std::size_t length = 0;
    for (std::size_t i = 0; i < 4; ++i) length = (length << 8) + std::to_integer<unsigned>(bytes[offset + i]);
    if (length > bytes.size() - offset - 12) return false;
    const std::string_view type(reinterpret_cast<const char*>(bytes.data() + offset + 4), 4);
    if (offset == 8 && (type != "IHDR" || length != 13)) return false;
    image_data = image_data || (type == "IDAT" && length != 0);
    offset += 12 + length;
    if (type == "IEND") return length == 0 && offset == bytes.size() && image_data;
  }
  return false;
}

} // namespace

kit::AgentResult<huxerui::ImageAsset> DecodeVerificationScreenshot(huxerui::Bytes bytes) {
  using Result = kit::AgentResult<huxerui::ImageAsset>;
  if (bytes.size() <= max_screenshot_bytes && HasCompletePngData(bytes)) {
    try {
      auto image = huxerui::ImageAsset::FromEncoded(std::move(bytes));
      if (image.Format() == huxerui::ImageFormat::Png && image.PixelWidth() && image.PixelHeight() &&
          image.PixelWidth() <= 4096 && image.PixelHeight() <= 4096) {
        return Result::Success(std::move(image));
      }
    } catch (const std::invalid_argument&) {
    }
  }
  return Result::Failure({.code = kit::ErrorCode::InvalidArgument, .message = "Expected a valid PNG screenshot up to 4096 pixels per side"});
}

File VerificationCaptureFile(File directory, const kit::ToolCallId& call) {
  return directory.Child(call.value + ".capture.png");
}

bool IsVerificationScreenshot(const kit::ArtifactRef& ref) {
  return ref.version == 1 && ref.id.value.starts_with(screenshot_prefix) &&
      ValidHandle(std::string_view(ref.id.value).substr(screenshot_prefix.size()));
}

kit::AgentResult<kit::ArtifactDraft> StageVerificationScreenshot(const File& directory, const kit::ToolCallId& call) {
  using Result = kit::AgentResult<kit::ArtifactDraft>;
  if (!ValidHandle(call.value)) return Missing<kit::ArtifactDraft>();
  auto captured = VerificationCaptureFile(directory, call);
  std::error_code error;
  if (std::filesystem::symlink_status(std::filesystem::u8path(captured.Path()), error).type() !=
      std::filesystem::file_type::regular || error) {
    return Missing<kit::ArtifactDraft>("The workspace tests did not produce the required implementation screenshot");
  }
  auto info = captured.Stat();
  if (!info.Succeeded() || info.Value().size > max_screenshot_bytes) {
    return Missing<kit::ArtifactDraft>("The implementation screenshot exceeds the 8 MiB limit");
  }
  auto bytes = captured.ReadBytes();
  if (!bytes.Succeeded() || bytes.Value().size() > max_screenshot_bytes) {
    return Missing<kit::ArtifactDraft>("The implementation screenshot could not be read");
  }
  auto image = DecodeVerificationScreenshot(std::move(bytes).Value());
  if (!image.Succeeded()) return Result::Failure(image.Error());
  auto dest = directory.Child(call.value + ".png");
  auto output = dest.OpenRandomAccess(huxerui::FileAccessMode::WriteOnly, huxerui::FileOpenMode::CreateNew);
  if (!output.Succeeded()) return Missing<kit::ArtifactDraft>("The screenshot could not be staged for this test call");
  const auto payload = image.Value().EncodedBytes();
  const auto written = output.Value().WriteAt(0, payload);
  const auto closed = output.Value().Close();
  if (!written.Succeeded() || !closed.Succeeded()) {
    static_cast<void>(dest.Delete());
    return Missing<kit::ArtifactDraft>("The screenshot could not be saved");
  }
  return Result::Success({.id = kit::ArtifactId{std::string(screenshot_prefix) + call.value},
      .type = std::string(VerificationScreenshot), .name = "Implementation screenshot",
      .storage_handle = call.value, .size_bytes = payload.size()});
}

kit::Async<huxerui::ImageAsset> LoadVerificationScreenshot(
    std::shared_ptr<kit::AgentRuntime> runtime, kit::ArtifactRef ref) {
  if (!runtime || !IsVerificationScreenshot(ref)) co_return Missing<huxerui::ImageAsset>();
  auto opened = co_await runtime->Artifacts().OpenAsync(std::move(ref));
  if (!opened.Succeeded()) co_return kit::AgentResult<huxerui::ImageAsset>::Failure(opened.Error());
  auto stream = std::move(opened).Value();
  huxerui::Bytes contents;
  while (true) {
    auto chunk = co_await stream.ReadAsync(64 * 1024);
    if (!chunk.Succeeded() || chunk.Value().size() > max_screenshot_bytes - contents.size()) {
      co_return Missing<huxerui::ImageAsset>("The implementation screenshot could not be read");
    }
    if (chunk.Value().empty()) break;
    contents.insert(contents.end(), chunk.Value().begin(), chunk.Value().end());
  }
  co_return DecodeVerificationScreenshot(std::move(contents));
}

kit::ArtifactStore WithWorkspaceEvidence(kit::ArtifactStore fallback, File directory) {
  auto preview = [fallback = std::move(fallback.preview), directory](
      kit::ArtifactResolveRequest request) -> kit::Async<kit::ArtifactPreview> {
        const auto screenshot = IsVerificationScreenshot(request.artifact);
        if (!screenshot && !request.artifact.id.value.starts_with(prefix)) co_return co_await fallback(std::move(request));
        auto file = FileFor(directory, request);
        if (!file) co_return Missing<kit::ArtifactPreview>();
        auto size = co_await file->StatAsync();
        if (!size.Succeeded() || size.Value().size > (screenshot ? max_screenshot_bytes : max_evidence_bytes))
          co_return Missing<kit::ArtifactPreview>();
        if (screenshot) co_return kit::AgentResult<kit::ArtifactPreview>::Success({
            .artifact = request.artifact, .summary = "Implementation screenshot"});
        auto text = co_await file->ReadStringAsync();
        if (!text.Succeeded()) co_return Missing<kit::ArtifactPreview>();
        auto parsed = nlohmann::json::parse(text.Value(), nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) co_return Missing<kit::ArtifactPreview>();
        co_return kit::AgentResult<kit::ArtifactPreview>::Success({.artifact = request.artifact, .structured = DecodeJson(parsed),
            .summary = parsed.value("summary", "Workspace evidence")});
  };
  return {
      .preview = preview,
      .open = [fallback = fallback.open, directory](kit::ArtifactResolveRequest request) -> kit::Async<AsyncInputStream> {
        if (!request.artifact.id.value.starts_with(prefix) && !IsVerificationScreenshot(request.artifact))
          co_return co_await fallback(std::move(request));
        auto file = FileFor(directory, request);
        if (!file) co_return Missing<AsyncInputStream>();
        auto opened = co_await file->OpenReadAsync();
        if (!opened.Succeeded()) co_return Missing<AsyncInputStream>();
        co_return kit::AgentResult<AsyncInputStream>::Success(std::move(opened).Value());
      },
      .selection = [preview, fallback = std::move(fallback.selection)](
          kit::ArtifactSelectionRequest request) -> kit::Async<std::string> {
        using Result = kit::AgentResult<std::string>;
        if (!request.resource.artifact.id.value.starts_with(prefix) && !IsVerificationScreenshot(request.resource.artifact)) {
          if (fallback) co_return co_await fallback(std::move(request));
          co_return Result::Failure({.code = kit::ErrorCode::Unsupported, .message = "The artifact has no text selection adapter"});
        }
        auto item = co_await preview(std::move(request.resource));
        if (!item.Succeeded()) co_return Result::Failure(item.Error());
        const auto* object = item.Value().structured ? std::get_if<kit::JsonValue::Object>(&item.Value().structured->value) : nullptr;
        const auto field = [object](std::string_view name) -> const std::string* {
          if (!object) return nullptr;
          const auto found = object->find(std::string(name));
          return found == object->end() ? nullptr : std::get_if<std::string>(&found->second.value);
        };
        const auto* kind = field("kind");
        const auto* path = field("path");
        const auto* text = field("diff");
        if (!kind || *kind != CodePatchPreview || !path || !text)
          co_return Result::Failure({.code = kit::ErrorCode::Unsupported, .message = "This workspace artifact has no text selection"});
        if (!request.parts.empty() && (request.parts.size() != 1 || request.parts.front().value != *path))
          co_return Missing<std::string>("The selected file is unavailable in the patch");
        if (text->size() > request.byte_limit)
          co_return Result::Failure({.code = kit::ErrorCode::BudgetExceeded, .message = "The selected diff exceeds its byte limit"});
        co_return Result::Success(*text);
      }};
}

kit::Async<kit::ArtifactDraft> StageWorkspaceEvidence(File directory, kit::ToolCallId call, std::string type,
    std::string name, kit::JsonValue contents) {
  auto target = EvidenceFile(directory, call.value);
  const auto data = EncodeJson(contents).dump();
  if (data.size() > max_evidence_bytes || !target || !co_await directory.CreateDirectoriesAsync() ||
      !co_await target->WriteStringAsync(data)) {
    co_return kit::AgentResult<kit::ArtifactDraft>::Failure(
        {.code = kit::ErrorCode::ProviderFailure, .message = "Could not save the workspace evidence"});
  }
  co_return kit::AgentResult<kit::ArtifactDraft>::Success({
      .id = kit::ArtifactId{std::string(prefix) + call.value}, .type = std::move(type), .name = std::move(name),
      .storage_handle = std::move(call.value), .size_bytes = data.size()});
}

} // namespace agentkit_preview
