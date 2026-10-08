#include "SettingsScreen.h"

#include <Gfx.h>
#include <HalDisplay.h>
#include <HalTiltSensor.h>
#include <Logging.h>

#include <cstdio>

#include <Memory.h>

#include "core/Settings.h"
#include "core/UiList.h"
#include "core/UiText.h"
#include "core/fontIds.h"
#include "screens/LanguageScreen.h"

#ifndef CROSSXTCH_VERSION
#define CROSSXTCH_VERSION "dev"
#endif

namespace {
// Tilt page-turn is only offered on boards with the QMI8658 IMU (X3).
bool hasTilt() { return halTiltSensor.isAvailable(); }
constexpr int kLanguage = 0;
constexpr int kSleep = 1;
constexpr int kRefresh = 2;
constexpr int kNight = 3;
int itemCount() { return hasTilt() ? 7 : 5; }
int tiltIndex() { return 4; }
int gyroIndex() { return 5; }
int firmwareIndex() { return hasTilt() ? 6 : 4; }

void formatTrueSleep(char* out, size_t outSize) {
  if (settings.trueSleepMinutes == Settings::kSleepNone) {
    snprintf(out, outSize, "%s", uiText::powerOffNone);
  } else {
    snprintf(out, outSize, uiText::powerOffMin, settings.trueSleepMinutes);
  }
}

void formatGyroAutoOff(char* out, size_t outSize) {
  if (settings.gyroAutoOffSeconds == 0) {
    snprintf(out, outSize, "%s", uiText::gyroAutoOffNone);
  } else {
    snprintf(out, outSize, uiText::gyroAutoOffSec, settings.gyroAutoOffSeconds);
  }
}

void bumpGyroAutoOff() {
  switch (settings.gyroAutoOffSeconds) {
    case 0:
      settings.gyroAutoOffSeconds = 30;
      break;
    case 30:
      settings.gyroAutoOffSeconds = 45;
      break;
    case 45:
      settings.gyroAutoOffSeconds = 60;
      break;
    default:
      settings.gyroAutoOffSeconds = 0;
      break;
  }
}

void bumpTrueSleep() {
  switch (settings.trueSleepMinutes) {
    case Settings::kSleepNone:
      settings.trueSleepMinutes = Settings::kSleep5Min;
      break;
    case Settings::kSleep5Min:
      settings.trueSleepMinutes = Settings::kSleep10Min;
      break;
    case Settings::kSleep10Min:
      settings.trueSleepMinutes = Settings::kSleep15Min;
      break;
    default:
      settings.trueSleepMinutes = Settings::kSleepNone;
      break;
  }
}

char refreshBuf[48];

const char* refreshLabel() {
  if (settings.refreshEveryNPages == 1) {
    return uiText::refreshEveryPage;
  }
  snprintf(refreshBuf, sizeof(refreshBuf), uiText::refreshEveryN, settings.refreshEveryNPages);
  return refreshBuf;
}

void bumpRefresh() {
  switch (settings.refreshEveryNPages) {
    case 1:
      settings.refreshEveryNPages = 5;
      break;
    case 5:
      settings.refreshEveryNPages = 10;
      break;
    case 10:
      settings.refreshEveryNPages = 15;
      break;
    case 15:
      settings.refreshEveryNPages = 20;
      break;
    default:
      settings.refreshEveryNPages = 1;
      break;
  }
}
}  // namespace

void SettingsScreen::loop() {
  if (input.wasReleased(MappedInput::Button::Back)) {
    settings.save();
    finish();
    return;
  }
  if (ui::statusMinuteChanged(shownMinute)) {
    requestUpdate();
  }
  if (ui::applyDelta(index, input.consumeNavigationDelta(), itemCount())) {
    requestUpdate();
  } else if (input.wasReleased(MappedInput::Button::Confirm)) {
    if (index == kLanguage) {
      auto screen = makeUniqueNoThrow<LanguageScreen>(gfx, input, false);
      if (!screen) {
        LOG_ERR("SET", "OOM: language");
        return;
      }
      push(std::move(screen));
      return;
    } else if (index == kSleep) {
      bumpTrueSleep();
      char sleepLog[48];
      formatTrueSleep(sleepLog, sizeof(sleepLog));
      LOG_INF("SET", "%s", sleepLog);
    } else if (index == kRefresh) {
      bumpRefresh();
      LOG_INF("SET", "%s", refreshLabel());
    } else if (index == kNight) {
      settings.nightMode = settings.nightMode ? 0 : 1;
      display.setInverted(settings.nightMode != 0);
      LOG_INF("SET", "Night mode %s", settings.nightMode ? "on" : "off");
    } else if (hasTilt() && index == tiltIndex()) {
      settings.tiltPageTurn = settings.tiltPageTurn ? 0 : 1;
      LOG_INF("SET", "Tilt page turn %s", settings.tiltPageTurn ? "on" : "off");
    } else if (hasTilt() && index == gyroIndex()) {
      bumpGyroAutoOff();
      LOG_INF("SET", "Gyro auto-off %u sec", settings.gyroAutoOffSeconds);
    } else if (index == firmwareIndex()) {
      settings.save();
      goToFirmwareUpdate();
      return;
    }
    settings.save();
    requestUpdate();
  }
}

void SettingsScreen::render() {
  gfx.clear(false);
  ui::drawStatusBar(gfx, shownMinute);

  char lang[48];
  char deep[48];
  char night[48];
  char tilt[48];
  char gyro[48];
  snprintf(lang, sizeof(lang), uiText::language, uiText::languageName);
  formatTrueSleep(deep, sizeof(deep));
  snprintf(night, sizeof(night), uiText::nightMode, settings.nightMode ? uiText::on : uiText::off);
  snprintf(tilt, sizeof(tilt), uiText::tiltPageTurn, settings.tiltPageTurn ? uiText::on : uiText::off);
  formatGyroAutoOff(gyro, sizeof(gyro));

  const char* labels[7];
  labels[0] = lang;
  labels[1] = deep;
  labels[2] = refreshLabel();
  labels[3] = night;
  if (hasTilt()) {
    labels[4] = tilt;
    labels[5] = gyro;
    labels[6] = uiText::updateFirmware;
  } else {
    labels[4] = uiText::updateFirmware;
  }

  const int rowH = gfx.lineHeight(FONT_UI) + 10;
  const int startY = 90;
  for (int i = 0; i < itemCount(); ++i) {
    ui::drawMenuRow(gfx, startY + i * rowH, rowH, labels[i], i == index);
  }

  gfx.drawCenteredText(FONT_UI, gfx.height() - 40, "crossxtch " CROSSXTCH_VERSION);
  presentUi();
}
