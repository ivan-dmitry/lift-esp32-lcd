// LVGL assert → restart, with the reason kept across the restart (main.cpp).
// Included by lv_conf.h, so it must stay plain C.
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
void lvAssertRestart(void);
#ifdef __cplusplus
}
#endif
