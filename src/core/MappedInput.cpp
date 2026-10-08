#include "core/MappedInput.h"

#include <HalDisplay.h>
#include <HalTiltSensor.h>

#include "core/Power.h"
#include "core/Settings.h"

uint16_t MappedInput::pendingForwardTaps = 0;
uint16_t MappedInput::pendingBackTaps = 0;
uint16_t MappedInput::pendingConfirmTaps = 0;
uint16_t MappedInput::pendingCancelTaps = 0;

uint8_t MappedInput::map(const Button button) const {
  switch (button) {
    case Button::Back:
      return HalGPIO::BTN_BACK;
    case Button::Confirm:
      return HalGPIO::BTN_CONFIRM;
    case Button::Left:
      return HalGPIO::BTN_LEFT;
    case Button::Right:
      return HalGPIO::BTN_RIGHT;
    case Button::Up:
      return HalGPIO::BTN_UP;
    case Button::Down:
      return HalGPIO::BTN_DOWN;
    case Button::Power:
      return HalGPIO::BTN_POWER;
    case Button::PageBack:
      return HalGPIO::BTN_UP;
    case Button::PageForward:
      return HalGPIO::BTN_DOWN;
  }
  return HalGPIO::BTN_CONFIRM;
}

bool MappedInput::wasPressed(const Button button) const { return gpio.wasPressed(map(button)); }
bool MappedInput::wasReleased(const Button button) const {
  if (button == Button::Confirm) return confirmLatched;
  if (button == Button::Back) return backLatched;
  return gpio.wasReleased(map(button));
}
bool MappedInput::isPressed(const Button button) const { return gpio.isPressed(map(button)); }

void MappedInput::update() {
  gpio.update();
  confirmLatched = gpio.wasReleased(HalGPIO::BTN_CONFIRM) || consumePendingConfirmTaps() > 0;
  backLatched = gpio.wasReleased(HalGPIO::BTN_BACK) || consumePendingCancelTaps() > 0;
}

// Catch release edges the main loop misses during a blocking refresh.
bool MappedInput::busyWaitPoll(int8_t /*busyPin*/, uint8_t /*busyLevel*/) {
  ::gpio.update();
  power::pollForHold(::gpio);
  const bool edgeSides = ::gpio.hasEdgeSideButtons();
  if (::gpio.wasReleased(HalGPIO::BTN_DOWN) || ::gpio.wasReleased(HalGPIO::BTN_RIGHT) ||
      (edgeSides && ::gpio.wasReleased(HalGPIO::BTN_UP))) {
    ++pendingForwardTaps;
  }
  if (::gpio.wasReleased(HalGPIO::BTN_LEFT) || (!edgeSides && ::gpio.wasReleased(HalGPIO::BTN_UP))) {
    ++pendingBackTaps;
  }
  if (::gpio.wasReleased(HalGPIO::BTN_CONFIRM)) {
    ++pendingConfirmTaps;
  }
  if (::gpio.wasReleased(HalGPIO::BTN_BACK)) {
    ++pendingCancelTaps;
  }
  return false;  // let the driver still run its normal fallback delay
}

void MappedInput::installBusyWaitPoll() { display.setBusyWaitSliceHook(&MappedInput::busyWaitPoll); }

uint16_t MappedInput::consumePendingForwardTaps() {
  const uint16_t n = pendingForwardTaps;
  pendingForwardTaps = 0;
  return n;
}

uint16_t MappedInput::consumePendingBackTaps() {
  const uint16_t n = pendingBackTaps;
  pendingBackTaps = 0;
  return n;
}

uint16_t MappedInput::consumePendingConfirmTaps() {
  const uint16_t n = pendingConfirmTaps;
  pendingConfirmTaps = 0;
  return n;
}

uint16_t MappedInput::consumePendingCancelTaps() {
  const uint16_t n = pendingCancelTaps;
  pendingCancelTaps = 0;
  return n;
}

void MappedInput::resetPendingPageTaps() {
  pendingForwardTaps = 0;
  pendingBackTaps = 0;
  pendingConfirmTaps = 0;
  pendingCancelTaps = 0;
}

int MappedInput::queuedPageDelta() const {
  return static_cast<int>(pendingForwardTaps) - static_cast<int>(pendingBackTaps);
}

int MappedInput::consumeNavigationDelta() {
  int delta = 0;
  if (wasReleased(Button::PageForward) || wasReleased(Button::Right) || wasReleased(Button::Down)) {
    ++delta;
  }
  if (wasReleased(Button::PageBack) || wasReleased(Button::Left) || wasReleased(Button::Up)) {
    --delta;
  }
  delta += static_cast<int>(consumePendingForwardTaps());
  delta -= static_cast<int>(consumePendingBackTaps());
  return delta;
}

int MappedInput::consumeReaderPageDelta() {
  int delta = 0;
  const bool edgeSides = gpio.hasEdgeSideButtons();
  if (wasReleased(Button::PageForward) || wasReleased(Button::Right) || wasReleased(Button::Down) ||
      (edgeSides && wasReleased(Button::Up))) {
    ++delta;
  }
  if (wasReleased(Button::Left) || (!edgeSides && wasReleased(Button::Up))) {
    --delta;
  }
  // A flick pages forward, in either direction.
  if (settings.tiltPageTurn && halTiltSensor.consumeFlick()) {
    ++delta;
  }
  delta += static_cast<int>(consumePendingForwardTaps());
  delta -= static_cast<int>(consumePendingBackTaps());
  return delta;
}
