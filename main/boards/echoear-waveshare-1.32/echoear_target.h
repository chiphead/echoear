#pragma once

#include "driver/gpio.h"
#include "lvgl.h"

namespace EchoEarTarget {
inline constexpr const char* kDisplayName = "Waveshare 1.32";
inline constexpr auto kRotation = LV_DISPLAY_ROTATION_0;
inline constexpr bool kDeviceAec = false;
inline constexpr bool kSoundDirection = false;
inline constexpr auto kSdClock = GPIO_NUM_NC;
inline constexpr auto kSdCommand = GPIO_NUM_NC;
inline constexpr auto kSdData = GPIO_NUM_NC;
}  // namespace EchoEarTarget
