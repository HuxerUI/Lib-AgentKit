#include <app_resources.h>
#include <huxerui/app.h>
#include <huxerui/theme.h>

#include "attachment_imports.h"

#include <algorithm>
#include <array>
#include <iterator>

namespace agentkit_preview {
namespace {

namespace kit = huxerui::agentkit;
using namespace huxerui;

constexpr std::string_view prefix = "preview-attachment:";
constexpr std::size_t max_bytes = 4 << 20;
constexpr std::array<std::pair<std::string_view, std::string_view>, 7> formats{{
    {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"webp", "image/webp"}, {"heic", "image/heic"},
    {"heif", "image/heif"}, {"pdf", "application/pdf"},
}};

template<class T>
kit::AgentResult<T> Fail(kit::ErrorCode code, std::string message) {
  return kit::AgentResult<T>::Failure({.code = code, .message = std::move(message)});
}

bool Supports(kit::ModelCapabilities caps, std::string_view media_type) {
  return media_type.starts_with("image/") ? caps.image_input : caps.file_input;
}

std::string MediaType(const FileReference& file) {
  const auto name = file.Name();
  const auto dot = name.find_last_of('.');
  if (dot != name.npos) {
    auto extension = name.substr(dot + 1);
    for (char& ch : extension) {
      if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    }
    const auto found = std::ranges::find_if(formats, [&](const auto& format) { return format.first == extension; });
    if (found != formats.end()) return std::string(found->second);
  }
  const auto metadata = file.ContentType();
  const auto found = std::ranges::find_if(formats, [&](const auto& format) { return metadata && *metadata == format.second; });
  return found == formats.end() ? std::string{} : std::string(found->second);
}

std::optional<File> PayloadFile(const File& directory, std::string_view handle) {
  if (handle.empty() || handle.size() > 128 || !std::ranges::all_of(handle, [](char ch) {
        return (ch >= '0' && ch <= '9') || ch == '-';
      })) return {};
  return directory.Child(std::string(handle) + ".bin");
}

} // namespace

struct AttachmentImports::Selection {
  kit::AgentChatAttachment attachment;
  std::string media_type;
  Bytes bytes;
  bool writing = false;
  bool saved = false;
};

AttachmentImports::AttachmentImports(File directory) : directory_(std::move(directory)) {}

kit::Async<std::shared_ptr<const kit::AgentChatAttachment>> AttachmentImports::Stage(FileReference source,
    kit::ModelCapabilities capabilities) {
  using Entry = std::shared_ptr<const kit::AgentChatAttachment>;
  const auto media_type = MediaType(source);
  if (media_type.empty() || !Supports(capabilities, media_type))
    co_return Fail<Entry>(kit::ErrorCode::Unsupported, "The selected model does not accept this file type.");
  auto opened = co_await source.OpenReadAsync();
  if (!opened.Succeeded()) co_return Fail<Entry>(kit::ErrorCode::ProviderFailure, "Could not read the selected file.");
  auto stream = std::move(opened).Value();
  Bytes bytes;
  for (;;) {
    auto read = co_await stream.ReadAsync(std::min<std::size_t>(32 * 1024, max_bytes - bytes.size() + 1));
    if (!read.Succeeded()) co_return Fail<Entry>(kit::ErrorCode::ProviderFailure, "Could not read the selected file.");
    if (read.Value().empty()) break;
    if (read.Value().size() > max_bytes - bytes.size())
      co_return Fail<Entry>(kit::ErrorCode::BudgetExceeded, "The selected file exceeds the Preview attachment budget.");
    bytes.insert(bytes.end(), read.Value().begin(), read.Value().end());
  }
  if (bytes.empty()) co_return Fail<Entry>(kit::ErrorCode::InvalidArgument, "The selected file is empty.");
  auto selection = std::make_shared<Selection>(Selection{
      {{std::string(prefix) + kit::NewCommandId().value}, source.Name()}, media_type, std::move(bytes)});
  std::erase_if(drafts_, [](const auto& entry) { return entry.second.expired(); });
  drafts_.emplace(selection->attachment.id, selection);
  co_return kit::AgentResult<Entry>::Success(Entry(selection, &selection->attachment));
}

kit::Async<kit::ArtifactDraft> AttachmentImports::Resolve(kit::ArtifactId id) {
  const auto found = drafts_.find(id);
  auto selected = found == drafts_.end() ? nullptr : found->second.lock();
  if (!selected) co_return Fail<kit::ArtifactDraft>(kit::ErrorCode::NotFound, "The draft attachment is no longer available.");
  if (selected->writing) co_return Fail<kit::ArtifactDraft>(kit::ErrorCode::Busy, "The attachment is being saved.");
  const auto handle = id.value.substr(prefix.size());
  const auto file = PayloadFile(directory_, handle);
  if (!file) co_return Fail<kit::ArtifactDraft>(kit::ErrorCode::InvalidArgument, "Invalid attachment ID.");
  if (!selected->saved) {
    selected->writing = true;
    const auto release = std::unique_ptr<bool, void(*)(bool*)>(&selected->writing, [](bool* writing) { *writing = false; });
    if (!co_await directory_.CreateDirectoriesAsync())
      co_return Fail<kit::ArtifactDraft>(kit::ErrorCode::PersistenceFailure, "Could not open the private attachment directory.");
    const auto prior = co_await file->StatAsync();
    if (prior.Succeeded()) co_return Fail<kit::ArtifactDraft>(kit::ErrorCode::Conflict, "The attachment path is already occupied.");
    if (prior.Error().code != IoErrorCode::NotFound)
      co_return Fail<kit::ArtifactDraft>(kit::ErrorCode::PersistenceFailure, "Could not check the attachment destination.");
    if (!co_await file->WriteBytesAsync(selected->bytes)) {
      static_cast<void>(co_await file->DeleteAsync());
      co_return Fail<kit::ArtifactDraft>(kit::ErrorCode::PersistenceFailure, "Could not save the attachment snapshot.");
    }
    selected->saved = true;
  }
  co_return kit::AgentResult<kit::ArtifactDraft>::Success({.id = std::move(id), .type = selected->media_type,
      .name = selected->attachment.name, .storage_handle = handle, .size_bytes = selected->bytes.size()});
}

kit::ArtifactStore WithAttachmentImports(kit::ArtifactStore artifacts, std::shared_ptr<AttachmentImports> imports) {
  artifacts.resolve_staged_attachment = [imports](kit::ActionParty actor, kit::SessionId, kit::ArtifactId id)
      -> kit::Async<kit::ArtifactDraft> {
    if (actor.kind != kit::ActionParty::Kind::User)
      co_return Fail<kit::ArtifactDraft>(kit::ErrorCode::PermissionDenied, "An attachment requires a user submission.");
    co_return co_await imports->Resolve(std::move(id));
  };
  artifacts.open = [directory = imports->Directory(), open = std::move(artifacts.open)](kit::ArtifactResolveRequest request)
      -> kit::Async<AsyncInputStream> {
    if (!request.artifact.id.value.starts_with(prefix) && open) co_return co_await open(std::move(request));
    const auto file = PayloadFile(directory, request.storage_handle);
    if (!file || request.artifact.version != 1 || request.artifact.id.value != std::string(prefix) + request.storage_handle)
      co_return Fail<AsyncInputStream>(kit::ErrorCode::NotFound, "Attachment version unavailable.");
    auto stream = co_await file->OpenReadAsync();
    if (!stream.Succeeded()) co_return Fail<AsyncInputStream>(kit::ErrorCode::NotFound, "Attachment snapshot unavailable.");
    co_return kit::AgentResult<AsyncInputStream>::Success(std::move(stream).Value());
  };
  artifacts.preview = [directory = imports->Directory(), preview = std::move(artifacts.preview)](kit::ArtifactResolveRequest request)
      -> kit::Async<kit::ArtifactPreview> {
    if (!request.artifact.id.value.starts_with(prefix) && preview) co_return co_await preview(std::move(request));
    const auto file = PayloadFile(directory, request.storage_handle);
    if (!file || request.artifact.version != 1 || request.artifact.id.value != std::string(prefix) + request.storage_handle)
      co_return Fail<kit::ArtifactPreview>(kit::ErrorCode::NotFound, "Attachment snapshot unavailable.");
    const auto status = co_await file->StatAsync();
    if (!status.Succeeded())
      co_return Fail<kit::ArtifactPreview>(kit::ErrorCode::NotFound, "Attachment snapshot unavailable.");
    co_return kit::AgentResult<kit::ArtifactPreview>::Success({.artifact = request.artifact});
  };
  return artifacts;
}

[[huxerui::composable]]
View ComposerAddMenu(std::shared_ptr<AttachmentImports> imports, State<kit::AgentChatDraftSources> sources,
    kit::ModelCapabilities capabilities, kit::ModelCapabilities image_capabilities, std::function<void(bool)> on_staged,
    std::vector<MenuEntry> actions) {
  const auto picker = UseService<FilePicker>();
  const auto tasks = UseTaskScope();
  const auto menu = UseMenu();
  const auto anchor = UseViewHandle();
  const auto busy = UseState(false);
  const auto error = UseState(std::string{});
  const auto mixed_files = UseString(::app::strings::ui_attachment_model_required);
  const kit::ModelCapabilities accepted{.image_input = capabilities.image_input || image_capabilities.image_input,
      .file_input = capabilities.file_input};
  const auto pick = [picker, tasks, imports, sources, capabilities, image_capabilities, accepted, busy, error, on_staged, mixed_files] {
    if (busy.Get()) return;
    busy = true;
    error = "";
    tasks.Launch([picker, imports, sources, capabilities, image_capabilities, accepted, busy, error, on_staged, mixed_files]() -> Task<void> {
      FilePickerFilter filter{.name = "Model attachments"};
      for (const auto& [ext, mime] : formats) {
        if (!Supports(accepted, mime)) continue;
        filter.extensions.emplace_back(ext);
        if (std::ranges::find(filter.content_types, mime) == filter.content_types.end()) filter.content_types.emplace_back(mime);
      }
      auto selected = co_await picker->OpenFilesAsync(std::move(filter));
      std::vector<std::shared_ptr<const kit::AgentChatAttachment>> attachments;
      bool has_image = false;
      bool has_file = false;
      for (auto& file : selected) {
        const auto type = MediaType(file);
        auto staged = co_await imports->Stage(std::move(file), accepted);
        if (!staged.Succeeded()) { error = staged.Error().message; continue; }
        has_image |= type.starts_with("image/");
        has_file |= !type.starts_with("image/");
        attachments.push_back(std::move(staged).Value());
      }
      busy = false;
      if (attachments.empty()) co_return;
      const bool use_image_model = has_image && image_capabilities.image_input && (!has_file || image_capabilities.file_input);
      if (!use_image_model && ((has_image && !capabilities.image_input) || (has_file && !capabilities.file_input))) {
        error = mixed_files;
        co_return;
      }
      auto draft = sources.Get();
      std::ranges::move(attachments, std::back_inserter(draft.attachments));
      sources = std::move(draft);
      if (on_staged) on_staged(use_image_model);
    });
  };
  auto entries = actions;
  if (imports && picker && picker->CanOpenFiles() && (accepted.image_input || accepted.file_input))
    entries.insert(entries.begin(), MenuItem(::app::images::folder, ::app::strings::ui_attach_files, pick));
  if (entries.empty()) return View{};
  const auto label = ::app::strings::ui_composer_add;
  auto style = UseEnvironment<IconButtonStyle>();
  style.icon_size = 18.0F;
  style.minimum_interactive_size = 34.0F;
  style.state_layer_size = 30.0F;
  auto control = ProvideEnvironment(style, IconButton(::app::images::add, label)
      .OnClick([menu, anchor, entries = std::move(entries)] {
        menu.Show(anchor, entries, {.placement = {AnchorSide::Above, AnchorAlignment::Start}});
      }).With(anchor, Tooltip(label), Enabled(!busy.Get())).Key("composer-add"));
  if (error->empty()) return control;
  return Column{std::move(control), Text(error.Get(), TextRole::Label)}.With(Spacing(4.0F));
}

} // namespace agentkit_preview
