#include "BrowserScreen.h"

#include <Gfx.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>

#include "core/Settings.h"
#include "core/UiList.h"
#include "core/UiText.h"
#include "core/fontIds.h"
#include "screens/UpdateScreen.h"

namespace {
bool hasExt(const char* name, const char* ext) {
  const size_t n = strlen(name);
  const size_t e = strlen(ext);
  if (n < e) {
    return false;
  }
  for (size_t i = 0; i < e; ++i) {
    char a = name[n - e + i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    if (a != ext[i]) {
      return false;
    }
  }
  return true;
}

std::string joinPath(const char* dir, const char* name) {
  if (!dir || dir[0] == '\0' || (dir[0] == '/' && dir[1] == '\0')) {
    std::string out = "/";
    out += name ? name : "";
    return out;
  }
  std::string out = dir;
  if (out.back() != '/') {
    out += '/';
  }
  out += name ? name : "";
  return out;
}
}  // namespace

BrowserScreen::BrowserScreen(Gfx& gfx, MappedInput& input, const char* initialPath, const Mode mode)
    : Screen(mode == Mode::Firmware ? "Firmware" : "Browser", gfx, input),
      path(initialPath && initialPath[0] ? initialPath : "/"), mode(mode) {}

void BrowserScreen::load() {
  entries.clear();
  entries.reserve(64);
  HalFile root = Storage.open(path.c_str());
  if (!root || !root.isDirectory()) {
    LOG_ERR("DIR", "Cannot open %s", path.c_str());
    return;
  }
  char name[HalFile::kMaxNameBytes];
  for (HalFile file = root.openNextFile(); file; file = root.openNextFile()) {
    if (file.getName(name, sizeof(name)) == 0) {
      continue;
    }
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      continue;
    }
    if (file.isDirectory()) {
      std::string row = name;
      row += '/';
      entries.push_back(std::move(row));
    } else if (mode == Mode::Firmware ? hasExt(name, ".bin")
                                     : hasExt(name, ".xtch")) {
      entries.emplace_back(name);
    }
  }
  std::sort(entries.begin(), entries.end());
  if (index >= static_cast<int>(entries.size())) {
    index = 0;
  }
  window = 0;
  LOG_INF("DIR", "%s (%u items)", path.c_str(), static_cast<unsigned>(entries.size()));
}

void BrowserScreen::onEnter() {
  Screen::onEnter();
  load();
  requestUpdate();
}

void BrowserScreen::onResume() {
  Screen::onResume();
  load();
}

void BrowserScreen::goUp() {
  if (path == "/") {
    finish();
    return;
  }
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos || slash == 0) {
    path = "/";
  } else {
    path.resize(slash);
  }
  index = 0;
  load();
  requestUpdate();
}

void BrowserScreen::activate() {
  if (entries.empty()) {
    return;
  }
  const std::string& name = entries[static_cast<size_t>(index)];
  if (!name.empty() && name.back() == '/') {
    path = joinPath(path.c_str(), name.substr(0, name.size() - 1).c_str());
    LOG_DBG("DIR", "Enter %s", path.c_str());
    index = 0;
    load();
    requestUpdate();
    return;
  }
  const std::string next = joinPath(path.c_str(), name.c_str());
  LOG_INF("DIR", "Open %s", next.c_str());
  if (mode == Mode::Firmware) {
    auto screen = makeUniqueNoThrow<UpdateScreen>(gfx, input, next.c_str());
    if (!screen) {
      LOG_ERR("DIR", "OOM: update");
      return;
    }
    push(std::move(screen));
    return;
  }
  // Drop the listing so the reader can claim the heap. Restore it if the
  // push fails, or the browser comes back empty.
  entries.clear();
  entries.shrink_to_fit();
  if (!goToReader(next.c_str())) {
    LOG_ERR("DIR", "OOM: reader");
    load();
    requestUpdate();
  }
}

void BrowserScreen::loop() {
  if (input.wasReleased(MappedInput::Button::Back)) {
    goUp();
    return;
  }
  if (ui::statusMinuteChanged(shownMinute)) {
    requestUpdate();
  }
  const int count = static_cast<int>(entries.size());
  if (ui::applyDelta(index, input.consumeNavigationDelta(), count)) {
    requestUpdate();
  } else if (count > 0 && input.wasReleased(MappedInput::Button::Confirm)) {
    activate();
  }
}

void BrowserScreen::render() {
  gfx.clear(false);
  ui::drawStatusBar(gfx, shownMinute);

  const int pathY = 8 + gfx.lineHeight(FONT_UI_BOLD) + 6;
  gfx.drawText(FONT_UI_BOLD, 12, pathY, path.c_str());

  const int rowH = gfx.lineHeight(FONT_UI) + 8;
  const int top = pathY + gfx.lineHeight(FONT_UI_BOLD) + 8;
  const int bottomPad = mode == Mode::Firmware ? 52 : 24;
  const int rows = (gfx.height() - top - bottomPad) / rowH;
  ui::followWindow(window, index, rows);
  if (entries.empty()) {
    gfx.drawCenteredText(FONT_UI, gfx.height() / 2, mode == Mode::Firmware ? uiText::noBinFiles : uiText::noBooks);
  } else {
    const int last = std::min(window + rows, static_cast<int>(entries.size()));
    for (int i = window; i < last; ++i) {
      ui::drawRow(gfx, top + (i - window) * rowH, rowH, entries[static_cast<size_t>(i)].c_str(), i == index);
    }
  }
  if (mode == Mode::Firmware) {
    gfx.drawCenteredText(FONT_UI, gfx.height() - 28, uiText::backToCancel);
  }
  presentUi();
}
