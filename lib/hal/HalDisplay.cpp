#include <HalDisplay.h>
#include <HalGPIO.h>

HalDisplay display;

HalDisplay::HalDisplay() : einkDisplay(EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RST, EPD_BUSY) {}

void HalDisplay::begin() {
  if (gpio.deviceIsX3()) {
    einkDisplay.setDisplayX3();
  }
  einkDisplay.begin();

  // Resync after a wake that did not leave the panel showing this framebuffer.
  const auto wakeupReason = gpio.getWakeupReason();
  if (wakeupReason == HalGPIO::WakeupReason::PowerButton || wakeupReason == HalGPIO::WakeupReason::AfterFlash ||
      wakeupReason == HalGPIO::WakeupReason::Other) {
    einkDisplay.requestResync();
  }
}

void HalDisplay::clearScreen(uint8_t color) const { einkDisplay.clearScreen(color); }

static EInkDisplay::RefreshMode convertRefreshMode(HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return EInkDisplay::FULL_REFRESH;
    case HalDisplay::HALF_REFRESH:
      return EInkDisplay::HALF_REFRESH;
    case HalDisplay::FAST_REFRESH:
    default:
      return EInkDisplay::FAST_REFRESH;
  }
}

void HalDisplay::displayBuffer(HalDisplay::RefreshMode mode, bool turnOffScreen) {
  // X3 HALF is the UC8253 scrub bank: one waveform that drives every pixel to
  // the target. Promoting it to requestResync(1) instead ran a full clear, a
  // conditioning pass, and a fast settle (~930+460+430 ms) on every screen change.
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
}

void HalDisplay::setBusyWaitSliceHook(bool (*sliceHook)(int8_t busyPin, uint8_t busyLevel)) {
  einkDisplay.setBusyWaitSliceHook(sliceHook);
}

void HalDisplay::setInverted(bool inverted) { einkDisplay.setInverted(inverted); }

void HalDisplay::deepSleep() { einkDisplay.deepSleep(); }

uint8_t* HalDisplay::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  // X3 HALF wants a clean base. Without the resync, the gentle differential
  // waveform ghosts the previous frame. The reader's FAST path stays differential.
  if (gpio.deviceIsX3() && fallback == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }
  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
}

void HalDisplay::startGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  if (gpio.deviceIsX3() && fallback == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }
  einkDisplay.startGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
}

void HalDisplay::finishGrayscaleBase() { einkDisplay.finishGrayscaleBase(); }

void HalDisplay::preconditionGrayscale() { einkDisplay.preconditionGrayscale(); }

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) { einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer); }

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) { einkDisplay.copyGrayscaleMsbBuffers(msbBuffer); }

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) { einkDisplay.cleanupGrayscaleBuffers(bwBuffer); }

void HalDisplay::displayGrayBuffer(bool turnOffScreen) { einkDisplay.displayGrayBuffer(turnOffScreen); }

void HalDisplay::startGrayBuffer(bool turnOffScreen) { einkDisplay.startGrayBuffer(turnOffScreen); }

void HalDisplay::finishGrayBuffer() { einkDisplay.finishGrayBuffer(); }

bool HalDisplay::combinesGrayscaleBase() const { return einkDisplay.combinesGrayscaleBase(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }
