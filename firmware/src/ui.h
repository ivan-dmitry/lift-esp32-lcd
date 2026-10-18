#pragma once
#include <lvgl.h>
#include "config.h"

enum Screen : uint8_t {
  SCR_MAIN = 0, SCR_MENU, SCR_CALL, SCR_PIN, SCR_CAN, SCR_SNIFF, SCR_FRAME,
  SCR_MAP, SCR_FLOORS, SCR_FLOORLIST, SCR_NET, SCR_WIFISCAN, SCR_MQTT,
  SCR_DISPLAY, SCR_AUDIO, SCR_LOG, SCR_ABOUT, SCR_CLOCK, SCR_KEYBOARD,
  SCR_COUNT
};

namespace UI {
  void   begin();
  void   loop();                 // refresh live values, return to the main screen when idle
  void   show(Screen s);         // only RECORDS the request — loop() builds the screen
  void   splash(uint32_t ms);
  Screen current();
  bool   authed();               // always true: panel settings need no code (web page still does)
  void   clearAuth();
}
