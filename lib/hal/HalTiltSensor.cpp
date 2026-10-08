#include "HalTiltSensor.h"

#include <Logging.h>

#include <cmath>

HalTiltSensor halTiltSensor;  // Singleton instance

bool HalTiltSensor::readTiltRate(float& dps) const {
  Imu::Sample sample;
  if (!_sdkImu.read(sample)) return false;
  dps = sample.gx;
  return true;
}

void HalTiltSensor::begin() {
  _available = _sdkImu.begin();
  if (_available) {
    _lastPollMs = millis();
    // begin() leaves the sensors sampling; stand them by until tilt page turn
    // actually wakes them, so a disabled IMU doesn't drain the battery.
    if (!_sdkImu.sleep()) {
      LOG_ERR("GYR", "IMU standby failed");
    }
    LOG_INF("GYR", "SDK IMU initialized");
    return;
  }
  LOG_DBG("GYR", "IMU not present");
}

bool HalTiltSensor::wake() {
  if (!_available) {
    return false;
  }

  if (!_sdkImu.wake()) {
    LOG_ERR("GYR", "IMU wake failed");
    return false;
  }

  _lastPollMs = millis();
  _lastTiltMs = millis();
  _wakeMs = millis();
  _isAwake = true;
  return true;
}

bool HalTiltSensor::deepSleep() {
  if (!_available) {
    return false;
  }

  if (!_sdkImu.sleep()) {
    LOG_ERR("GYR", "IMU sleep failed");
    return false;
  }

  clearPendingEvents();
  _inTilt = false;
  _isAwake = false;
  return true;
}

void HalTiltSensor::update(const bool enabled, const bool inReader) {
  if (!_available) {
    return;
  }

  // Only sample in the reader. Leaving the IMU awake on home/settings
  // is a milliamps-level drain with no gesture to report.
  const bool wantAwake = enabled && inReader;
  if (wantAwake && !_isAwake) {
    _isAwake = wake();
    return;
  }
  if (!wantAwake && _isAwake) {
    _isAwake = !deepSleep();
    return;
  }
  if (!wantAwake) {
    return;
  }

  const unsigned long now = millis();
  // Stabilization: discard readings during gyro startup transient
  if ((now - _wakeMs) < WAKE_STABILIZE_MS) {
    return;
  }

  if ((now - _lastPollMs) < POLL_INTERVAL_MS) {
    return;
  }
  _lastPollMs = now;

  float tiltAxis = 0;
  if (!readTiltRate(tiltAxis)) {
    return;
  }

  if (_inTilt) {
    // Wait for device to return to neutral before allowing next trigger
    if (fabsf(tiltAxis) < NEUTRAL_RATE_DPS) {
      _inTilt = false;
    }
  } else {
    // Check for new tilt gesture (with cooldown)
    if ((now - _lastTiltMs) >= COOLDOWN_MS) {
      if (tiltAxis > RATE_THRESHOLD_DPS || tiltAxis < -RATE_THRESHOLD_DPS) {
        _flickEvent = true;
        _hadActivity = true;
        _inTilt = true;
        _lastTiltMs = now;
        LOG_INF("GYR", "Flick (%.1f) dps", tiltAxis);
      }
    }
  }
}

bool HalTiltSensor::consumeFlick() {
  const bool val = _flickEvent;
  _flickEvent = false;
  return val;
}

bool HalTiltSensor::hadActivity() {
  const bool val = _hadActivity;
  _hadActivity = false;
  return val;
}

void HalTiltSensor::clearPendingEvents() {
  _flickEvent = false;
  _hadActivity = false;
  // Intentionally preserve _inTilt so a held tilt doesn't retrigger on next poll
}
