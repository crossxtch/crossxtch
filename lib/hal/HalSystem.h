#pragma once

namespace HalSystem {
void begin();
// Write a crash report to the SD card when this boot followed a panic.
void checkPanic();
bool isRebootFromPanic();
}  // namespace HalSystem
