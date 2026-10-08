#include "HalFrontlight.h"

#include <Logging.h>

HalFrontlight HalFrontlight::instance;

void HalFrontlight::begin(const uint8_t brightness, const uint8_t warmth, const bool on) {
  if (!manager.present()) return;

  manager.begin();
  const uint8_t level = brightness > 100 ? 100 : brightness;
  const uint8_t warm = warmth > 100 ? 100 : warmth;
  manager.setColorTemperature(warm);
  manager.setBrightness(on ? level : 0);
  LOG_INF("LIGHT", "Frontlight up: %u%% warm=%u%% %s", level, warm, on ? "on" : "off");
}
