#include <Arduino.h>
#include <BoardConfig.h>
#include <Gfx.h>
#include <HalClock.h>
#include <HalDisplay.h>
#include <HalFrontlight.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <HalSystem.h>
#include <HalTiltSensor.h>
#include <Logging.h>
#include <WiFi.h>
#include <Xtch.h>
#include <builtinFonts/all.h>

#include "core/ScreenManager.h"
#include "core/MappedInput.h"
#include "core/Power.h"
#include "core/Settings.h"
#include "core/UiText.h"
#include "core/fontIds.h"
#include "network/WifiCredentialStore.h"

#ifndef CROSSXTCH_VERSION
#define CROSSXTCH_VERSION "dev"
#endif

Gfx gfx(display);
MappedInput mappedInput(gpio);
ScreenManager screenManager(gfx, mappedInput);

EpdFont ui12RegularFont(&ubuntu_12_medium);
EpdFont ui12BoldFont(&ubuntu_12_bold);
EpdFont uiJpFont(&jp_12);
EpdFontFamily ui12Family(&ui12RegularFont, &ui12BoldFont);
EpdFontFamily ui12BoldFamily(&ui12BoldFont, &ui12BoldFont);
EpdFontFamily uiJpFamily(&uiJpFont);

static const char* wakeupName(HalGPIO::WakeupReason reason) {
  switch (reason) {
    case HalGPIO::WakeupReason::PowerButton:
      return "power";
    case HalGPIO::WakeupReason::AfterFlash:
      return "flash";
    case HalGPIO::WakeupReason::AfterUSBPower:
      return "usb";
    default:
      return "other";
  }
}

static void setupDisplayAndFonts() {
  display.begin();
  MappedInput::installBusyWaitPoll();
  gfx.begin();
  gfx.insertFont(FONT_UI, &ui12Family);
  gfx.insertFont(FONT_UI_BOLD, &ui12BoldFamily);
  gfx.setFallbackFont(&uiJpFamily);
}

void setup() {
#ifdef ENABLE_SERIAL_LOG
  logSerial.begin(115200);
#if LOG_SERIAL_HAS_TX_TIMEOUT
  logSerial.setTxTimeoutMs(1);
#endif
#endif

  WiFi.mode(WIFI_OFF);

  HalSystem::begin();
  gpio.begin();
  // Page buffer first, framebuffer second: see XtchBook::reserveScratchBuffers.
  // WifiSession::end() restarts so this runs again on a clean heap.
  XtchBook::reserveScratchBuffers(BoardConfig::ACTIVE.displayWidth, BoardConfig::ACTIVE.displayHeight);
  powerManager.begin();
  halTiltSensor.begin();
  halClock.begin();

  const auto wakeupReason = gpio.getWakeupReason();
  LOG_INF("MAIN", "crossxtch " CROSSXTCH_VERSION " board=%s gpio=%s wake=%s bat=%u%% panic=%d",
          BoardConfig::ACTIVE.name, gpio.deviceIsX3() ? "x3" : "x4", wakeupName(wakeupReason),
          static_cast<unsigned>(powerManager.getBatteryPercentage()), HalSystem::isRebootFromPanic() ? 1 : 0);

  if (!Storage.begin()) {
    LOG_ERR("MAIN", "SD init failed");
    setupDisplayAndFonts();
    screenManager.showMessage(uiText::sdCardError);
    return;
  }
  LOG_INF("MAIN", "SD ready");

  HalSystem::checkPanic();
  settings.load();
  uiText::apply();
  wifiCredentials.load();
  Frontlight.begin(0, 0, false);

  bool recoveryFirmware = false;
  switch (wakeupReason) {
    case HalGPIO::WakeupReason::PowerButton:
      if (!gpio.verifyPowerButtonWakeup(800, true)) {
        LOG_INF("MAIN", "Power press too short, sleeping");
        powerManager.startDeepSleep(gpio);
      } else {
        LOG_INF("MAIN", "Power wake, waiting for button release");
        power::noteWakeHold();
        const unsigned long settleStart = millis();
        while (millis() - settleStart < 500) {
          gpio.update();
          delay(10);
        }
        if (gpio.isPressed(HalGPIO::BTN_UP)) {
          recoveryFirmware = true;
          LOG_INF("MAIN", "Recovery firmware mode (UP + POWER)");
        }
      }
      break;
    case HalGPIO::WakeupReason::AfterUSBPower:
      LOG_INF("MAIN", "USB-power wake, sleeping");
      powerManager.startDeepSleep(gpio);
      break;
    default:
      break;
  }

  setupDisplayAndFonts();
  display.setInverted(settings.nightMode != 0);
  LOG_INF("MAIN", "Display up night=%u", settings.nightMode);

  if (recoveryFirmware) {
    screenManager.goToFirmwareUpdate(true);
    return;
  }

  if (!settings.languageChosen()) {
    LOG_INF("MAIN", "Language not chosen");
    screenManager.goToLanguageSetup();
    return;
  }

  if (wakeupReason == HalGPIO::WakeupReason::PowerButton && isXtchPath(settings.lastBookPath) &&
      Storage.exists(settings.lastBookPath)) {
    LOG_INF("MAIN", "Resume %s", settings.lastBookPath);
    screenManager.goToReader(settings.lastBookPath);
  } else {
    LOG_INF("MAIN", "Go home (last='%s')", settings.lastBookPath);
    screenManager.goHome();
  }
}

void loop() {
  mappedInput.update();
  if (power::consumeWakeRelease(gpio)) {
    return;
  }
  power::noteUserActivity(gpio);
  if (power::maybeSleep(gpio, settings)) {
    return;
  }
  if (power::maybeToggleTiltLock(gpio)) {
    return;
  }
  const bool tiltOn = settings.tiltPageTurn != 0 && !power::tiltLocked();
  halTiltSensor.update(tiltOn, screenManager.isReader());
  screenManager.loop();
  power::idleDelay();
}
