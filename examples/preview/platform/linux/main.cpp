#include <huxerui/app.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

extern "C" {
void g_set_prgname(const char* prgname);
void g_set_application_name(const char* application_name);
void gtk_window_set_default_icon_name(const char* name);
}

namespace {

bool ContainsPathEntry(std::string_view list, std::string_view entry) {
  std::size_t start = 0;
  while (start <= list.size()) {
    const auto end = list.find(':', start);
    const auto part = end == std::string_view::npos ? list.substr(start) : list.substr(start, end - start);
    if (part == entry) return true;
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return false;
}

void ConfigureLinuxDesktopIdentity() {
  std::error_code ec;
  const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec && !exe.empty()) {
    const auto share_dir = exe.parent_path() / "share";
    const auto icon = share_dir / "icons" / "hicolor" / "scalable" / "apps" / "ganwork.svg";
    if (std::filesystem::exists(icon, ec)) {
      const auto share_str = share_dir.string();
      const char* existing = std::getenv("XDG_DATA_DIRS");
      const std::string_view base = (existing && *existing) ? std::string_view(existing) : "/usr/local/share:/usr/share";
      if (!ContainsPathEntry(base, share_str)) {
        std::string updated = share_str;
        updated.push_back(':');
        updated.append(base);
        ::setenv("XDG_DATA_DIRS", updated.c_str(), 1);
      }
    }
  }
  g_set_prgname("ganwork");
  g_set_application_name("GanWork");
  gtk_window_set_default_icon_name("ganwork");
}

} // namespace

int main() {
  ConfigureLinuxDesktopIdentity();
  return huxerui::RunApplication();
}
