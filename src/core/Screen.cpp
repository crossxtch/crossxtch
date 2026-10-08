#include "core/Screen.h"

#include <Gfx.h>
#include <Logging.h>

#include "core/ScreenManager.h"
#include "core/fontIds.h"

void Screen::onEnter() {
  LOG_DBG("SCR", "Enter %s", name);
  needsCleanRefresh = true;
}

void Screen::onExit() { LOG_DBG("SCR", "Exit %s", name); }

void Screen::onResume() { needsCleanRefresh = true; }

void Screen::presentUi() {
  gfx.present(needsCleanRefresh ? HalDisplay::HALF_REFRESH : HalDisplay::FAST_REFRESH);
  needsCleanRefresh = false;
}

void Screen::setNotice(const char* text) {
  notice = text;
  requestUpdate();
}

void Screen::drawNotice(const int y) {
  if (!notice || notice[0] == '\0') {
    return;
  }
  gfx.drawCenteredText(FONT_UI, y >= 0 ? y : gfx.height() - 28, notice);
}

void Screen::requestUpdate() { screenManager.requestUpdate(); }
void Screen::finish(const int levels) { screenManager.pop(levels); }
void Screen::push(std::unique_ptr<Screen> screen) { screenManager.push(std::move(screen)); }
void Screen::goHome() { screenManager.goHome(); }
bool Screen::goToReader(const char* path) { return screenManager.goToReader(path); }
bool Screen::goToBrowser() { return screenManager.goToBrowser(); }
bool Screen::goToSettings() { return screenManager.goToSettings(); }
void Screen::goToFirmwareUpdate(const bool recovery) { screenManager.goToFirmwareUpdate(recovery); }
bool Screen::goToWifiFileTransfer() { return screenManager.goToWifiFileTransfer(); }
