#pragma once

#include <Arduino.h>
#include <Imu.h>

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;

// Portrait flick detector. Either direction is one event; the reader pages forward.
class HalTiltSensor {
 public:
  void begin();
  bool deepSleep();
  bool isAvailable() const { return _available; }

  // `enabled` is tilt page-turn on and not locked. The IMU sleeps outside the reader.
  void update(bool enabled, bool inReader);

  // True once per flick. Consumed on read.
  bool consumeFlick();

  // Non-consuming until read. Resets the auto-sleep timer.
  bool hadActivity();
  void clearPendingEvents();

 private:
  bool wake();
  // Portrait left/right rate, degrees per second.
  bool readTiltRate(float& dps) const;

  bool _available = false;
  mutable Imu _sdkImu;

  bool _flickEvent = false;
  bool _hadActivity = false;
  bool _inTilt = false;
  bool _isAwake = false;
  unsigned long _lastTiltMs = 0;
  unsigned long _wakeMs = 0;

  static constexpr float RATE_THRESHOLD_DPS = 150.0f;
  static constexpr float NEUTRAL_RATE_DPS = 50.0f;
  static constexpr unsigned long COOLDOWN_MS = 1000;
  static constexpr unsigned long POLL_INTERVAL_MS = 50;
  static constexpr unsigned long WAKE_STABILIZE_MS = 300;

  mutable unsigned long _lastPollMs = 0;
};
