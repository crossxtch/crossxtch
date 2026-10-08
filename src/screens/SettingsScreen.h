#pragma once

#include "core/Screen.h"

class SettingsScreen final : public Screen {
  int index = 0;
  uint8_t shownMinute = 255;
  // First failed save stays on this screen so the notice can paint. The next
  // Back leaves even if the card still rejects the write.
  bool saveWarned = false;

  bool persist();

 public:
  SettingsScreen(Gfx& gfx, MappedInput& input) : Screen("Settings", gfx, input) {}
  void loop() override;
  void render() override;
};
