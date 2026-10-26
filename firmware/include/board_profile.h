#pragma once
#include <Arduino.h>
#include <driver/gpio.h>
#ifndef WANAN_BOARD_ID
#define WANAN_BOARD_ID "custom-unverified"
#endif
#ifndef WANAN_LED_PIN
#define WANAN_LED_PIN -1
#endif
#ifndef WANAN_LED_IS_RGB
#define WANAN_LED_IS_RGB 0
#endif
#ifndef WANAN_LED_ACTIVE_LOW
#define WANAN_LED_ACTIVE_LOW 0
#endif
static_assert(WANAN_LED_PIN == -1 || GPIO_IS_VALID_OUTPUT_GPIO(WANAN_LED_PIN), "Invalid LED output GPIO");
namespace BoardProfile {
inline constexpr const char* kId = WANAN_BOARD_ID;
inline constexpr bool kLedActiveLow = WANAN_LED_ACTIVE_LOW != 0;
#if CONFIG_FREERTOS_UNICORE
inline constexpr BaseType_t kNetCore = 0;
#else
inline constexpr BaseType_t kNetCore = 1;
#endif
}
