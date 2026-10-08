#pragma once

#include <Arduino.h>
#include <Rtc.h>

class HalClock;
extern HalClock halClock;  // Singleton

class HalClock {
  bool _available = false;
  mutable Rtc _sdkRtc;
  mutable uint8_t _cachedHour = 0;
  mutable uint8_t _cachedMinute = 0;
  mutable bool _hasCachedTime = false;
  mutable unsigned long _lastPollMs = 0;

  static constexpr unsigned long CLOCK_POLL_MS = 10000;  // 10 seconds

 public:
  // Call after BoardConfig has selected the active device.
  void begin();

  // True if an RTC is present on this device
  bool isAvailable() const { return _available; }

  // Full UTC wall time from the RTC. False if missing or oscillator stopped.
  bool nowUtc(Rtc::DateTime& dt) const;

  // Local hour (0-23) and minute. Offset is biased quarter-hours: 48 = UTC+0,
  // 0 = UTC-12, 104 = UTC+14. False if the RTC is missing.
  bool getLocalTime(uint8_t& hour, uint8_t& minute, uint8_t utcOffsetQuarterHoursBiased = 48) const;

  // Sync the RTC from an NTP server. Requires WiFi to be connected.
  // Blocks for up to ~5s while waiting for SNTP response.
  // Returns true if the RTC was successfully updated.
  //
  // Debouncing (skip if already synced once) is enforced by the caller, not here,
  // so the HAL stays free of any app-layer settings dependency.
  bool syncFromNTP();

 private:
  bool getTime(uint8_t& hour, uint8_t& minute) const;
};
