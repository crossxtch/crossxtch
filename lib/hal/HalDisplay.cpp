#include <HalDisplay.h>
#include <HalGPIO.h>
#include <Logging.h>

HalDisplay display;

namespace {
const char* refreshName(const HalDisplay::RefreshMode mode) {
  switch (mode) {
    case HalDisplay::FULL_REFRESH:
      return "full";
    case HalDisplay::HALF_REFRESH:
      return "half";
    case HalDisplay::FAST_REFRESH:
    default:
      return "fast";
  }
}

// Wall time of one panel or SPI call. Serial is the tuning log for waveform vs copy.
void logPerf(const char* what, const uint32_t started) {
  LOG_INF("PERF", "%s %lums", what, static_cast<unsigned long>(millis() - started));
}
}  // namespace

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
  const uint32_t t0 = millis();
  einkDisplay.displayBuffer(convertRefreshMode(mode), turnOffScreen);
  LOG_INF("PERF", "panel %s %lums", refreshName(mode), static_cast<unsigned long>(millis() - t0));
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
  const uint32_t t0 = millis();
  einkDisplay.displayGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
  LOG_INF("PERF", "base %s %lums", refreshName(fallback), static_cast<unsigned long>(millis() - t0));
}

void HalDisplay::startGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  if (gpio.deviceIsX3() && fallback == RefreshMode::HALF_REFRESH) {
    einkDisplay.requestResync(1);
  }
  const uint32_t t0 = millis();
  einkDisplay.startGrayscaleBase(convertRefreshMode(fallback), turnOffScreen);
  LOG_INF("PERF", "base-start %s %lums", refreshName(fallback), static_cast<unsigned long>(millis() - t0));
}

void HalDisplay::finishGrayscaleBase() {
  const uint32_t t0 = millis();
  einkDisplay.finishGrayscaleBase();
  logPerf("base-wait", t0);
}

void HalDisplay::preconditionGrayscale() {
  const uint32_t t0 = millis();
  einkDisplay.preconditionGrayscale();
  logPerf("precondition", t0);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {
  const uint32_t t0 = millis();
  einkDisplay.copyGrayscaleLsbBuffers(lsbBuffer);
  logPerf("spi-lsb", t0);
}

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {
  const uint32_t t0 = millis();
  einkDisplay.copyGrayscaleMsbBuffers(msbBuffer);
  logPerf("spi-msb", t0);
}

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {
  const uint32_t t0 = millis();
  einkDisplay.cleanupGrayscaleBuffers(bwBuffer);
  logPerf("cleanup", t0);
}

void HalDisplay::displayGrayBuffer(bool turnOffScreen) {
  const uint32_t t0 = millis();
  einkDisplay.displayGrayBuffer(turnOffScreen);
  logPerf("gray", t0);
}

void HalDisplay::startGrayBuffer(bool turnOffScreen) {
  const uint32_t t0 = millis();
  einkDisplay.startGrayBuffer(turnOffScreen);
  logPerf("gray-start", t0);
}

void HalDisplay::finishGrayBuffer() {
  const uint32_t t0 = millis();
  einkDisplay.finishGrayBuffer();
  logPerf("gray-wait", t0);
}

bool HalDisplay::combinesGrayscaleBase() const { return einkDisplay.combinesGrayscaleBase(); }

uint16_t HalDisplay::getDisplayWidth() const { return einkDisplay.getDisplayWidth(); }

uint16_t HalDisplay::getDisplayHeight() const { return einkDisplay.getDisplayHeight(); }

uint16_t HalDisplay::getDisplayWidthBytes() const { return einkDisplay.getDisplayWidthBytes(); }
