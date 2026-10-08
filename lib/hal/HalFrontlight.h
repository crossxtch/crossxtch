#pragma once

#include <FrontlightManager.h>

// Inert on boards without a frontlight, so callers need no board check.
class HalFrontlight {
 public:
  static HalFrontlight& getInstance() { return instance; }

  void begin(uint8_t brightness, uint8_t warmth, bool on);

 private:
  HalFrontlight() = default;

  FrontlightManager manager;
  static HalFrontlight instance;
};

#define Frontlight HalFrontlight::getInstance()
