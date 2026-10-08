#pragma once

#include <memory>

#include "core/MappedInput.h"

class Gfx;

class Screen {
  friend class ScreenManager;

 protected:
  const char* name;
  Gfx& gfx;
  MappedInput& input;

 public:
  Screen(const char* name, Gfx& gfx, MappedInput& input) : name(name), gfx(gfx), input(input) {}
  virtual ~Screen() = default;

  virtual void onEnter();
  virtual void onExit();
  // Pop restores the parent without onEnter(); override to refresh state that
  // must change when this screen is uncovered. Call the base so the next
  // presentUi() uses HALF_REFRESH.
  virtual void onResume();
  virtual void loop() {}
  virtual void render() {}
  virtual bool isReader() const { return false; }
  virtual bool blocksSleep() const { return false; }

  void requestUpdate();
  // levels > 1 pops through intermediate screens without resuming them
  // (e.g. jump straight back to the reader, skipping a menu two levels up).
  void finish(int levels = 1);
  void push(std::unique_ptr<Screen> screen);
  void goHome();
  bool goToReader(const char* path);
  bool goToBrowser();
  bool goToSettings();
  void goToFirmwareUpdate(bool recovery = false);
  bool goToWifiFileTransfer();

  // HALF_REFRESH on the first paint after enter/resume, FAST afterwards.
  void presentUi();

 protected:
  // One line a screen can show after a failure that would otherwise look like
  // a dead button. Cleared by the screen when the next action succeeds.
  const char* notice = nullptr;
  void setNotice(const char* text);
  // y < 0 draws just above the bottom edge.
  void drawNotice(int y = -1);

 private:
  bool needsCleanRefresh = true;
};
