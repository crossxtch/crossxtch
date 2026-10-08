#include "HomeScreen.h"

#include <Gfx.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Xtch.h>

#include "core/Settings.h"
#include "core/UiList.h"
#include "core/UiText.h"
#include "core/fontIds.h"

void HomeScreen::refreshMenu() {
  const bool hasContinue = isXtchPath(settings.lastBookPath) && Storage.exists(settings.lastBookPath);
  itemCount = hasContinue ? 4 : 3;
  if (index >= itemCount) {
    index = 0;
  }
}

void HomeScreen::onEnter() {
  Screen::onEnter();
  refreshMenu();
  LOG_INF("HOME", "Continue %s last='%s'", itemCount == 4 ? "yes" : "no", settings.lastBookPath);
  requestUpdate();
}

void HomeScreen::onResume() {
  Screen::onResume();
  refreshMenu();
}

void HomeScreen::loop() {
  if (ui::statusMinuteChanged(shownMinute)) {
    requestUpdate();
  }
  if (ui::applyDelta(index, input.consumeNavigationDelta(), itemCount)) {
    requestUpdate();
  } else if (input.wasReleased(MappedInput::Button::Confirm)) {
    const bool hasContinue = itemCount == 4;
    // Item order: [Continue?], Browse, File Transfer, Settings.
    int i = index - (hasContinue ? 1 : 0);
    bool opened = false;
    if (hasContinue && index == 0) {
      LOG_DBG("HOME", "Continue");
      opened = goToReader(settings.lastBookPath);
    } else if (i == 0) {
      LOG_DBG("HOME", "Browse");
      opened = goToBrowser();
    } else if (i == 1) {
      LOG_DBG("HOME", "File Transfer");
      opened = goToWifiFileTransfer();
    } else {
      LOG_DBG("HOME", "Settings");
      opened = goToSettings();
    }
    if (!opened) {
      setNotice(uiText::outOfMemory);
    } else {
      notice = nullptr;
    }
  }
}

void HomeScreen::render() {
  gfx.clear(false);
  ui::drawStatusBar(gfx, shownMinute);

  const bool hasContinue = itemCount == 4;
  const char* labels[4];
  int n = 0;
  if (hasContinue) {
    labels[n++] = uiText::continueReading;
  }
  labels[n++] = uiText::browse;
  labels[n++] = uiText::fileTransfer;
  labels[n++] = uiText::settings;

  const int rowH = gfx.lineHeight(FONT_UI) + 10;
  const int startY = 120;
  for (int i = 0; i < n; ++i) {
    ui::drawMenuRow(gfx, startY + i * rowH, rowH, labels[i], i == index);
  }

  if (hasContinue) {
    gfx.drawText(FONT_UI, 24, gfx.height() - 48, settings.lastBookPath);
  }
  drawNotice();
  presentUi();
}
