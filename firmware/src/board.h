/*
   Pin assignment.

   There is only one board here (the ESP32-C6 Super Mini), so
   the pins live right here instead of in Kconfig. Should a second one come
   along, this moves into a Kconfig.projbuild -- before that it would be
   ceremony without benefit.

   ESP32-C6 Super Mini:

     GPIO1   ADC1_CH1   phototransistor of the reflective sensor
     GPIO15  LED        the board's status LED
     GPIO9   BOOT       button, debounced in recovery.cpp
     GPIO8              WS2812 RGB LED, unused here
     GPIO12/13          USB D-/D+, do not touch

   Copyright 2026 Jonas Brüstel
   SPDX-License-Identifier: Apache-2.0
*/

#pragma once

#include <cstdint>
#include <driver/gpio.h>

namespace board {

inline constexpr const char *NAME = "esp32c6-supermini";

/* The ADC input. On the C6, ADC1 sits on GPIO0..GPIO6 and there is no ADC2;
   of those, only GPIO0..GPIO3 are broken out on the Super Mini and free of
   boot duties. */
inline constexpr int PIN_ADC = 1;

/* The IR LED is tied to 3V3 through 560 Ω (R1, about 3.7 mA) -- inside the
   closed module bay there is no ambient light it would need to be pulsed
   against. 100 Ω, the first value, drove the phototransistor into saturation.

   Set a GPIO here if you ever want to measure with the LED on and off to
   subtract ambient light. At under 4 mA a GPIO can drive it directly. */
inline constexpr int PIN_IR = -1;

inline constexpr int PIN_LED_STATUS = 15;

/* No second (activity) LED on this board. The RGB LED on
   GPIO8 could be one, but would need the led_strip driver for a single
   blink pattern. */
inline constexpr int PIN_LED_ACTIVITY = -1;

inline constexpr int PIN_BOOT = 9;

/* Verified on this board (2026-09-27): the LED lights with the pin high. The
   Super Mini clones are not consistent here -- if the LED blinks inverted on
   another one, it is this switch and nothing else. */
inline constexpr bool LED_ACTIVE_LOW = false;

static_assert(PIN_ADC >= 0 && PIN_ADC <= 6, "PIN_ADC must be on ADC1 -- on the C6 that is GPIO0..GPIO6");
static_assert(PIN_BOOT < 0 || PIN_BOOT != PIN_ADC, "PIN_BOOT collides with the ADC input");
static_assert(PIN_IR < 0 || PIN_IR != PIN_ADC, "PIN_IR collides with the ADC input");

inline constexpr bool has_led(int pin) {
  return pin >= 0;
}

inline uint32_t led_level(bool on) {
  return static_cast<uint32_t>(LED_ACTIVE_LOW ? !on : on);
}

inline void led_init(int pin) {
  if (!has_led(pin))
    return;
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << pin,
      .mode         = GPIO_MODE_OUTPUT,
      .pull_up_en   = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type    = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
  gpio_set_level(static_cast<gpio_num_t>(pin), led_level(false));
}

inline void button_init(int pin) {
  if (pin < 0)
    return;
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << pin,
      .mode         = GPIO_MODE_INPUT,
      /* The button pulls to ground, so the pin needs a pull-up to read high
         while nobody presses it. */
      .pull_up_en   = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type    = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
}

inline bool button_pressed(int pin) {
  return pin >= 0 && gpio_get_level(static_cast<gpio_num_t>(pin)) == 0;
}

inline void led_set(int pin, bool on) {
  if (has_led(pin))
    gpio_set_level(static_cast<gpio_num_t>(pin), led_level(on));
}

} // namespace board
