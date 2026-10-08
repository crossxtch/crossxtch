#pragma once

#include <Gfx.h>
#include <HalClock.h>
#include <HalPowerManager.h>

#include <cstdio>

#include "core/Settings.h"
#include "core/fontIds.h"

// Shared list-screen drawing. Menu screens (Home, Settings) use a wider inset;
// file/chapter lists use the full-width row.
namespace ui {

inline int wrapIndex(const int index, const int delta, const int count) {
  if (count <= 0) {
    return 0;
  }
  return ((index + delta) % count + count) % count;
}

inline bool applyDelta(int& index, const int delta, const int count) {
  if (delta == 0 || count <= 0) {
    return false;
  }
  index = wrapIndex(index, delta, count);
  return true;
}

inline void followWindow(int& window, const int index, const int rows) {
  if (rows <= 0) {
    window = 0;
    return;
  }
  if (index < window) {
    window = index;
  } else if (index >= window + rows) {
    window = index - rows + 1;
  }
}

inline void drawMenuRow(Gfx& gfx, const int y, const int rowH, const char* label, const bool selected) {
  if (selected) {
    gfx.fillRect(24, y - 4, gfx.width() - 48, rowH, true);
    gfx.drawText(FONT_UI, 40, y, label, false);
  } else {
    gfx.drawText(FONT_UI, 40, y, label, true);
  }
}

inline void drawRow(Gfx& gfx, const int y, const int rowH, const char* label, const bool selected) {
  if (selected) {
    gfx.fillRect(8, y - 2, gfx.width() - 16, rowH, true);
  }
  gfx.drawText(FONT_UI, 16, y, label, !selected);
}

// True when the clock minute on screen is stale.
inline bool statusMinuteChanged(const uint8_t shownMinute) {
  uint8_t hour = 0;
  uint8_t minute = 0;
  return halClock.getLocalTime(hour, minute, settings.clockUtcOffsetQ) && minute != shownMinute;
}

// Centered clock and battery percent on the top row. Updates shownMinute.
inline void drawStatusBar(Gfx& gfx, uint8_t& shownMinute) {
  uint8_t hour = 0;
  uint8_t minute = 0;
  if (halClock.getLocalTime(hour, minute, settings.clockUtcOffsetQ)) {
    char clock[8];
    snprintf(clock, sizeof(clock), "%02u:%02u", hour, minute);
    gfx.drawCenteredText(FONT_UI_BOLD, 8, clock);
    shownMinute = minute;
  }

  char bat[8];
  snprintf(bat, sizeof(bat), "%u%%", static_cast<unsigned>(powerManager.getBatteryPercentage()));
  gfx.drawText(FONT_UI, gfx.width() - gfx.textWidth(FONT_UI, bat) - 12, 8, bat);
}

}  // namespace ui
