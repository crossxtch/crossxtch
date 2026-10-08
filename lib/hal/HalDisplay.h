#pragma once
#include <Arduino.h>
#include <EInkDisplay.h>

class HalDisplay {
 public:
  HalDisplay();

  enum RefreshMode {
    FULL_REFRESH,  // Full refresh with complete waveform
    HALF_REFRESH,  // Scrub refresh: every pixel driven to target
    FAST_REFRESH   // Fast refresh using custom LUT
  };

  void begin();

  void clearScreen(uint8_t color = 0xFF) const;
  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);

  // Optional poll during blocking BUSY waits (button edges the main loop would miss).
  void setBusyWaitSliceHook(bool (*sliceHook)(int8_t busyPin, uint8_t busyLevel));

  // Framebuffer stays in normal polarity. The driver inverts while sending.
  void setInverted(bool inverted);
  void deepSleep();
  uint8_t* getFrameBuffer() const;

  // X3 grayscale preconditioning (OEM "AA-pre-BW(mid)" settle pass). Call after
  // the BW base frame is displayed and before the grayscale planes are written.
  // No-op on X4.
  void preconditionGrayscale();

  // Base frame for a grayscale overlay. On X3, HALF first requests a resync so
  // the prior frame does not ghost through the differential waveform. FAST
  // keeps that differential path. Other panels display with `fallback`.
  void displayGrayscaleBase(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false);
  // X3: fire the base waveform and return once BUSY is active so the host can
  // paint the LSB plane in CPU RAM. finish waits it out. Other panels block in
  // start. Do not SPI to the panel between start and finish.
  void startGrayscaleBase(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false);
  void finishGrayscaleBase();

  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);

  void displayGrayBuffer(bool turnOffScreen = false);
  void startGrayBuffer(bool turnOffScreen = false);
  void finishGrayBuffer();

  // True when the base and gray planes share one waveform (not X3/X4).
  bool combinesGrayscaleBase() const;

  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;

 private:
  EInkDisplay einkDisplay;
};

extern HalDisplay display;
