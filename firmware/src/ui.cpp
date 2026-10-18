// LVGL screens — landscape 480×320
// Fixed grid: status bar 28 · body 236 · footer 56 (same grid as esp32Fire)
#include "ui.h"
#include "ui_theme.h"
#include "drivers.h"
#include "store.h"
#include "lift.h"
#include <esp_timer.h>

static Screen   s_cur      = SCR_MAIN;
static Screen   s_prev     = SCR_MAIN;
static Screen   s_pending  = SCR_COUNT;   // screen waiting to be built (SCR_COUNT = none)
static Screen   s_afterPin = SCR_MENU;
static bool     s_wantCloseModal = false;
static bool     s_wantCal = false;        // touch calibration blocks LVGL, so it is deferred
static uint8_t  s_wantExport = 0;
static lv_obj_t* s_modal = nullptr;
static bool     s_sbClock = false;        // status bar right side shows network + clock
static char     s_toast[96] = "";
static uint32_t s_toastUntil = 0;

// Named pointers to the widgets the refresh functions update. Reset to nullptr
// on every screen build. esp32Fire used a numbered array s_dyn[] and twice
// wrote past its end because the numbering drifted (the README lists both
// panics); names cannot drift.
static struct {
  lv_obj_t* sbL; lv_obj_t* sbR; lv_obj_t* clk; int16_t sbLx;
  lv_obj_t* wxIc; lv_obj_t* wxT; uint32_t wxGen; bool wxShown; int16_t wxX; bool touchB;
  // main
  lv_obj_t* ind; lv_obj_t* arrow; lv_obj_t* floor;
  lv_obj_t* ban; lv_obj_t* banIc; lv_obj_t* banTx; lv_obj_t* banSub;
  const lv_font_t* floorFont; const lv_font_t* bigFont;
  int16_t   floorYBig, floorY3, arrowW, arrowX, floorX, floorW;
  bool      incident;
  // call
  lv_obj_t* callInfo; lv_obj_t* fb[18]; lv_obj_t* fbL[18]; int8_t fbIdx[18]; uint8_t fbN;
  // can
  lv_obj_t* can1; lv_obj_t* can2; lv_obj_t* can3; uint32_t canSig;
  // sniff
  lv_obj_t* snMain[SEEN_MAX]; lv_obj_t* snRight[SEEN_MAX]; uint8_t snN; uint16_t snGen;
  // frame
  lv_obj_t* byBox[8]; lv_obj_t* byVal[8]; lv_obj_t* biBox[8]; lv_obj_t* biVal[8];
  lv_obj_t* fr1; lv_obj_t* fr2;
  // map
  lv_obj_t* mpR[SIG_COUNT]; lv_obj_t* coM[3]; lv_obj_t* coR[3];
  // floors / mqtt / about
  lv_obj_t* flLive; lv_obj_t* mqSt; lv_obj_t* ab[6];
  int8_t    netSig;
} W;

static void showNow(Screen s);

// ─────────────────────── widget helpers ───────────────────────
static lv_obj_t* mkBox(lv_obj_t* p, int x, int y, int w, int h, lv_color_t bg) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, bg, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t* mkLbl(lv_obj_t* p, int x, int y, const char* t,
                       const lv_font_t* f, lv_color_t c, int w = 0) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  if (w) { lv_obj_set_width(l, w); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); }
  lv_label_set_text(l, t);
  return l;
}

// SINGLE-LINE label: setting both width and height makes LVGL cut with "..."
// instead of wrapping onto a second line over the content below. Mandatory
// for every label inside a fixed-height list row.
static lv_obj_t* mkLbl1(lv_obj_t* p, int x, int y, const char* t,
                        const lv_font_t* f, lv_color_t c, int w, int h) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_pos(l, x, y);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_obj_set_size(l, w, h);
  lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  lv_label_set_text(l, t);
  return l;
}

// Only touch LVGL when something really changed: lv_label_set_text() and the
// style setters invalidate the area even for an identical value, and the main
// screen is refreshed 10 times a second.
static void setTxt(lv_obj_t* l, const char* t) {
  if (l && strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t);
}
static void setCol(lv_obj_t* l, lv_color_t c) {
  if (l && lv_obj_get_style_text_color(l, 0).full != c.full) lv_obj_set_style_text_color(l, c, 0);
}
static void setBg(lv_obj_t* o, lv_color_t c) {
  if (o && lv_obj_get_style_bg_color(o, 0).full != c.full) lv_obj_set_style_bg_color(o, c, 0);
}

static const char* statusRight() {
  static char r[48];
  const char* net = !Net::on() ? "" : Net::apMode() ? "AP · " : Mqtt::connected() ? "HA · "
                  : Net::staConnected() ? "WiFi · " : "no WiFi · ";
  snprintf(r, sizeof(r), "%s%s", net, Clock::timeOnly());
  return r;
}

static void statusBar(const char* left, const char* right,
                      lv_color_t bg = C_SURF, lv_color_t fg = C_DIM) {
  lv_obj_t* sb = mkBox(lv_scr_act(), 0, 0, LCD_W, H_STATUSBAR, bg);
  W.sbL = mkLbl(sb, 10, 6, left, &ui12, fg, 260);
  W.sbR = mkLbl(sb, LCD_W - 210, 6, right, &ui12, fg, 200);
  lv_obj_set_style_text_align(W.sbR, LV_TEXT_ALIGN_RIGHT, 0);
}
static void statusBarClock(const char* left) {
  statusBar(left, statusRight());
  s_sbClock = true;
}

struct FootBtn { const char* txt; lv_color_t bg; lv_color_t fg; lv_event_cb_t cb; intptr_t arg; };

static void footer(const FootBtn* b, uint8_t n) {
  const lv_font_t* fnt = (n >= 5) ? &ui12 : &ui14b;
  for (uint8_t i = 0; i < n; i++) {
    const int w = LCD_W / n;
    lv_obj_t* o = lv_btn_create(lv_scr_act());
    lv_obj_set_pos(o, i * w, LCD_H - H_FOOTER);
    lv_obj_set_size(o, w - 1, H_FOOTER);
    lv_obj_set_style_bg_color(o, b[i].bg, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_t* l = lv_label_create(o);
    lv_obj_set_style_text_font(l, fnt, 0);
    lv_obj_set_style_text_color(l, b[i].fg, 0);
    lv_label_set_text(l, b[i].txt);
    lv_obj_center(l);
    if (b[i].cb) lv_obj_add_event_cb(o, b[i].cb, LV_EVENT_CLICKED, (void*)b[i].arg);
  }
}

static lv_obj_t* mkBtn(lv_obj_t* p, int x, int y, int w, int h, const char* txt,
                       lv_color_t bg, lv_event_cb_t cb, intptr_t arg,
                       const lv_font_t* f = &ui14b, lv_obj_t** outLbl = nullptr) {
  lv_obj_t* b = lv_btn_create(p);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_radius(b, 4, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_border_color(b, C_ARROW, 0);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, C_TXT, 0);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void*)arg);
  if (outLbl) *outLbl = l;
  return b;
}

// Scrollable list, 46 px rows
static lv_obj_t* mkList(int y, int h) {
  lv_obj_t* l = lv_obj_create(lv_scr_act());
  lv_obj_set_pos(l, 0, y);
  lv_obj_set_size(l, LCD_W, h);
  lv_obj_set_style_bg_color(l, C_BG, 0);
  lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(l, 0, 0);
  lv_obj_set_style_radius(l, 0, 0);
  lv_obj_set_style_pad_all(l, 0, 0);
  lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(l, 0, 0);
  lv_obj_set_scroll_dir(l, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_AUTO);
  return l;
}

// LVGL allocates from the ESP32 heap (LV_MEM_CUSTOM = 1). A list row costs
// ~1 KB; before adding one, make sure there is room — running out mid-build
// means LV_ASSERT_HANDLER and a restart in the middle of someone's work.
// Lists stop adding rows while this much heap is still free. With WiFi, web,
// MQTT and the weather task running the display has ~95 KB; a 60-row LOG took
// 40 KB of it, and a weather fetch at that moment ran LVGL out of memory.
static bool listHasRoom() { return ESP.getFreeHeap() > 80000; }

static lv_obj_t* mkRow(lv_obj_t* list, const char* lead, const char* main,
                       const char* sub, const char* right, lv_color_t mainC,
                       lv_event_cb_t cb, intptr_t arg, const char* lead2 = nullptr,
                       lv_obj_t** outMain = nullptr, lv_obj_t** outRight = nullptr) {
  lv_obj_t* r = lv_obj_create(list);
  lv_obj_set_size(r, LCD_W, 46);
  lv_obj_set_style_bg_color(r, C_BG, 0);
  lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
  lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_color(r, C_LINE, 0);
  lv_obj_set_style_border_width(r, 1, 0);
  lv_obj_set_style_radius(r, 0, 0);
  lv_obj_set_style_pad_all(r, 0, 0);
  lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
  if (lead)  mkLbl1(r, 8, lead2 ? 5 : 15, lead, &ui12, C_DIM, 64, 16);
  if (lead2) mkLbl1(r, 8, 24, lead2, &ui12, C_DIM, 64, 16);
  lv_obj_t* m = nullptr;
  if (main)  m = mkLbl1(r, 76, 5, main, &ui14b, mainC, right ? 300 : 390, 19);
  if (sub)   mkLbl1(r, 76, 26, sub, &ui12, C_DIM, right ? 300 : 390, 16);
  lv_obj_t* rr = nullptr;
  if (right) {
    rr = mkLbl1(r, LCD_W - 110, 15, right, &ui12, C_DIM, 92, 16);   // clear of the scrollbar
    lv_obj_set_style_text_align(rr, LV_TEXT_ALIGN_RIGHT, 0);
  }
  if (cb) lv_obj_add_event_cb(r, cb, LV_EVENT_CLICKED, (void*)arg);
  if (outMain)  *outMain = m;
  if (outRight) *outRight = rr;
  return r;
}

static lv_obj_t* rowBase(lv_obj_t* p, int y, const char* label) {
  lv_obj_t* r = mkBox(p, 0, y, LCD_W, 42, C_BG);
  lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
  lv_obj_set_style_border_color(r, C_LINE, 0);
  lv_obj_set_style_border_width(r, 1, 0);
  mkLbl(r, 10, 13, label, &ui12, C_DIM, 136);
  return r;
}

// Settings row, 42 px: label · value · "-" and "+" buttons. cb gets -1 / +1.
static lv_obj_t* mkFormRow(lv_obj_t* p, int y, const char* label, const char* value,
                           lv_event_cb_t cb) {
  lv_obj_t* r = rowBase(p, y, label);
  lv_obj_t* v = mkLbl1(r, 150, 12, value, &ui14, C_TXT, 220, 19);
  if (cb) {
    mkBtn(r, LCD_W - 100, 6, 44, 30, "-", C_SURF2, cb, -1);
    mkBtn(r, LCD_W - 50, 6, 44, 30, "+", C_SURF2, cb, 1);
  }
  return v;
}

// Row with one wide button instead of "-"/"+"
static lv_obj_t* mkToggleRow(lv_obj_t* p, int y, const char* label, const char* value,
                             const char* btn, lv_color_t btnBg, lv_event_cb_t cb) {
  lv_obj_t* r = rowBase(p, y, label);
  lv_obj_t* v = mkLbl1(r, 150, 12, value, &ui14, C_TXT, 220, 19);
  mkBtn(r, LCD_W - 104, 6, 98, 30, btn, btnBg, cb, 0, &ui12);
  return v;
}

// Row whose value box opens the keyboard
static lv_obj_t* mkTapRow(lv_obj_t* p, int y, const char* label, const char* value,
                          lv_event_cb_t cb) {
  lv_obj_t* r = rowBase(p, y, label);
  lv_obj_t* box = mkBox(r, 150, 6, 320, 30, C_SURF2);
  lv_obj_set_style_radius(box, 4, 0);
  lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
  mkLbl1(box, 10, 6, value, &ui14, C_TXT, 300, 19);
  if (cb) lv_obj_add_event_cb(box, cb, LV_EVENT_CLICKED, nullptr);
  return box;
}

static void toast(const char* t) {
  snprintf(s_toast, sizeof(s_toast), "%s", t);
  s_toastUntil = millis() + 4000;
}
static bool toastOn() {
  if (s_toast[0] && timeReached(millis(), s_toastUntil)) s_toast[0] = 0;   // expired: gone, not back in 24.8 days
  return s_toast[0] != 0;
}

// ─────────────────────── navigation ───────────────────────
static void goTo(lv_event_t* e) { UI::show((Screen)(intptr_t)lv_event_get_user_data(e)); }

// Settings on the panel are NOT behind a code: whoever stands at the panel can
// open the menu. The things that touch the lift's bus (ACK mode, sending
// commands, remote commands) still ask for confirmation, and the technician
// code now only locks the web page, which anyone on the network can reach.
bool UI::authed() { return true; }
void UI::clearAuth() {}

static void beepKey() { if (Store::cfg().keyBeep) Out::beep(2400, 25); }

// ─────────────────────── modal dialogs ───────────────────────
// Deferred as well: deleting the dialog inside the CANCEL button's own
// callback deletes the very button LVGL is still dispatching.
static void closeModal(lv_event_t*) { s_wantCloseModal = true; }

static lv_obj_t* modalBack() {
  lv_obj_t* m = mkBox(lv_scr_act(), 0, 0, LCD_W, LCD_H, lv_color_hex(0x05070A));
  lv_obj_set_style_bg_opa(m, LV_OPA_80, 0);
  return m;
}

static void askConfirm(const char* title, const char* msg, const char* okTxt,
                       lv_color_t okBg, lv_event_cb_t okCb) {
  if (s_modal) return;
  s_modal = modalBack();
  lv_obj_t* d = mkBox(s_modal, 40, 64, 400, 190, C_SURF);
  lv_obj_set_style_radius(d, 6, 0);
  lv_obj_set_style_border_width(d, 1, 0);
  lv_obj_set_style_border_color(d, C_LINE, 0);
  mkLbl(d, 16, 14, title, &ui22b, C_TXT, 368);
  lv_obj_t* m = mkLbl(d, 16, 50, msg, &ui12, C_DIM, 368);
  lv_label_set_long_mode(m, LV_LABEL_LONG_WRAP);
  mkBtn(d, 0, 142, 199, 48, "CANCEL", C_SURF2, closeModal, 0);
  lv_obj_t* k = mkBtn(d, 201, 142, 199, 48, okTxt, okBg, okCb, 0);
  lv_obj_set_style_radius(k, 0, 0);
}

struct Choice { const char* txt; lv_event_cb_t cb; intptr_t arg; };

static void askChoice(const char* title, const Choice* c, uint8_t n) {
  if (s_modal) return;
  s_modal = modalBack();
  lv_obj_t* d = mkBox(s_modal, 10, 34, 460, 250, C_SURF);
  lv_obj_set_style_radius(d, 6, 0);
  lv_obj_set_style_border_width(d, 1, 0);
  lv_obj_set_style_border_color(d, C_LINE, 0);
  mkLbl(d, 14, 10, title, &ui14b, C_TXT, 430);
  if (n > 8) n = 8;
  for (uint8_t k = 0; k <= n; k++) {
    const bool cancel = (k == n);
    const uint8_t slot = cancel ? 8 : k;
    lv_obj_t* lbl = nullptr;
    mkBtn(d, 8 + (slot % 3) * 148, 38 + (slot / 3) * 68, 144, 62,
          cancel ? "CANCEL" : c[k].txt, cancel ? lv_color_hex(0x3A2226) : C_SURF2,
          cancel ? closeModal : c[k].cb, cancel ? 0 : c[k].arg, &ui14b, &lbl);
    lv_obj_set_width(lbl, 136);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
  }
}

// ═══════════════════════ Direction arrow — drawn, not a glyph ═══════════════════════
// Icon fonts make small arrows; a 150 px arrow as a glyph would cost more flash
// than the whole floor font. Drawn as a triangle + rectangle it costs nothing
// and scales to any box. While moving it scrolls in the direction of travel —
// the motion is what tells people at a glance that the car is coming.
static uint8_t s_arDir = 0;    // 0 nothing · 1 up · 2 down · 3 doors open
static bool    s_arMove = false;
static int16_t s_arPhase = 0;

static void drawArrowAt(lv_draw_ctx_t* ctx, const lv_area_t& a, lv_coord_t top, bool up,
                        const lv_draw_rect_dsc_t& d) {
  const lv_coord_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
  const lv_coord_t L = h * 72 / 100;          // arrow length
  const lv_coord_t hh = w * 58 / 100;         // head height
  const lv_coord_t sw = w * 38 / 100;         // shaft width
  const lv_coord_t cx = a.x1 + w / 2;
  lv_point_t p[3];
  lv_area_t shaft;
  shaft.x1 = cx - sw / 2;
  shaft.x2 = cx + sw / 2;
  if (up) {
    p[0] = {cx, top}; p[1] = {a.x1, (lv_coord_t)(top + hh)}; p[2] = {a.x2, (lv_coord_t)(top + hh)};
    shaft.y1 = top + hh - 1; shaft.y2 = top + L;
  } else {
    p[0] = {a.x1, (lv_coord_t)(top + L - hh)}; p[1] = {a.x2, (lv_coord_t)(top + L - hh)};
    p[2] = {cx, (lv_coord_t)(top + L)};
    shaft.y1 = top; shaft.y2 = top + L - hh + 1;
  }
  lv_draw_polygon(ctx, &d, p, 3);
  lv_draw_rect(ctx, &d, &shaft);
}

// Doors-open pictogram: door frame, both panels pushed to the sides, and two
// arrows pointing outward in the gap — the same idea as the door-open button
// inside every car. (Material's door glyph shows the doors CLOSED.)
static void drawDoorOpen(lv_draw_ctx_t* ctx, const lv_area_t& a) {
  const lv_coord_t w = lv_area_get_width(&a), h = lv_area_get_height(&a);
  const lv_coord_t ph = h < w * 6 / 5 ? h : w * 6 / 5;      // pictogram height
  lv_area_t fr = {a.x1, (lv_coord_t)(a.y1 + (h - ph) / 2), a.x2, (lv_coord_t)(a.y1 + (h - ph) / 2 + ph - 1)};
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_opa = LV_OPA_TRANSP;
  d.border_color = C_GOLD;                                    // same colour as the floor number
  d.border_width = 6;
  d.border_opa = LV_OPA_COVER;
  d.radius = 4;
  lv_draw_rect(ctx, &d, &fr);                                 // frame
  lv_draw_rect_dsc_init(&d);
  d.bg_color = C_GOLD;
  d.bg_opa = LV_OPA_COVER;
  d.radius = 2;
  // Animation, 2 s loop (s_arPhase 0..49 at 40 ms): the leaves slide apart in
  // 0.8 s (ease-out), then stay open 1.2 s with the arrows showing.
  const lv_coord_t in = 12, inner = w - 2 * in;
  const int st = s_arPhase < 20 ? s_arPhase : 20;
  const int e = 1000 - (20 - st) * (20 - st) * 1000 / 400;   // 0..1000, ease-out
  const lv_coord_t half = inner / 2, open = inner * 3 / 10;
  const lv_coord_t pw = (lv_coord_t)(half - (half - open) * e / 1000);
  lv_area_t pl = {(lv_coord_t)(fr.x1 + in), (lv_coord_t)(fr.y1 + in), (lv_coord_t)(fr.x1 + in + pw), (lv_coord_t)(fr.y2 - in)};
  lv_area_t pr = {(lv_coord_t)(fr.x2 - in - pw), (lv_coord_t)(fr.y1 + in), (lv_coord_t)(fr.x2 - in), (lv_coord_t)(fr.y2 - in)};
  lv_draw_rect(ctx, &d, &pl);                                 // left panel, pushed aside
  lv_draw_rect(ctx, &d, &pr);                                 // right panel
  const lv_coord_t cx = (fr.x1 + fr.x2) / 2, cy = (fr.y1 + fr.y2) / 2;
  const lv_coord_t aw = (pr.x1 - pl.x2) / 2 - 6, ahh = aw;    // arrows fill the gap
  d.radius = 0;
  lv_point_t l[3] = {{(lv_coord_t)(cx - 3), (lv_coord_t)(cy - ahh)}, {(lv_coord_t)(cx - 3), (lv_coord_t)(cy + ahh)},
                     {(lv_coord_t)(cx - 3 - aw), cy}};
  lv_point_t r[3] = {{(lv_coord_t)(cx + 3), (lv_coord_t)(cy - ahh)}, {(lv_coord_t)(cx + 3), (lv_coord_t)(cy + ahh)},
                     {(lv_coord_t)(cx + 3 + aw), cy}};
  if (e < 700) return;                                        // arrows once the gap is wide enough
  lv_draw_polygon(ctx, &d, l, 3);
  lv_draw_polygon(ctx, &d, r, 3);
}

static void arrowDraw(lv_event_t* e) {
  if (!s_arDir) return;
  lv_obj_t* o = lv_event_get_target(e);
  lv_draw_ctx_t* ctx = lv_event_get_draw_ctx(e);
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  if (s_arDir == 3) { drawDoorOpen(ctx, a); return; }
  const lv_coord_t h = lv_area_get_height(&a);
  const lv_coord_t L = h * 72 / 100;
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = C_GOLD;
  d.bg_opa = s_arMove ? LV_OPA_COVER : LV_OPA_60;
  d.radius = 0;
  const bool up = (s_arDir == 1);
  const lv_coord_t base = a.y1 + (h - L) / 2;
  if (!s_arMove) { drawArrowAt(ctx, a, base, up, d); return; }
  // Two copies one box-height apart give a seamless loop; the draw context
  // clips everything outside the object.
  const lv_coord_t off = up ? -s_arPhase : s_arPhase;
  drawArrowAt(ctx, a, base + off, up, d);
  drawArrowAt(ctx, a, base + off + (up ? h : -h), up, d);
}

static void arrowTick(lv_timer_t*) {
  if (!W.arrow || !s_arDir) return;
  if (s_arDir == 3) {                       // door pictogram: its own loop
    s_arPhase = (int16_t)((s_arPhase + 1) % 50);
    lv_obj_invalidate(W.arrow);
    return;
  }
  if (!s_arMove) return;
  // Height is 0 until LVGL's first layout pass after a screen build — and this
  // timer can fire before that. Dividing by it panicked the board.
  const lv_coord_t h = lv_obj_get_height(W.arrow);
  if (h <= 0) return;
  s_arPhase = (int16_t)((s_arPhase + 4) % h);
  lv_obj_invalidate(W.arrow);
}

// ═══════════════════════ MAIN · full-screen lift indicator ═══════════════════════
// The whole 480×320 panel is the indicator: no status bar, no footer. CALL,
// LOG and MENU are three small icon buttons in the top-right corner (hidden in
// display-only mode, where a 2.5 s hold anywhere opens the menu instead).
// The slot left of the floor number shows the direction arrow while there is a
// direction, and a big door pictogram while the car stands with its door open.
//
// Two layouts, rebuilt when the lift changes between them:
//   normal   — giant floor number (220 px), arrow / door, a slim status line below
//   incident — fire service, fault, inspection, out of service, overload or no
//              signal: the bottom third becomes a banner with a 42 px title and
//              a 22 px explanation. Whoever looks at the lift must understand
//              at once that it cannot be used, and why.
static bool isIncident(LiftMode m) { return m != LM_NORMAL; }

// WMO weather code → icon + colour
static void wxIcon(int code, bool day, const char*& ic, lv_color_t& col) {
  if (code <= 1)                               { ic = day ? IC_WX_SUN : IC_WX_NIGHT; col = day ? C_GOLD : C_TXT; }
  else if (code == 2)                          { ic = day ? IC_WX_PARTLY : IC_WX_NIGHT; col = C_TXT; }
  else if (code == 3)                          { ic = IC_CLOUD; col = C_DIM; }
  else if (code == 45 || code == 48)           { ic = IC_WX_FOG; col = C_DIM; }
  else if (code >= 51 && code <= 57)           { ic = IC_WX_DRIZZLE; col = C_ISO; }
  else if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) { ic = IC_WX_RAIN; col = C_ISO; }
  else if ((code >= 71 && code <= 77) || code == 85 || code == 86)   { ic = IC_WX_SNOW; col = C_WHITE; }
  else if (code >= 95)                         { ic = IC_WX_STORM; col = C_GOLD; }
  else                                         { ic = IC_CLOUD; col = C_DIM; }
}

static void longToMenu(lv_event_t*) { UI::show(SCR_MENU); }

static void iconBtn(int x, const char* ic, Screen to) {
  lv_obj_t* b = lv_btn_create(lv_scr_act());
  lv_obj_set_pos(b, x, 6);
  lv_obj_set_size(b, 44, 40);          // touch target stays ≥ 40 px for resistive touch
  lv_obj_set_style_bg_color(b, C_SURF, 0);
  lv_obj_set_style_radius(b, 6, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_border_width(b, 1, 0);
  lv_obj_set_style_border_color(b, C_LINE, 0);
  lv_obj_t* l = lv_label_create(b);
  lv_obj_set_style_text_font(l, &ico28, 0);
  lv_obj_set_style_text_color(l, C_DIM, 0);
  lv_label_set_text(l, ic);
  lv_obj_center(l);
  lv_obj_add_event_cb(b, goTo, LV_EVENT_CLICKED, (void*)(intptr_t)to);
}

static void buildMain() {
  const bool kiosk = Store::cfg().kiosk;
  const bool inc = isIncident(Lift::mode());
  W.incident = inc;

  // Full-screen black base: receives the long press (its children do not take touches)
  W.ind = mkBox(lv_scr_act(), 0, 0, LCD_W, LCD_H, C_BLACK);
  lv_obj_add_event_cb(W.ind, longToMenu, LV_EVENT_LONG_PRESSED, nullptr);

  // Top-left instead of a status bar: the clock, as tall as the icon buttons
  // on the right, and the lift name small beside it
  // Top row, centred: clock · weather icon · temperature (42 px). No lift name.
  W.clk = mkLbl1(W.ind, 0, 3, "", &ui42b, C_WHITE, 140, 46);
  W.wxIc = mkLbl1(W.ind, 0, 8, "", &icowx, C_GOLD, 40, 36);
  W.wxT  = mkLbl1(W.ind, 0, 3, "", &ui42b, C_WHITE, 110, 46);
  W.wxGen = 0xFFFFFFFF; W.wxX = -1;
  // Small line bottom-left, only while an announcement plays or the simulator runs
  W.sbL = mkLbl1(W.ind, 10, 300, "", &ui14b, C_ARROW, 300, 19);

  // Indicator area: y 52..256 normally, y 52..196 when the incident banner is up
  // No status line under the number in normal service: the whole height below
  // the top row is the indicator. The banner only exists for an incident.
  const int top = 52, bottom = inc ? 196 : 312;
  const int area = bottom - top;
  const int ax = 16, aw = 120, ah = inc ? 130 : 180;
  W.arrow = lv_obj_create(W.ind);
  lv_obj_set_pos(W.arrow, ax, top + (area - ah) / 2);
  lv_obj_set_size(W.arrow, aw, ah);
  lv_obj_set_style_bg_opa(W.arrow, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(W.arrow, 0, 0);
  lv_obj_set_style_pad_all(W.arrow, 0, 0);
  lv_obj_clear_flag(W.arrow, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
  lv_obj_add_event_cb(W.arrow, arrowDraw, LV_EVENT_DRAW_MAIN, nullptr);
  s_arDir = 0; s_arMove = false; s_arPhase = 0;

  // fl220 line is 168 px, fl140 is 107 px. While the banner is up the number
  // drops to 140 px so the banner can be big.
  W.bigFont   = inc ? &fl140 : &fl220;
  W.floorYBig = (int16_t)(top + (area - (inc ? 107 : 168)) / 2);
  W.floorY3   = (int16_t)(top + (area - 107) / 2);
  const int fx = ax + aw + 8;
  W.floor = mkLbl(W.ind, fx, W.floorYBig, "--", W.bigFont, C_DIM, LCD_W - fx - 8);
  lv_label_set_long_mode(W.floor, LV_LABEL_LONG_CLIP);
  W.floorFont = W.bigFont;
  W.arrowW = (int16_t)aw;
  W.arrowX = W.floorX = W.floorW = -1;           // refreshMain() centres the group

  if (inc) {
    W.ban = mkBox(W.ind, 8, 200, LCD_W - 16, 112, C_SURF);
    lv_obj_set_style_radius(W.ban, 8, 0);
    lv_obj_clear_flag(W.ban, LV_OBJ_FLAG_CLICKABLE);
    W.banIc = mkLbl(W.ban, 16, 33, "", &ico48, C_TXT);
    W.banTx = mkLbl1(W.ban, 80, 12, "", &ui42b, C_TXT, LCD_W - 16 - 88, 48);
    W.banSub = mkLbl1(W.ban, 80, 66, "", &ui22b, C_TXT, LCD_W - 16 - 88, 30);
  }

  // No touch: the CALL / LOG / MENU buttons would do nothing — not drawn at all
  W.touchB = Display::touchEnabled();
  if (!kiosk && W.touchB) {
    iconBtn(LCD_W - 8 - 44 * 3 - 12, IC_DIALPAD, SCR_CALL);
    iconBtn(LCD_W - 8 - 44 * 2 - 6,  IC_HISTORY, SCR_LOG);
    iconBtn(LCD_W - 8 - 44,          IC_MENU,    SCR_MENU);
  }
}

static void refreshMain() {
  const LiftState& s = Lift::st();
  const LiftMode m = Lift::mode();
  if (isIncident(m) != W.incident) { showNow(SCR_MAIN); return; }   // switch layout
  if (Display::touchEnabled() != W.touchB) { showNow(SCR_MAIN); return; }   // touch switched on the web
  char b[64];

  // Clock (white) + weather, centred as one group in the top row
  const bool clk = Clock::valid();
  setTxt(W.clk, clk ? Clock::timeOnly() : "");
  if (Audio::busy())      snprintf(b, sizeof(b), "ANNOUNCEMENT");
  else if (Lift::simOn()) snprintf(b, sizeof(b), "SIMULATION");
  else                    b[0] = 0;
  setTxt(W.sbL, b);
  {
    // Weather after the clock: condition icon (coloured) + temperature
    const Weather::Now wx = Weather::now();
    if (wx.gen != W.wxGen || wx.valid != W.wxShown) {
      W.wxGen = wx.gen; W.wxShown = wx.valid;
      if (wx.valid) {
        const char* ic; lv_color_t col;
        wxIcon(wx.code, wx.isDay, ic, col);
        setTxt(W.wxIc, ic); setCol(W.wxIc, col);
        char t[12];
        snprintf(t, sizeof(t), "%d\xC2\xB0", (int)lroundf(wx.tempC));     // "31°"
        setTxt(W.wxT, t);
      } else { setTxt(W.wxIc, ""); setTxt(W.wxT, ""); }
    }
    // Widths of fixed templates, so the group does not jump as the digits change.
    // Centred on the whole width — or, while the CALL / LOG / MENU buttons are
    // there, on the space left of them.
    const int cw = clk ? lv_txt_get_width("00:00", 5, &ui42b, 0, LV_TEXT_FLAG_NONE) : 0;
    const int tw = lv_txt_get_width(wx.valid && fabsf(wx.tempC) >= 9.5f ? "00\xC2\xB0" : "0\xC2\xB0",
                                    wx.valid && fabsf(wx.tempC) >= 9.5f ? 4 : 3, &ui42b, 0, LV_TEXT_FLAG_NONE);
    const int ww = wx.valid ? 36 + 6 + tw : 0;
    const int group = cw + (cw && ww ? 24 : 0) + ww;
    const bool full = Store::cfg().kiosk || !Display::touchEnabled();
    const int area = full ? LCD_W : LCD_W - 8 - 44 * 3 - 12 - 8;
    const int x0 = (area - group) / 2;
    if (x0 != W.sbLx) {                           // (sbLx reused as "group x")
      W.sbLx = (int16_t)x0;
      lv_obj_set_x(W.clk, x0);
      const int wx0 = x0 + cw + (cw ? 24 : 0);
      lv_obj_set_x(W.wxIc, wx0);
      lv_obj_set_x(W.wxT, wx0 + 42);
    }
  }

  // ── floor number ──
  const char* lab = s.linkOk ? s.label : "--";
  const lv_font_t* f = (strlen(lab) >= 3) ? &fl140 : W.bigFont;
  if (f != W.floorFont) {
    W.floorFont = f;
    lv_obj_set_style_text_font(W.floor, f, 0);
    lv_obj_set_y(W.floor, f == W.bigFont ? W.floorYBig : W.floorY3);
  }
  setTxt(W.floor, lab);
  setCol(W.floor, !s.linkOk ? C_LINE : (m == LM_FIRE ? C_ALARM_HI : C_GOLD));

  // ── arrow, or the door pictogram while standing with the door open ──
  const bool doorShown = s.linkOk && s.door && !s.moving;
  const uint8_t ad = doorShown ? 3 : (s.linkOk ? s.dir : 0);
  const bool am = s.linkOk && s.moving;
  if (ad != s_arDir || am != s_arMove) {
    s_arDir = ad; s_arMove = am; s_arPhase = 0;
    lv_obj_invalidate(W.arrow);
  }

  // ── centre icon + number as ONE group ──
  // A fixed slot left the number off to the right whenever the label is a
  // single digit (most buildings). Measure the text, centre icon + gap +
  // number on the screen; with no icon the number alone is centred.
  {
    const lv_coord_t tw = lv_txt_get_width(lab, strlen(lab), f, 0, LV_TEXT_FLAG_NONE);
    const int gap = 20;
    const int group = (ad ? W.arrowW + gap : 0) + tw;
    const int x0 = (LCD_W - group) / 2;
    const int fx = ad ? x0 + W.arrowW + gap : x0;
    if (ad && x0 != W.arrowX) { W.arrowX = (int16_t)x0; lv_obj_set_x(W.arrow, x0); }
    if (fx != W.floorX || tw + 4 != W.floorW) {
      W.floorX = (int16_t)fx; W.floorW = (int16_t)(tw + 4);
      lv_obj_set_x(W.floor, fx);
      lv_obj_set_width(W.floor, tw + 4);
    }
  }

  // ── banner ──
  const char* ic = "";
  const char* tx = "";
  const char* sub = "";
  lv_color_t bg = C_SURF, fg = C_DIM;
  char t2[48];
  switch (m) {
    case LM_NOLINK:
      ic = IC_LINK_OFF;
      tx = s.seen ? "NO SIGNAL" : "WAITING...";
      snprintf(t2, sizeof(t2), s.seen ? "Lost the lift's CAN signal" : "Listening on CAN %uk",
               Can::bitrate());
      sub = t2;
      bg = C_FAULT_BG; fg = C_FAULT; break;
    case LM_FIRE:
      // Flash 1 Hz — same rhythm as the fire panel's alarm screen
      ic = IC_FIRE; tx = "FIRE SERVICE"; sub = "Do not use the lift";
      bg = ((millis() / 500) & 1) ? C_ALARM : C_ALARM_LO; fg = C_WHITE; break;
    case LM_FAULT:
      snprintf(b, sizeof(b), "FAULT %u", s.fault);
      ic = IC_WARNING; tx = b; sub = "Call maintenance";
      bg = C_FAULT; fg = C_BLACK; break;
    case LM_INSPECT:
      ic = IC_BUILD; tx = "INSPECTION"; sub = "Lift under maintenance";
      bg = C_ISO; fg = C_WHITE; break;
    case LM_OOS:
      ic = IC_BLOCK; tx = "OUT OF SERVICE"; sub = "This lift is not available";
      bg = C_SURF2; fg = C_TXT; break;
    case LM_OVERLOAD:
      ic = IC_WEIGHT; tx = "OVERLOAD"; sub = "Please step out of the car";
      bg = C_ALARM; fg = C_WHITE; break;
    default: {
      const char* what;
      if (s.moving)    { ic = s.dir == 2 ? IC_DOWN : IC_UP; what = s.dir == 2 ? "GOING DOWN" : s.dir == 1 ? "GOING UP" : "MOVING"; }
      else if (s.door) { ic = IC_DOOR; what = "DOOR OPEN"; }
      else if (s.dir)  { ic = s.dir == 2 ? IC_DOWN : IC_UP; what = s.dir == 2 ? "NEXT: DOWN" : "NEXT: UP"; }
      else             { ic = IC_CHECK; what = "READY"; }
      if (s.load >= 0) snprintf(b, sizeof(b), "%s  ·  LOAD %d %%", what, s.load);
      else             snprintf(b, sizeof(b), "%s", what);
      tx = b;
      fg = C_GOLD;
      break;
    }
  }
  if (!W.ban) return;                       // normal service: no banner
  setBg(W.ban, bg);
  setTxt(W.banIc, ic); setCol(W.banIc, fg);
  setTxt(W.banTx, tx); setCol(W.banTx, fg);
  if (W.banSub) { setTxt(W.banSub, sub); setCol(W.banSub, fg); }
}

// ═══════════════════════ CALL ═══════════════════════
#define CALL_PER_PAGE 18
static uint8_t s_callTab = 0;      // 0 car · 1 hall up · 2 hall down
static uint8_t s_callPage = 0;

static uint8_t callPages() { return (uint8_t)((Store::cfg().floors + CALL_PER_PAGE - 1) / CALL_PER_PAGE); }
static uint8_t tabCmd() { return s_callTab == 0 ? CMD_CAR : s_callTab == 1 ? CMD_HALL_UP : CMD_HALL_DN; }

static void refreshCall();

static void callTab(lv_event_t* e) { s_callTab = (uint8_t)(intptr_t)lv_event_get_user_data(e); UI::show(SCR_CALL); }
static void callPage(lv_event_t*)  { s_callPage = (uint8_t)((s_callPage + 1) % callPages()); UI::show(SCR_CALL); }
static void callFloor(lv_event_t* e) {
  const uint8_t k = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  if (k >= W.fbN) return;
  beepKey();
  Lift::command(tabCmd(), W.fbIdx[k], SRC_PANEL);
  refreshCall();
}
static void callDoor(lv_event_t* e) {
  beepKey();
  Lift::command((uint8_t)(intptr_t)lv_event_get_user_data(e), 0, SRC_PANEL);
  refreshCall();
}

static void buildCall() {
  char b[64];
  snprintf(b, sizeof(b), "CALL · %s", Store::cfg().liftName);
  statusBarClock(b);

  static const char* const TAB[3] = {"CAR", "HALL UP", "HALL DOWN"};
  for (uint8_t i = 0; i < 3; i++) {
    lv_obj_t* t = mkBtn(lv_scr_act(), i * 160, H_STATUSBAR, 159, 44, TAB[i],
                        i == s_callTab ? C_SURF2 : C_SURF, callTab, i);
    lv_obj_set_style_radius(t, 0, 0);
    if (i == s_callTab) {
      lv_obj_set_style_border_side(t, LV_BORDER_SIDE_BOTTOM, 0);
      lv_obj_set_style_border_color(t, C_OK, 0);
      lv_obj_set_style_border_width(t, 3, 0);
    }
  }
  W.callInfo = mkLbl1(lv_scr_act(), 10, 76, "", &ui12, C_DIM, 460, 16);

  // Highest floor top-left, like a car operating panel read from the top
  const uint8_t nF = Store::cfg().floors;
  if (s_callPage >= callPages()) s_callPage = 0;
  W.fbN = 0;
  for (uint8_t k = 0; k < CALL_PER_PAGE; k++) {
    const int pos = s_callPage * CALL_PER_PAGE + k;
    if (pos >= nF) break;
    const int idx = nF - 1 - pos;
    W.fbIdx[k] = (int8_t)idx;
    W.fb[k] = mkBtn(lv_scr_act(), 6 + (k % 6) * 79, 96 + (k / 6) * 55, 76, 52,
                    Store::label((uint8_t)idx), C_SURF2, callFloor, k, &ui22b, &W.fbL[k]);
    W.fbN++;
  }

  char pg[16];
  snprintf(pg, sizeof(pg), "PAGE %u/%u", s_callPage + 1, callPages());
  if (callPages() > 1) {
    const FootBtn f[5] = {
      {"OPEN",       C_SURF2, C_TXT, callDoor, CMD_DOOR_OPEN},
      {"CLOSE",      C_SURF2, C_TXT, callDoor, CMD_DOOR_CLOSE},
      {"HOLD",       C_SURF2, C_TXT, callDoor, CMD_DOOR_HOLD},
      {pg,           C_SURF2, C_TXT, callPage, 0},
      {"BACK",       C_SURF2, C_TXT, goTo,     SCR_MAIN},
    };
    footer(f, 5);
  } else {
    const FootBtn f[4] = {
      {"DOOR OPEN",  C_SURF2, C_TXT, callDoor, CMD_DOOR_OPEN},
      {"DOOR CLOSE", C_SURF2, C_TXT, callDoor, CMD_DOOR_CLOSE},
      {"DOOR HOLD",  C_SURF2, C_TXT, callDoor, CMD_DOOR_HOLD},
      {"BACK",       C_SURF2, C_TXT, goTo,     SCR_MAIN},
    };
    footer(f, 4);
  }
  refreshCall();
}

static void refreshCall() {
  const LiftState& s = Lift::st();
  CmdResult why = RES_OK;
  const bool ok = Lift::cmdAllowed(SRC_PANEL, &why);
  const uint64_t bm = s_callTab == 0 ? s.callCar : s_callTab == 1 ? s.callUp : s.callDn;
  const uint8_t nF = Store::cfg().floors;
  for (uint8_t k = 0; k < W.fbN; k++) {
    const int idx = W.fbIdx[k];
    const bool lit = (bm >> idx) & 1;
    const bool here = s.linkOk && idx == s.floorIdx;
    const bool bad = (s_callTab == 1 && idx == nF - 1) || (s_callTab == 2 && idx == 0);
    setBg(W.fb[k], lit ? C_OK : C_SURF2);
    const lv_coord_t bw = here ? 2 : 0;
    if (lv_obj_get_style_border_width(W.fb[k], 0) != bw) lv_obj_set_style_border_width(W.fb[k], bw, 0);
    setCol(W.fbL[k], (ok && !bad) ? C_TXT : C_DIM);
  }
  char b[120];
  if (!ok) {
    const char* hint = why == RES_TX_OFF ? " — enable in MENU > CAN BUS"
                     : why == RES_LISTEN ? " — enable ACK in MENU > CAN BUS"
                     : why == RES_MODE   ? Lift::modeText(Lift::mode()) : "";
    snprintf(b, sizeof(b), "Calls locked: %s%s%s", Lift::resultText(why),
             why == RES_MODE ? " · " : "", hint);
    setTxt(W.callInfo, b);
    setCol(W.callInfo, C_FAULT);
  } else if (Lift::lastCmdAt() && millis() - Lift::lastCmdAt() < 8000) {
    setTxt(W.callInfo, Lift::lastCmdText());
    setCol(W.callInfo, C_OK);
  } else {
    setTxt(W.callInfo, Lift::simOn() ? "Simulation — calls go to the virtual lift, not the bus."
                                     : "Touch a floor to call the car. Lit = call registered.");
    setCol(W.callInfo, Lift::simOn() ? C_ARROW : C_DIM);
  }
}

// ═══════════════════════ MENU ═══════════════════════
struct Tile { const char* ic; const char* t; const char* s; Screen to; };

static void simToggle(lv_event_t*) {
  if (Lift::simOn()) Lift::simStop(); else Lift::simStart();
  UI::show(SCR_MENU);
}

static void buildMenu() {
  statusBarClock("MENU");
  lv_obj_t* body = mkBox(lv_scr_act(), 0, H_STATUSBAR, LCD_W, H_BODY, C_BG);
  const Settings& c = Store::cfg();
  static char sub[10][32];
  snprintf(sub[0], 32, "%sk · %s", canRateLabel(c.canKbps), Can::modeActive() == CAN_NORMAL ? "ACK" : "listen");
  snprintf(sub[1], 32, "%s", Lift::protoText());
  snprintf(sub[2], 32, "%u floors", c.floors);
  snprintf(sub[3], 32, "%u IDs heard", Can::seenCount());
  if (!Net::on())               snprintf(sub[4], 32, "off");
  else if (Net::apMode())       snprintf(sub[4], 32, "own AP");
  else if (Net::staConnected()) snprintf(sub[4], 32, "%s", Net::ip().c_str());
  else                          snprintf(sub[4], 32, "joining...");
  snprintf(sub[5], 32, "%s", !Mqtt::on() ? "off" : Mqtt::connected() ? "connected" : "no broker");
  snprintf(sub[6], 32, "%u %%%s", c.brightness, c.kiosk ? " · display only" : "");
  snprintf(sub[7], 32, "level %u%s", c.volume, c.chime ? " · chime" : "");
  snprintf(sub[8], 32, "%u entries", Store::logCount());
  snprintf(sub[9], 32, "v" FW_VERSION);

  const Tile tl[10] = {
    {IC_CAN,     "CAN BUS",   sub[0], SCR_CAN},
    {IC_TUNE,    "PROTOCOL",  sub[1], SCR_MAP},
    {IC_LAYERS,  "FLOORS",    sub[2], SCR_FLOORS},
    {IC_SEARCH,  "FRAMES",   sub[3], SCR_SNIFF},
    {IC_WIFI,    "NETWORK",   sub[4], SCR_NET},
    {IC_HOME,    "HA · MQTT", sub[5], SCR_MQTT},
    {IC_DISPLAY, "DISPLAY",   sub[6], SCR_DISPLAY},
    {IC_VOLUME,  "SOUND",     sub[7], SCR_AUDIO},
    {IC_HISTORY, "LOG",       sub[8], SCR_LOG},
    {IC_INFO,    "SYSTEM",    sub[9], SCR_ABOUT},
  };
  for (uint8_t i = 0; i < 10; i++) {
    const int x = 8 + (i % 3) * 157, y = 4 + (i / 3) * 58;
    lv_obj_t* t = mkBox(body, x, y, 149, 56, C_SURF);
    lv_obj_set_style_radius(t, 5, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, C_LINE, 0);
    lv_obj_add_flag(t, LV_OBJ_FLAG_CLICKABLE);
    mkLbl(t, 10, 13, tl[i].ic, &ico28, C_DIM);
    mkLbl1(t, 46, 9,  tl[i].t, &ui14b, C_TXT, 98, 19);
    mkLbl1(t, 46, 31, tl[i].s, &ui12,  C_DIM, 98, 16);
    lv_obj_add_event_cb(t, goTo, LV_EVENT_CLICKED, (void*)(intptr_t)tl[i].to);
  }
  const FootBtn f[2] = {
    {"BACK", C_SURF2, C_TXT, goTo, SCR_MAIN},
    {Lift::simOn() ? "STOP SIMULATION" : "SIMULATION", Lift::simOn() ? C_ISO : C_SURF2, C_TXT, simToggle, 0},
  };
  footer(f, 2);
}

// ═══════════════════════ TECHNICIAN CODE ═══════════════════════
static uint16_t s_pinEntry = 0;
static uint8_t  s_pinDigits = 0;
static uint8_t  s_pinFails = 0;
// 0 = unlock · 1 = type a new code · 2 = type it again (one typo would lock
// the technician out of the settings for good)
static uint8_t  s_pinMode = 0;
static uint16_t s_pinNew  = 0;
static bool     s_pinMiss = false;
static lv_obj_t* s_pinDots[4];

static void pinPaint() {
  for (uint8_t i = 0; i < 4; i++)
    if (s_pinDots[i]) lv_obj_set_style_bg_color(s_pinDots[i], i < s_pinDigits ? C_TXT : C_SURF2, 0);
}

static void pinKey(lv_event_t* e) {
  const intptr_t k = (intptr_t)lv_event_get_user_data(e);
  if (k == 10) {                                    // DEL
    if (s_pinDigits) { s_pinDigits--; s_pinEntry /= 10; }
  } else if (k == 11) {                             // OK
    if (s_pinMode == 1) {
      if (s_pinDigits < 4 || s_pinEntry == 1234) {  // a code everybody knows is no lock
        s_pinMiss = (s_pinEntry == 1234);
        s_pinEntry = 0; s_pinDigits = 0;
        Out::beep(300, 300);
        UI::show(SCR_PIN);
        return;
      }
      s_pinNew = s_pinEntry; s_pinEntry = 0; s_pinDigits = 0;
      s_pinMode = 2; s_pinMiss = false;
      UI::show(SCR_PIN);
      return;
    }
    if (s_pinMode == 2) {
      if (s_pinDigits == 4 && s_pinEntry == s_pinNew) {
        Store::cfg().pin = s_pinNew;
        Store::save();
        Store::logAdd(EV_CONFIG);
        s_pinMode = 0; s_pinEntry = 0; s_pinDigits = 0; s_pinFails = 0;
        Out::beep(1800, 120);
        UI::show(SCR_ABOUT);
      } else {
        s_pinMode = 1; s_pinMiss = true;
        s_pinEntry = 0; s_pinDigits = 0;
        Out::beep(300, 300);
        UI::show(SCR_PIN);
      }
      return;
    }
    if (s_pinDigits == 4 && s_pinEntry == Store::cfg().pin) {
      Store::logAdd(EV_LOGIN, 0, 0, SRC_PANEL);
      s_pinEntry = 0; s_pinDigits = 0; s_pinFails = 0;
      UI::show(s_afterPin);
      return;
    }
    s_pinFails++;
    s_pinEntry = 0; s_pinDigits = 0;
    Out::beep(300, 300);
    UI::show(SCR_PIN);
    return;
  } else if (s_pinDigits < 4) {
    s_pinEntry = (uint16_t)(s_pinEntry * 10 + k);
    s_pinDigits++;
  }
  beepKey();
  pinPaint();
}

static void pinCancel(lv_event_t*) {
  const uint8_t m = s_pinMode;
  s_pinMode = 0; s_pinEntry = 0; s_pinDigits = 0; s_pinMiss = false;
  UI::show(m ? SCR_ABOUT : SCR_MAIN);
}

static void buildPin() {
  const uint8_t m = s_pinMode;
  statusBar(m == 1 ? "NEW CODE" : m == 2 ? "CONFIRM CODE" : "SETTINGS LOCKED", Clock::timeOnly());
  lv_obj_t* body = mkBox(lv_scr_act(), 0, H_STATUSBAR, LCD_W, H_BODY, C_BG);
  mkLbl(body, 10, 14, m == 1 ? "NEW CODE" : m == 2 ? "AGAIN" : "TECH CODE", &ui22b, C_TXT, 210);
  lv_obj_t* w = mkLbl(body, 10, 50,
      m == 1 ? "Four digits. This code locks the settings on the web page."
    : m == 2 ? "Type the new code again. A typo would lock you out."
             : "Enter the code.",
                      &ui12, C_DIM, 210);
  lv_label_set_long_mode(w, LV_LABEL_LONG_WRAP);
  for (uint8_t i = 0; i < 4; i++) {
    s_pinDots[i] = mkBox(body, 12 + i * 30, 124, 20, 20, C_SURF2);
    lv_obj_set_style_radius(s_pinDots[i], 10, 0);
  }
  const bool warn = (m == 0 && s_pinFails) || (m == 1 && s_pinMiss);
  mkLbl(body, 10, 160,
      m == 2 ? ""
    : m == 1 ? (s_pinMiss ? "Not accepted. Try another code." : "Do not use 1234.")
    : s_pinFails ? "Wrong code. Try again."
    : (Store::cfg().pin == 1234) ? "Still the default 1234 — change it."
                                 : "Enter the technician code.",
        &ui12, warn ? C_ALARM : C_DIM, 210);

  static const char* const lab[12] = {"1","2","3","4","5","6","7","8","9","DEL","0","OK"};
  static const intptr_t val[12] = {1,2,3,4,5,6,7,8,9,10,0,11};
  for (uint8_t i = 0; i < 12; i++)
    mkBtn(body, 230 + (i % 3) * 82, 8 + (i / 3) * 56, 78, 52, lab[i],
          i == 11 ? C_OK : C_SURF2, pinKey, val[i]);
  pinPaint();
  const FootBtn f[1] = {{"CANCEL", C_SURF2, C_TXT, pinCancel, 0}};
  footer(f, 1);
}

// ═══════════════════════ CAN BUS ═══════════════════════
static void canRate(lv_event_t* e) {
  const int d = (int)(intptr_t)lv_event_get_user_data(e);
  // From a measured (non-table) rate: step to the table rate just below / above it
  const uint32_t cur = canRateBps(Store::cfg().canKbps);
  int i = -1;
  for (uint8_t k = 0; k < CAN_KBPS_N; k++) if (CAN_KBPS[k] == Store::cfg().canKbps) i = k;
  if (i < 0) {
    i = (d > 0) ? (int)CAN_KBPS_N - 1 : 0;
    for (int k = 0; k < (int)CAN_KBPS_N; k++)
      if (d > 0 ? canRateBps(CAN_KBPS[k]) > cur : canRateBps(CAN_KBPS[k]) < cur) { if (d > 0) { i = k; break; } i = k; }
  } else i = constrain(i + d, 0, (int)CAN_KBPS_N - 1);
  Store::cfg().canKbps = CAN_KBPS[i];
  Can::setBitrate(CAN_KBPS[i]);
  Store::save();
  UI::show(SCR_CAN);
}
static void canAckDo(lv_event_t*) {
  Store::cfg().canMode = CAN_NORMAL;
  Can::setMode(CAN_NORMAL);
  Store::save();
  Store::logAdd(EV_CONFIG);
  UI::show(SCR_CAN);
}
static void canAck(lv_event_t*) {
  if (Store::cfg().canMode == CAN_NORMAL) {
    if (Can::fellBack()) { Can::clearFallback(); UI::show(SCR_CAN); return; }
    Store::cfg().canMode = CAN_LISTEN;
    Can::setMode(CAN_LISTEN);
    Store::save();
    UI::show(SCR_CAN);
    return;
  }
  askConfirm("Enable ACK mode?",
             "The display will acknowledge frames and send error frames like any node. "
             "It first listens for 20 clean frames, and falls back by itself if the bus "
             "reports errors. Only with the bit rate confirmed.",
             "ENABLE ACK", C_FAULT, canAckDo);
}
static void canTxDo(lv_event_t*) {
  Store::cfg().txEnable = true;
  Store::save();
  Store::logAdd(EV_CONFIG);
  UI::show(SCR_CAN);
}
static void canTx(lv_event_t*) {
  if (Store::cfg().txEnable) {
    Store::cfg().txEnable = false;
    Store::save();
    UI::show(SCR_CAN);
    return;
  }
  askConfirm("Send commands to the lift?",
             "Car calls and door commands from this panel will go onto the bus. "
             "Check the PROTOCOL command frames first, with someone watching the lift.",
             "ALLOW", C_FAULT, canTxDo);
}
static void canTimeout(lv_event_t* e) {
  const int d = (int)(intptr_t)lv_event_get_user_data(e);
  Store::cfg().timeoutSec = (uint8_t)constrain(Store::cfg().timeoutSec + d, TIMEOUT_MIN, TIMEOUT_MAX);
  Store::save();
  UI::show(SCR_CAN);
}
// Time the pulses on RX and, when they look like CAN, switch to the measured
// rate — also a non-standard one no table rate can match (listen-only first, as always).
static void canMeasure(lv_event_t*) {
  if (Lift::simOn()) { toast("Stop the simulation first (MENU)."); return; }
  Can::Probe p;
  if (!Can::probe(p)) { toast("Measurement failed to start."); return; }
  if (p.pulses < 20) { toast("Hardly any edges on RX: check CANH / CANL / GND."); return; }
  if (p.fit > 0.15f || p.rateBps < 10000 || p.rateBps > 1000000) {
    toast(p.uartBaud ? "Not CAN: looks like a UART / RS-485 line." : "Pulses do not look like CAN.");
    return;
  }
  // A table rate within 1 % is used as such, otherwise the measured one
  uint16_t code = 0;
  for (uint8_t k = 0; k < CAN_KBPS_N; k++) {
    const uint32_t b = canRateBps(CAN_KBPS[k]);
    if ((b > p.rateBps ? b - p.rateBps : p.rateBps - b) * 100 <= b) code = CAN_KBPS[k];
  }
  if (!code) code = (uint16_t)((p.rateBps + 5) / 10);
  if (!Can::rateOk(code)) { toast("Measured rate cannot be set on this chip."); return; }
  Store::cfg().canKbps = code;
  Can::setBitrate(code);
  Store::save();
  Store::logAdd(EV_CONFIG);
  static char t[64];
  snprintf(t, sizeof(t), "Measured %s kbit/s - set (listen-only).", canRateLabel(code));
  toast(t);
  UI::show(SCR_CAN);
}

static void canAuto(lv_event_t*) {
  if (Lift::simOn()) { toast("Stop the simulation first (MENU)."); return; }
  Can::autoBaudStart();
  UI::show(SCR_CAN);
}

static uint32_t canSignature() {
  return (uint32_t)Can::bitrate() << 8 | Can::modeActive() << 4 | Can::arming() << 3 |
         Can::fellBack() << 2 | Can::autoBauding() << 1 | Lift::simOn();
}

static void buildCan() {
  statusBar("CAN BUS · TWAI", Can::stateText());
  lv_obj_t* body = mkBox(lv_scr_act(), 0, H_STATUSBAR, LCD_W, H_BODY, C_BG);
  const Settings& c = Store::cfg();
  char b[48];
  snprintf(b, sizeof(b), "%s kbit/s", canRateLabel(c.canKbps));
  mkFormRow(body, 0, "Bit rate", b, canRate);

  const char* mv = c.canMode == CAN_LISTEN ? "LISTEN-ONLY (invisible)"
                 : Can::fellBack() ? "ACK wanted · FELL BACK"
                 : Can::arming()   ? "ACK · listening first"
                 :                   "ACK · may send";
  const char* mb = c.canMode == CAN_LISTEN ? "ENABLE ACK" : Can::fellBack() ? "RETRY ACK" : "LISTEN ONLY";
  lv_obj_t* v = mkToggleRow(body, 42, "Mode", mv, mb, c.canMode == CAN_LISTEN ? C_SURF2 : C_FAULT_BG, canAck);
  if (Can::fellBack()) setCol(v, C_FAULT);

  mkToggleRow(body, 84, "Send commands", c.txEnable ? "ON" : "off (safe)",
              c.txEnable ? "TURN OFF" : "ALLOW", c.txEnable ? C_FAULT_BG : C_SURF2, canTx);
  snprintf(b, sizeof(b), "%u s without a status frame", c.timeoutSec);
  mkFormRow(body, 126, "Signal timeout", b, canTimeout);

  W.can1 = mkLbl1(body, 10, 174, "", &ui12, C_DIM, 460, 16);
  W.can2 = mkLbl1(body, 10, 194, "", &ui12, C_DIM, 460, 16);
  W.can3 = mkLbl1(body, 10, 214, "", &ui12, C_DIM, 460, 16);
  W.canSig = canSignature();

  const FootBtn f[4] = {
    {"MEASURE",     C_SURF2, C_TXT, canMeasure, 0},
    {"DETECT",      C_SURF2, C_TXT, canAuto,    0},
    {"RAW FRAMES",  C_SURF2, C_TXT, goTo,       SCR_SNIFF},
    {"BACK",        C_SURF2, C_TXT, goTo,       SCR_MENU},
  };
  footer(f, 4);
}

static void refreshCan() {
  if (canSignature() != W.canSig) { showNow(SCR_CAN); return; }
  Can::Stats s; Can::stats(s);
  char b[96];
  setTxt(W.sbR, Can::stateText());
  snprintf(b, sizeof(b), "Received %lu frames · %u frames/s · %u IDs%s", (unsigned long)s.rx, s.fps,
           Can::seenCount(), CanDebug::clients() ? " · web debug open" : "");
  setTxt(W.can1, b);
  snprintf(b, sizeof(b), "Bus errors %lu · queue overruns %lu · TEC %u · REC %u",
           (unsigned long)s.busErr, (unsigned long)s.rxMissed, s.tec, s.rec);
  setTxt(W.can2, b);
  setCol(W.can2, (s.busErr || s.rxMissed) ? C_FAULT : C_DIM);
  if (Can::autoBauding()) {
    const uint8_t st = Can::autoBaudStep();
    snprintf(b, sizeof(b), "Detecting... trying %u kbit/s (%u of %u)",
             CAN_KBPS[st < CAN_KBPS_N ? st : CAN_KBPS_N - 1], st + 1, (unsigned)CAN_KBPS_N);
    setCol(W.can3, C_ARROW);
  } else if (toastOn()) {
    snprintf(b, sizeof(b), "%s", s_toast);
    setCol(W.can3, C_FAULT);
  } else if (Can::fellBack()) {
    snprintf(b, sizeof(b), "Too many bus errors in ACK mode — back to LISTEN-ONLY.");
    setCol(W.can3, C_FAULT);
  } else {
    snprintf(b, sizeof(b), "Sent %lu frames (%lu failed) · arbitration lost %lu",
             (unsigned long)s.tx, (unsigned long)s.txFail, (unsigned long)s.arbLost);
    setCol(W.can3, C_DIM);
  }
  setTxt(W.can3, b);
}

// ═══════════════════════ RAW FRAMES ═══════════════════════
static uint32_t s_frId = 0;
static bool     s_frExt = false;
static uint8_t  s_frByte = 0, s_frMask = 0;

static void bytesTxt(const Can::Seen& s, char* b, size_t n) {
  // Bytes that changed in the last 2 s are amber: run the lift and the floor
  // byte lights up on every floor, the door bit on every door cycle.
  const uint32_t now = millis();
  size_t k = 0;
  b[0] = 0;
  for (uint8_t i = 0; i < s.f.dlc && k + 16 < n; i++) {
    const bool hot = s.chgAt[i] && now - s.chgAt[i] < 2000;
    k += snprintf(b + k, n - k, hot ? "#F0A500 %02X# " : "%02X ", s.f.d[i]);
  }
  if (!s.f.dlc) snprintf(b, n, "(no data)");
}

static void sigsOn(uint32_t id, bool ext, char* b, size_t n) {
  size_t k = 0;
  b[0] = 0;
  for (uint8_t s = 0; s < SIG_COUNT && k + 20 < n; s++) {
    const SigMap& g = Store::map().sig[s];
    if ((g.flags & MAP_USED) && g.id == id && ((g.flags & MAP_EXT) != 0) == ext)
      k += snprintf(b + k, n - k, "%s%s", k ? ", " : "", SIG_NAME[s]);
  }
  if (!k) snprintf(b, n, "not mapped");
}

static void snRow(lv_event_t* e) {
  const Can::Seen* s = Can::seenAt((uint8_t)(intptr_t)lv_event_get_user_data(e));
  if (!s) return;
  s_frId = s->f.id; s_frExt = s->f.ext; s_frByte = 0; s_frMask = 0;
  UI::show(SCR_FRAME);
}
static void snClear(lv_event_t*) { Can::seenClear(); UI::show(SCR_SNIFF); }
static void snPause(lv_event_t*) { Can::setPaused(!Can::paused()); UI::show(SCR_SNIFF); }

static void buildSniff() {
  char b[64];
  snprintf(b, sizeof(b), "RAW FRAMES · %u IDs%s", Can::seenCount(),
           Can::seenCount() >= SEEN_MAX ? " (table full)" : "");
  statusBar(b, Can::paused() ? "PAUSED" : Lift::simOn() ? "simulation" : Can::stateText());
  lv_obj_t* list = mkList(H_STATUSBAR, H_BODY);
  const uint8_t n = Can::seenCount();
  W.snGen = Can::seenGen();
  W.snN = 0;
  if (!n) {
    mkRow(list, nullptr, "No frames heard yet",
          "Check CANH/CANL, transceiver power and the bit rate (CAN BUS > DETECT RATE)",
          nullptr, C_DIM, nullptr, 0);
  }
  char lead[12], bytes[160], sub[80], right[16];
  for (uint8_t i = 0; i < n; i++) {
    if (!listHasRoom()) break;
    const Can::Seen* s = Can::seenAt(i);
    snprintf(lead, sizeof(lead), s->f.ext ? "%08lX" : "%03lX", (unsigned long)s->f.id);
    bytesTxt(*s, bytes, sizeof(bytes));
    char sg[60];
    sigsOn(s->f.id, s->f.ext, sg, sizeof(sg));
    snprintf(sub, sizeof(sub), "DLC %u · %s", s->f.dlc, sg);
    snprintf(right, sizeof(right), "%u/s", s->rate);
    mkRow(list, lead, bytes, sub, right, C_TXT, snRow, i, nullptr, &W.snMain[i], &W.snRight[i]);
    lv_label_set_recolor(W.snMain[i], true);
    W.snN++;
  }
  const FootBtn f[3] = {
    {"CLEAR", C_SURF2, C_TXT, snClear, 0},
    {Can::paused() ? "RESUME" : "PAUSE", Can::paused() ? C_ISO : C_SURF2, C_TXT, snPause, 0},
    {"BACK",  C_SURF2, C_TXT, goTo, SCR_CAN},
  };
  footer(f, 3);
}

static void refreshSniff() {
  if (Can::seenGen() != W.snGen) { showNow(SCR_SNIFF); return; }   // rows added/removed
  char bytes[160], right[16];
  for (uint8_t i = 0; i < W.snN; i++) {
    const Can::Seen* s = Can::seenAt(i);
    if (!s) break;
    bytesTxt(*s, bytes, sizeof(bytes));
    setTxt(W.snMain[i], bytes);
    snprintf(right, sizeof(right), "%u/s", s->rate);
    setTxt(W.snRight[i], right);
  }
}

// ═══════════════════════ ONE FRAME — byte & bit inspector ═══════════════════════
// The on-panel way to learn an unknown protocol: run the lift, watch which
// byte follows the floor, tap it, ASSIGN BYTE → FLOOR. Same for bits.
static void refreshFrame();

static void frByte(lv_event_t* e) { s_frByte = (uint8_t)(intptr_t)lv_event_get_user_data(e); s_frMask = 0; refreshFrame(); }
static void frBit(lv_event_t* e) {
  const uint8_t m = (uint8_t)(1u << (intptr_t)lv_event_get_user_data(e));
  s_frMask = (s_frMask == m) ? 0 : m;
  refreshFrame();
}

static void assignSig(uint8_t s, uint8_t mask, uint8_t enc) {
  LiftMap& m = Store::map();
  SigMap& g = m.sig[s];
  g.id = s_frId;
  g.flags = (uint8_t)(MAP_USED | (s_frExt ? MAP_EXT : 0));
  g.byte = s_frByte;
  g.mask = mask;
  if (s == SIG_FLOOR) g.enc = enc;
  m.preset = PRESET_CUSTOM;
  Store::saveMap();
  Store::logAdd(EV_CONFIG);
  Mqtt::rediscover();
  char b[96];
  snprintf(b, sizeof(b), "%s = ID %lX byte %u%s", SIG_NAME[s], (unsigned long)s_frId, s_frByte,
           s == SIG_FLOOR ? " · set the offset in FLOORS" : "");
  toast(b);
  s_wantCloseModal = true;
}
static void frAssignByteCb(lv_event_t* e) {
  const intptr_t a = (intptr_t)lv_event_get_user_data(e);
  if (a <= 2)      assignSig(SIG_FLOOR, 0xFF, (uint8_t)a);
  else if (a == 3) assignSig(SIG_FAULT, 0xFF, 0);
  else if (a == 4) assignSig(SIG_LOAD, 0xFF, 0);
  else             assignSig((uint8_t)(SIG_CALL_CAR + (a - 5)), 0xFF, 0);
}
static void frAssignBitCb(lv_event_t* e) {
  assignSig((uint8_t)(SIG_UP + (intptr_t)lv_event_get_user_data(e)), s_frMask, 0);
}
static void frAssignByte(lv_event_t*) {
  static const Choice c[8] = {
    {"FLOOR (binary)", frAssignByteCb, 0}, {"FLOOR (BCD)", frAssignByteCb, 1},
    {"FLOOR (2 ASCII chars)", frAssignByteCb, 2}, {"FAULT CODE", frAssignByteCb, 3},
    {"LOAD %", frAssignByteCb, 4}, {"CAR CALLS", frAssignByteCb, 5},
    {"HALL CALLS UP", frAssignByteCb, 6}, {"HALL CALLS DOWN", frAssignByteCb, 7},
  };
  char t[48];
  snprintf(t, sizeof(t), "Assign ID %lX byte %u as...", (unsigned long)s_frId, s_frByte);
  askChoice(t, c, 8);
}
static void frAssignBit(lv_event_t*) {
  if (!s_frMask) { toast("Tap a bit first."); refreshFrame(); return; }
  static const Choice c[8] = {
    {"UP", frAssignBitCb, 0}, {"DOWN", frAssignBitCb, 1}, {"MOVING", frAssignBitCb, 2},
    {"DOOR OPEN", frAssignBitCb, 3}, {"FIRE SERVICE", frAssignBitCb, 4},
    {"INSPECTION", frAssignBitCb, 5}, {"OVERLOAD", frAssignBitCb, 6},
    {"OUT OF SERVICE", frAssignBitCb, 7},
  };
  char t[48];
  snprintf(t, sizeof(t), "Assign byte %u mask %02X as...", s_frByte, s_frMask);
  askChoice(t, c, 8);
}

static void buildFrame() {
  char b[48];
  snprintf(b, sizeof(b), s_frExt ? "ID %08lX (29-bit)" : "ID %03lX", (unsigned long)s_frId);
  statusBar(b, "");
  for (uint8_t i = 0; i < 8; i++) {
    W.byBox[i] = mkBox(lv_scr_act(), 6 + i * 59, 34, 55, 60, C_SURF);
    lv_obj_set_style_radius(W.byBox[i], 4, 0);
    lv_obj_set_style_border_width(W.byBox[i], 1, 0);
    lv_obj_set_style_border_color(W.byBox[i], C_LINE, 0);
    lv_obj_add_flag(W.byBox[i], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(W.byBox[i], frByte, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    char c[4];
    snprintf(c, sizeof(c), "B%u", i);
    lv_obj_t* cl = mkLbl(W.byBox[i], 0, 5, c, &ui12, C_DIM, 53);
    lv_obj_set_style_text_align(cl, LV_TEXT_ALIGN_CENTER, 0);
    W.byVal[i] = mkLbl(W.byBox[i], 0, 25, "--", &ui22b, C_TXT, 53);
    lv_obj_set_style_text_align(W.byVal[i], LV_TEXT_ALIGN_CENTER, 0);
  }
  for (uint8_t j = 0; j < 8; j++) {                  // bit 7 on the left, like on paper
    const uint8_t bit = (uint8_t)(7 - j);
    W.biBox[j] = mkBox(lv_scr_act(), 6 + j * 59, 100, 55, 46, C_SURF);
    lv_obj_set_style_radius(W.biBox[j], 4, 0);
    lv_obj_set_style_border_width(W.biBox[j], 1, 0);
    lv_obj_set_style_border_color(W.biBox[j], C_LINE, 0);
    lv_obj_add_flag(W.biBox[j], LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(W.biBox[j], frBit, LV_EVENT_CLICKED, (void*)(intptr_t)bit);
    char c[4];
    snprintf(c, sizeof(c), "b%u", bit);
    lv_obj_t* cl = mkLbl(W.biBox[j], 0, 3, c, &ui12, C_DIM, 53);
    lv_obj_set_style_text_align(cl, LV_TEXT_ALIGN_CENTER, 0);
    W.biVal[j] = mkLbl(W.biBox[j], 0, 17, "0", &ui22b, C_DIM, 53);
    lv_obj_set_style_text_align(W.biVal[j], LV_TEXT_ALIGN_CENTER, 0);
  }
  W.fr1 = mkLbl1(lv_scr_act(), 10, 154, "", &ui14, C_TXT, 460, 19);
  W.fr2 = mkLbl1(lv_scr_act(), 10, 176, "", &ui12, C_DIM, 460, 16);
  lv_obj_t* h = mkLbl(lv_scr_act(), 10, 198,
      "Run the lift: the byte that follows the floor is FLOOR; the bit that turns on "
      "with the door is DOOR OPEN. Tap a byte, then a bit, then ASSIGN.", &ui12, C_DIM, 460);
  lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
  const FootBtn f[3] = {
    {"ASSIGN BYTE", C_SURF2, C_TXT, frAssignByte, 0},
    {"ASSIGN BIT",  C_SURF2, C_TXT, frAssignBit,  0},
    {"BACK",        C_SURF2, C_TXT, goTo,         SCR_SNIFF},
  };
  footer(f, 3);
  refreshFrame();
}

static void refreshFrame() {
  const int si = Can::seenFind(s_frId, s_frExt);
  const Can::Seen* s = si >= 0 ? Can::seenAt((uint8_t)si) : nullptr;
  const uint32_t now = millis();
  char b[120];
  snprintf(b, sizeof(b), s ? "DLC %u · %u frames/s" : "not heard", s ? s->f.dlc : 0, s ? s->rate : 0);
  setTxt(W.sbR, b);
  for (uint8_t i = 0; i < 8; i++) {
    const bool have = s && i < s->f.dlc;
    char v[4];
    if (have) snprintf(v, sizeof(v), "%02X", s->f.d[i]); else snprintf(v, sizeof(v), "--");
    setTxt(W.byVal[i], v);
    const bool hot = have && s->chgAt[i] && now - s->chgAt[i] < 2000;
    setCol(W.byVal[i], !have ? C_DIM : hot ? C_FAULT : C_TXT);
    const bool sel = (i == s_frByte);
    const lv_coord_t bw = sel ? 2 : 1;
    if (lv_obj_get_style_border_width(W.byBox[i], 0) != bw) lv_obj_set_style_border_width(W.byBox[i], bw, 0);
    lv_obj_set_style_border_color(W.byBox[i], sel ? C_OK : C_LINE, 0);
  }
  const uint8_t val = (s && s_frByte < s->f.dlc) ? s->f.d[s_frByte] : 0;
  for (uint8_t j = 0; j < 8; j++) {
    const uint8_t bit = (uint8_t)(7 - j);
    const bool on = (val >> bit) & 1;
    setTxt(W.biVal[j], on ? "1" : "0");
    setCol(W.biVal[j], on ? C_TXT : C_DIM);
    setBg(W.biBox[j], (s_frMask >> bit) & 1 ? C_OK_BG : C_SURF);
    lv_obj_set_style_border_color(W.biBox[j], (s_frMask >> bit) & 1 ? C_OK : C_LINE, 0);
  }
  if (s && s_frByte < s->f.dlc) {
    const bool bcdOk = (val >> 4) <= 9 && (val & 0x0F) <= 9;
    const uint32_t age = s->chgAt[s_frByte] ? (now - s->chgAt[s_frByte]) / 1000 : 0;
    if (bcdOk)
      snprintf(b, sizeof(b), "Byte %u = 0x%02X = %u · as BCD %u · changed %lus ago", s_frByte, val,
               val, (unsigned)((val >> 4) * 10 + (val & 0x0F)), (unsigned long)age);
    else
      snprintf(b, sizeof(b), "Byte %u = 0x%02X = %u · changed %lus ago", s_frByte, val, val,
               (unsigned long)age);
  } else {
    snprintf(b, sizeof(b), "Byte %u not present in this frame", s_frByte);
  }
  setTxt(W.fr1, b);

  if (toastOn()) { setTxt(W.fr2, s_toast); setCol(W.fr2, C_OK); return; }
  size_t k = 0;
  b[0] = 0;
  for (uint8_t g = 0; g < SIG_COUNT && k + 30 < sizeof(b); g++) {
    const SigMap& m = Store::map().sig[g];
    if ((m.flags & MAP_USED) && m.id == s_frId && ((m.flags & MAP_EXT) != 0) == s_frExt &&
        m.byte == s_frByte)
      k += snprintf(b + k, sizeof(b) - k, "%s%s", k ? ", " : "Mapped here: ", SIG_NAME[g]);
  }
  if (!k) snprintf(b, sizeof(b), "Nothing mapped at this byte yet.");
  setTxt(W.fr2, b);
  setCol(W.fr2, C_DIM);
}

// ═══════════════════════ PROTOCOL MAP ═══════════════════════
static uint8_t s_mapSel = 0;

static void mapUnmap(lv_event_t*) {
  Store::map().sig[s_mapSel].flags = 0;
  Store::map().preset = PRESET_CUSTOM;
  Store::saveMap();
  Mqtt::rediscover();
  UI::show(SCR_MAP);
}
static void mapInvert(lv_event_t*) {
  Store::map().sig[s_mapSel].flags ^= MAP_INV;
  Store::map().preset = PRESET_CUSTOM;
  Store::saveMap();
  UI::show(SCR_MAP);
}
static void mapRow(lv_event_t* e) {
  s_mapSel = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  if (!(Store::map().sig[s_mapSel].flags & MAP_USED)) {
    toast("Map signals from RAW FRAMES, or on the web page.");
    return;
  }
  const bool bit = sigIsBit(s_mapSel);
  const Choice c[2] = {
    {"UNMAP", mapUnmap, 0},
    {(Store::map().sig[s_mapSel].flags & MAP_INV) ? "NORMAL LEVEL" : "INVERT", mapInvert, 0},
  };
  askChoice(SIG_NAME[s_mapSel], c, bit ? 2 : 1);
}
static void mapEvlDo(lv_event_t*) {
  Lift::presetEvl(Store::map());
  Store::saveMap();
  Store::logAdd(EV_CONFIG);
  Mqtt::rediscover();
  UI::show(SCR_MAP);
}
static void mapClearDo(lv_event_t*) {
  Lift::presetClear(Store::map());
  Store::saveMap();
  Store::logAdd(EV_CONFIG);
  Mqtt::rediscover();
  UI::show(SCR_MAP);
}
static void mapCoDo(lv_event_t*) {
  Lift::presetCia417(Store::map());
  Store::saveMap();
  Store::logAdd(EV_CONFIG);
  Mqtt::rediscover();
  UI::show(SCR_MAP);
}
static void mapEvl(lv_event_t*) {
  askConfirm("Load the EVL-CAN template?", "Replaces every signal and command in the map "
             "with the project's own EVL-CAN protocol (see README).", "LOAD", C_ISO, mapEvlDo);
}
static void mapCo(lv_event_t*) {
  askConfirm("Use CiA 417 (CANopen-Lift)?", "Built-in decoder for CiA 417 lifts; the signal map "
             "is emptied. CiA 417 normally runs at 250 kbit/s (CAN screen).", "USE CiA 417", C_ISO, mapCoDo);
}
static void mapFujiDo(lv_event_t*) {
  Lift::presetFuji(Store::map());
  Store::saveMap();
  Store::logAdd(EV_CONFIG);
  Mqtt::rediscover();
  UI::show(SCR_MAP);
}
static void mapFuji(lv_event_t*) {
  askConfirm("Load the Fuji map?", "Floor, arrows, running, door and the three call types of the "
             "Fuji hall-call bus (27.78 kbit/s). Replaces the signal map.", "LOAD FUJI", C_ISO, mapFujiDo);
}
static void mapCoTap(lv_event_t*) {
  toast("Lift number, node-ID and floor numbering: web page /cfg.");
}

// One call-controller record in words: "position 3 on", "hall call up 5 off"…
static void coRecText(const uint8_t* r, char* b, size_t n) {
  const char* on = (r[5] & 1) ? "on" : "off";
  switch (r[0]) {
    case 0x40: snprintf(b, n, "position %u %s", r[3] != 0xFF ? r[3] : r[1], on); break;
    case 0x42: snprintf(b, n, "arrow %s %s", (r[1] & 1) ? "up" : (r[1] & 2) ? "down" : "?", on); break;
    case 0x02: snprintf(b, n, "hall call %s %u %s", (r[1] == 1) ? "up" : (r[1] == 2) ? "down" : "up+dn", r[3], on); break;
    case 0x05: snprintf(b, n, "car call %u %s", r[3], on); break;
    default:   snprintf(b, n, "function %02X · %02X %02X %02X %02X %02X", r[0], r[1], r[2], r[3], r[4], r[5]);
  }
}
static void mapClear(lv_event_t*) {
  askConfirm("Clear the protocol map?", "Every signal and command is removed. The display "
             "shows NO SIGNAL until signals are mapped again.", "CLEAR ALL", C_ALARM, mapClearDo);
}

static void buildMap() {
  const LiftMap& m = Store::map();
  statusBar("PROTOCOL", Lift::protoText());
  lv_obj_t* list = mkList(H_STATUSBAR, H_BODY);
  char sub[96];
  if (m.proto == PROTO_CIA417) {
    snprintf(sub, sizeof(sub), "lift %u · lowest floor = %d on the bus · calls sent on ID %03X",
             m.coLift, m.coBase, Lift::coCallId());
    mkRow(list, nullptr, "CiA 417 · CANopen-Lift", sub, "", C_TXT, mapCoTap, 0, nullptr,
          &W.coM[0], &W.coR[0]);
    mkRow(list, nullptr, "-", "CANopen nodes heard (heartbeat)", "", C_TXT, nullptr, 0, nullptr,
          &W.coM[1], &W.coR[1]);
    mkRow(list, nullptr, "-", "last record from the call controller", "", C_TXT, nullptr, 0, nullptr,
          &W.coM[2], &W.coR[2]);
  }
  for (uint8_t s = 0; s < SIG_COUNT; s++) {
    const SigMap& g = m.sig[s];
    const bool used = g.flags & MAP_USED;
    if (!used) snprintf(sub, sizeof(sub), "not mapped");
    else {
      int k = snprintf(sub, sizeof(sub), (g.flags & MAP_EXT) ? "ID %08lX · byte %u" : "ID %03lX · byte %u",
                       (unsigned long)g.id, g.byte);
      if (sigIsBit(s)) k += snprintf(sub + k, sizeof(sub) - k, " · mask %02X%s", g.mask,
                                     (g.flags & MAP_INV) ? " · inverted" : "");
      if (s == SIG_FLOOR) k += snprintf(sub + k, sizeof(sub) - k, " · %s · offset %d",
                                        FENC_NAME[g.enc % FENC_COUNT], g.offset);
      if (sigIsBitmap(s) && g.offset) snprintf(sub + k, sizeof(sub) - k, " · bit offset %d", g.offset);
    }
    if (!listHasRoom()) break;
    mkRow(list, nullptr, SIG_NAME[s], sub, "", used ? C_TXT : C_DIM, mapRow, s, nullptr,
          nullptr, &W.mpR[s]);
  }
  for (uint8_t c = 0; c < CMD_COUNT; c++) {
    const CmdMap& k = m.cmd[c];
    char main[40];
    snprintf(main, sizeof(main), "Command: %s", CMD_NAME[c]);
    if (m.proto == PROTO_CIA417 && c <= CMD_HALL_DN) {
      snprintf(sub, sizeof(sub), "CiA 417 call request · ID %03X · %s", Lift::coCallId(),
               c == CMD_CAR ? "05 00 …" : c == CMD_HALL_UP ? "02 01 …" : "02 02 …");
      if (!listHasRoom()) break;
      mkRow(list, nullptr, main, sub, nullptr, C_TXT, nullptr, 0);
      continue;
    }
    if (!(k.flags & MAP_USED)) snprintf(sub, sizeof(sub), "not defined — edit on the web page /cfg");
    else {
      int n = snprintf(sub, sizeof(sub), "ID %lX ·", (unsigned long)k.id);
      for (uint8_t j = 0; j < k.dlc && j < 8; j++) n += snprintf(sub + n, sizeof(sub) - n, " %02X", k.data[j]);
      if (k.fenc != CENC_NONE) snprintf(sub + n, sizeof(sub) - n, " · floor in B%u (%s)", k.fbyte,
                                        CENC_NAME[k.fenc % CENC_COUNT]);
    }
    if (!listHasRoom()) break;
    mkRow(list, nullptr, main, sub, nullptr, (k.flags & MAP_USED) ? C_TXT : C_DIM, nullptr, 0);
  }
  const FootBtn f[5] = {
    {"FUJI",     C_SURF2, C_TXT, mapFuji,  0},
    {"CiA 417",  C_SURF2, C_TXT, mapCo,    0},
    {"EVL-CAN",  C_SURF2, C_TXT, mapEvl,   0},
    {"CLEAR",    C_SURF2, C_TXT, mapClear, 0},
    {"BACK",     C_SURF2, C_TXT, goTo,     SCR_MENU},
  };
  footer(f, 5);
}

static void refreshMap() {
  char b[24];
  if (W.coM[1]) {
    Lift::CoInfo o;
    Lift::coInfo(o);
    char t[64];
    snprintf(b, sizeof(b), "%u nodes", o.nodes);
    setTxt(W.coR[0], b);
    setCol(W.coR[0], o.nodes ? C_OK : C_DIM);
    setTxt(W.coM[1], o.nodes ? o.nodeList : "none — not a CANopen bus, or wrong bit rate");
    setCol(W.coM[1], o.nodes ? C_TXT : C_DIM);
    snprintf(b, sizeof(b), "HB %lu", (unsigned long)o.hb);
    setTxt(W.coR[1], b);
    if (o.lastRecAge == UINT32_MAX) {
      setTxt(W.coM[2], "nothing on the call controller ID yet");
      setCol(W.coM[2], C_DIM);
      setTxt(W.coR[2], "");
    } else {
      coRecText(o.lastRec, t, sizeof(t));
      setTxt(W.coM[2], t);
      setCol(W.coM[2], C_TXT);
      snprintf(b, sizeof(b), "%lu rec · %lus", (unsigned long)o.rec, (unsigned long)(o.lastRecAge / 1000));
      setTxt(W.coR[2], b);
    }
  }
  for (uint8_t s = 0; s < SIG_COUNT; s++) {
    if (!W.mpR[s]) continue;
    int32_t v; uint32_t age;
    if (!(Store::map().sig[s].flags & MAP_USED) || !Lift::sigValue(s, v, age)) { setTxt(W.mpR[s], ""); continue; }
    if (sigIsBitmap(s))      snprintf(b, sizeof(b), "%lX", (unsigned long)v);
    else if (s == SIG_FLOOR && Store::map().sig[s].enc == FENC_ASCII)
                             snprintf(b, sizeof(b), "\"%c%c\"", (char)(v >> 8) > ' ' ? (char)(v >> 8) : ' ',
                                      (char)(v & 0xFF) > ' ' ? (char)(v & 0xFF) : ' ');
    else                     snprintf(b, sizeof(b), "%ld", (long)v);
    if (age > 5000) strncat(b, " (old)", sizeof(b) - strlen(b) - 1);
    setTxt(W.mpR[s], b);
    setCol(W.mpR[s], age > 5000 ? C_DIM : C_ARROW);
  }
  if (toastOn()) setTxt(W.sbR, s_toast);
}

// ═══════════════════════ FLOORS ═══════════════════════
static void flCount(lv_event_t* e) {
  Settings& c = Store::cfg();
  c.floors = (uint8_t)constrain(c.floors + (int)(intptr_t)lv_event_get_user_data(e), FLOORS_MIN, MAX_FLOORS);
  if (c.mainFloor >= c.floors) c.mainFloor = 0;
  Store::save();
  Mqtt::rediscover();
  UI::show(SCR_FLOORS);
}
// CiA 417 without a mapped FLOOR signal: the floor numbering of the CiA 417 records
static bool flCoBase() {
  return Store::map().proto == PROTO_CIA417 && !(Store::map().sig[SIG_FLOOR].flags & MAP_USED);
}
static void flOffset(lv_event_t* e) {
  const int d = (int)(intptr_t)lv_event_get_user_data(e);
  LiftMap& m = Store::map();
  if (flCoBase()) m.coBase = (int8_t)constrain(m.coBase + d, OFFSET_MIN, OFFSET_MAX);
  SigMap& g = m.sig[SIG_FLOOR];
  if (!flCoBase()) g.offset = (int8_t)constrain(g.offset + d, OFFSET_MIN, OFFSET_MAX);
  Store::saveMap();
  UI::show(SCR_FLOORS);
}
static void flMain(lv_event_t* e) {
  Settings& c = Store::cfg();
  c.mainFloor = (uint8_t)((c.mainFloor + c.floors + (int)(intptr_t)lv_event_get_user_data(e)) % c.floors);
  Store::save();
  UI::show(SCR_FLOORS);
}
static void flBase(lv_event_t* e) {
  Settings& c = Store::cfg();
  c.basements = (uint8_t)constrain(c.basements + (int)(intptr_t)lv_event_get_user_data(e), 0, 9);
  Store::save();
  UI::show(SCR_FLOORS);
}
static void flGround(lv_event_t* e) {
  Settings& c = Store::cfg();
  c.groundStyle = (uint8_t)((c.groundStyle + 4 + (int)(intptr_t)lv_event_get_user_data(e)) % 4);
  Store::save();
  UI::show(SCR_FLOORS);
}
static void flApplyDo(lv_event_t*) {
  Store::autoLabels();
  Mqtt::rediscover();
  UI::show(SCR_FLOORS);
}
static void flApply(lv_event_t*) {
  askConfirm("Rename every floor?", "Labels are generated from the basement count and the "
             "ground floor name. Labels typed by hand are overwritten.", "RENAME", C_ISO, flApplyDo);
}

static void buildFloors() {
  const Settings& c = Store::cfg();
  statusBar("FLOORS", "");
  lv_obj_t* body = mkBox(lv_scr_act(), 0, H_STATUSBAR, LCD_W, H_BODY, C_BG);
  char b[48];
  snprintf(b, sizeof(b), "%u  (%s ... %s)", c.floors, Store::label(0), Store::label(c.floors - 1));
  mkFormRow(body, 0, "Floors served", b, flCount);
  snprintf(b, sizeof(b), "%d%s", flCoBase() ? Store::map().coBase : Store::map().sig[SIG_FLOOR].offset,
           flCoBase() ? "  (CiA 417 floor no.)" : "");
  mkFormRow(body, 42, "Bus value of lowest", b, flOffset);
  snprintf(b, sizeof(b), "%s", Store::label(c.mainFloor));
  mkFormRow(body, 84, "Main (fire) floor", b, flMain);
  snprintf(b, sizeof(b), "%u", c.basements);
  mkFormRow(body, 126, "Basements", b, flBase);
  static const char* const G[4] = {"\"1\"", "\"G\"", "\"L\"", "\"0\""};
  mkFormRow(body, 168, "Ground floor called", G[c.groundStyle & 3], flGround);
  W.flLive = mkLbl1(body, 10, 214, "", &ui12, C_DIM, 460, 16);
  const FootBtn f[3] = {
    {"APPLY NAMES", C_SURF2, C_TXT, flApply, 0},
    {"EDIT LABELS", C_SURF2, C_TXT, goTo,    SCR_FLOORLIST},
    {"BACK",        C_SURF2, C_TXT, goTo,    SCR_MENU},
  };
  footer(f, 3);
}

static void refreshFloors() {
  const LiftState& s = Lift::st();
  char b[96];
  const bool co = Store::map().proto == PROTO_CIA417;
  if (co && s.rawFloor == INT32_MIN)
    snprintf(b, sizeof(b), "CiA 417: no position record from the call controller yet.");
  else if (!co && !(Store::map().sig[SIG_FLOOR].flags & MAP_USED))
    snprintf(b, sizeof(b), "The FLOOR signal is not mapped yet (PROTOCOL / RAW FRAMES).");
  else if (s.rawFloor == INT32_MIN)
    snprintf(b, sizeof(b), "Nothing received on the FLOOR signal yet.");
  else if (s.floorIdx < 0)
    snprintf(b, sizeof(b), "Bus sends %ld -> out of range: adjust the bus value of lowest.", (long)s.rawFloor);
  else
    snprintf(b, sizeof(b), "Bus sends %ld -> floor #%d, label \"%s\"", (long)s.rawFloor, s.floorIdx, s.label);
  setTxt(W.flLive, b);
  setCol(W.flLive, s.floorIdx < 0 ? C_FAULT : C_OK);
}

// ═══════════════════════ KEYBOARD (raw ASCII) ═══════════════════════
static char*   s_kbBuf  = nullptr;
static size_t  s_kbCap  = 0;
static Screen  s_kbBack = SCR_MENU;
static bool    s_kbUpper = false;    // caps lock (floor labels)
static bool    s_kbShift = false;
static char    s_kbTitle[40];
static char    s_kbOrig[72];
static void  (*s_kbDone)() = nullptr;
static lv_obj_t* s_kbField = nullptr;

static void openKeyboard(char* buf, size_t cap, const char* title, bool upper, Screen back,
                         void (*done)()) {
  s_kbBuf = buf; s_kbCap = cap; s_kbBack = back; s_kbDone = done;
  s_kbUpper = upper; s_kbShift = upper;
  strncpy(s_kbOrig, buf, sizeof(s_kbOrig) - 1);
  s_kbOrig[sizeof(s_kbOrig) - 1] = 0;
  strncpy(s_kbTitle, title, sizeof(s_kbTitle) - 1);
  s_kbTitle[sizeof(s_kbTitle) - 1] = 0;
  UI::show(SCR_KEYBOARD);
}

static void kbPaint() {
  if (!s_kbField || !s_kbBuf) return;
  char t[80];
  snprintf(t, sizeof(t), "%s|", s_kbBuf);
  lv_label_set_text(s_kbField, t);
}

static void kbKey(lv_event_t* e) {
  const char c = (char)(intptr_t)lv_event_get_user_data(e);
  if (!s_kbBuf) return;
  const size_t n = strlen(s_kbBuf);
  if (c == 1) {                                   // DEL
    if (n) s_kbBuf[n - 1] = 0;
  } else if (c == 2) {                            // SHIFT
    s_kbShift = !s_kbShift;
    UI::show(SCR_KEYBOARD);
    return;
  } else if (c == 3) {                            // DONE
    if (s_kbDone) s_kbDone();
    s_kbDone = nullptr;
    UI::show(s_kbBack);
    return;
  } else if (c == 5) {                            // CANCEL: give the old text back
    strncpy(s_kbBuf, s_kbOrig, s_kbCap - 1);
    s_kbBuf[s_kbCap - 1] = 0;
    s_kbDone = nullptr;
    UI::show(s_kbBack);
    return;
  } else {
    char k = (s_kbShift && c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
    // Shift (not caps lock) turns the digit row into symbols, like a PC
    // keyboard — WiFi passwords nearly always contain one.
    if (s_kbShift && !s_kbUpper && c >= '0' && c <= '9') k = ")!@#$%^&*("[c - '0'];
    if (n + 1 < s_kbCap) { s_kbBuf[n] = k; s_kbBuf[n + 1] = 0; }
    if (s_kbShift && !s_kbUpper) s_kbShift = false;
  }
  kbPaint();
}

static void kbBtn(lv_obj_t* p, int x, int y, int w, int h, const char* lab, intptr_t code,
                  lv_color_t bg) {
  mkBtn(p, x, y, w, h, lab, bg, kbKey, code);
}

static void buildKeyboard() {
  char r[40];
  snprintf(r, sizeof(r), "%s%u / %u chars", s_kbUpper ? "CAPS · " : "",
           s_kbBuf ? (unsigned)strlen(s_kbBuf) : 0, (unsigned)(s_kbCap ? s_kbCap - 1 : 0));
  statusBar(s_kbTitle, r);
  lv_obj_t* body = mkBox(lv_scr_act(), 0, H_STATUSBAR, LCD_W, LCD_H - H_STATUSBAR, C_BG);
  lv_obj_t* f = mkBox(body, 6, 4, 468, 38, C_SURF2);
  lv_obj_set_style_radius(f, 4, 0);
  lv_obj_set_style_border_width(f, 1, 0);
  lv_obj_set_style_border_color(f, C_OK, 0);
  s_kbField = mkLbl(f, 10, 5, "", &ui22b, C_TXT, 440);
  kbPaint();

  static const char* R0 = "1234567890";
  static const char* R1 = "qwertyuiop";
  static const char* R2 = "asdfghjkl";
  static const char* R3 = "zxcvbnm";
  const int KH = 47, G = 3, Y0 = 44;
  char k1[2] = {0, 0};
  for (int i = 0; i < 10; i++) {
    k1[0] = (s_kbShift && !s_kbUpper) ? ")!@#$%^&*("[R0[i] - '0'] : R0[i];
    kbBtn(body, 6 + i * 47, Y0, 44, KH, k1, R0[i], C_SURF);
  }
  const int y1 = Y0 + (KH + G);
  for (int i = 0; i < 10; i++) {
    k1[0] = s_kbShift ? (char)(R1[i] - 32) : R1[i];
    kbBtn(body, 6 + i * 47, y1, 44, KH, k1, R1[i], C_SURF2);
  }
  const int y2 = Y0 + 2 * (KH + G);
  for (int i = 0; i < 9; i++) {
    k1[0] = s_kbShift ? (char)(R2[i] - 32) : R2[i];
    kbBtn(body, 6 + i * 47, y2, 44, KH, k1, R2[i], C_SURF2);
  }
  kbBtn(body, 6 + 9 * 47, y2, 44, KH, ":", ':', C_SURF2);
  const int y3 = Y0 + 3 * (KH + G);
  kbBtn(body, 6, y3, 66, KH, "SHIFT", 2, s_kbShift ? C_OK : C_SURF2);
  for (int i = 0; i < 7; i++) {
    k1[0] = s_kbShift ? (char)(R3[i] - 32) : R3[i];
    kbBtn(body, 75 + i * 47, y3, 44, KH, k1, R3[i], C_SURF2);
  }
  kbBtn(body, 75 + 7 * 47, y3, 70, KH, "DEL", 1, lv_color_hex(0x3A2226));
  // CANCEL far left, DONE far right — as far apart as possible, one throws the
  // text away and the other keeps it
  const int y4 = Y0 + 4 * (KH + G);
  kbBtn(body, 6,   y4, 66,  KH, "CANCEL", 5,  lv_color_hex(0x3A2226));
  kbBtn(body, 75,  y4, 44,  KH, "-",      '-', C_SURF2);
  kbBtn(body, 124, y4, 103, KH, "space",  ' ', C_SURF2);
  kbBtn(body, 232, y4, 44,  KH, ".",      '.', C_SURF2);
  kbBtn(body, 281, y4, 44,  KH, "_",      '_', C_SURF2);
  kbBtn(body, 330, y4, 144, KH, "DONE",   3,   C_OK);
}

// ═══════════════════════ FLOOR LABELS ═══════════════════════
static uint8_t s_lblIdx = 0;
static char    s_lblBuf[LABEL_LEN];

static void lblDone() { Store::setLabel(s_lblIdx, s_lblBuf); Mqtt::rediscover(); }
static void lblRow(lv_event_t* e) {
  s_lblIdx = (uint8_t)(intptr_t)lv_event_get_user_data(e);
  snprintf(s_lblBuf, sizeof(s_lblBuf), "%s", Store::label(s_lblIdx));
  openKeyboard(s_lblBuf, sizeof(s_lblBuf), "FLOOR LABEL (max 3)", true, SCR_FLOORLIST, lblDone);
}

static void buildFloorList() {
  statusBar("FLOOR LABELS", "tap to edit");
  lv_obj_t* list = mkList(H_STATUSBAR, H_BODY);
  char lead[8];
  const Settings& c = Store::cfg();
  for (int i = c.floors - 1; i >= 0; i--) {
    if (!listHasRoom()) break;
    snprintf(lead, sizeof(lead), "#%d", i);
    mkRow(list, lead, Store::label((uint8_t)i),
          i == c.mainFloor ? "main floor (fire recall)" : i == 0 ? "lowest floor" : nullptr,
          nullptr, C_TXT, lblRow, i);
  }
  const FootBtn f[1] = {{"BACK", C_SURF2, C_TXT, goTo, SCR_FLOORS}};
  footer(f, 1);
}

// ═══════════════════════ NETWORK ═══════════════════════
static char s_netSsid[33];
static char s_netPass[65];

static void netToggle(lv_event_t*) { Net::setEnabled(!Net::on()); UI::show(SCR_NET); }
static void netEditSsid(lv_event_t*) {
  snprintf(s_netSsid, sizeof(s_netSsid), "%s", Net::ssid());
  openKeyboard(s_netSsid, sizeof(s_netSsid), "WiFi NETWORK NAME", false, SCR_NET, nullptr);
}
static void netEditPass(lv_event_t*) {
  s_netPass[0] = 0;                     // always typed fresh, the old one is never shown
  openKeyboard(s_netPass, sizeof(s_netPass), "WiFi PASSWORD", false, SCR_NET, nullptr);
}
static void netApply(lv_event_t*) { if (s_netSsid[0]) Net::setCreds(s_netSsid, s_netPass); UI::show(SCR_NET); }
static void netScanGo(lv_event_t*) { Net::scanStart(); UI::show(SCR_WIFISCAN); }
static void netPick(lv_event_t* e) {
  int r; bool lk;
  if (!Net::scanInfo((int)(intptr_t)lv_event_get_user_data(e), s_netSsid, sizeof(s_netSsid), r, lk)) return;
  s_netPass[0] = 0;
  if (!lk) { Net::setCreds(s_netSsid, s_netPass); UI::show(SCR_NET); return; }
  // Picked → ask for the password straight away
  openKeyboard(s_netPass, sizeof(s_netPass), "WiFi PASSWORD", false, SCR_NET, []() {
    Net::setCreds(s_netSsid, s_netPass);
  });
}

static void buildWifiScan() {
  const int n = Net::scanState();
  char b[48];
  if (n == -1)    snprintf(b, sizeof(b), "scanning...");
  else if (n < 0) snprintf(b, sizeof(b), "not scanned");
  else            snprintf(b, sizeof(b), "%d networks", n);
  statusBar("CHOOSE WiFi NETWORK", b);
  lv_obj_t* list = mkList(H_STATUSBAR, H_BODY);
  if (n == -1) {
    mkRow(list, nullptr, "Scanning...", "Non-blocking scan — CAN keeps running", nullptr, C_DIM, nullptr, 0);
  } else if (n <= 0) {
    mkRow(list, nullptr, "No networks found", "Tap SCAN AGAIN, or move closer to the access point",
          nullptr, C_DIM, nullptr, 0);
  } else {
    char ssid[33], lead[8], right[16];
    for (int i = 0; i < n && i < 20; i++) {
      int r; bool lk;
      if (!Net::scanInfo(i, ssid, sizeof(ssid), r, lk)) continue;
      if (!listHasRoom()) break;
      const int bars = r > -55 ? 4 : r > -67 ? 3 : r > -78 ? 2 : 1;
      snprintf(lead, sizeof(lead), "%.*s", bars, "||||");
      snprintf(right, sizeof(right), "%d dBm", r);
      mkRow(list, lead, ssid, lk ? "password protected" : "OPEN network — anyone can join",
            right, lk ? C_TXT : C_FAULT, netPick, i);
    }
  }
  const FootBtn f[2] = {
    {"SCAN AGAIN", C_OK,    C_WHITE, netScanGo, 0},
    {"BACK",       C_SURF2, C_TXT,   goTo,      SCR_NET},
  };
  footer(f, 2);
}

static void buildNet() {
  char b[80];
  snprintf(b, sizeof(b), "NETWORK · %s", !Net::on() ? "off" : Net::apMode() ? "own AP"
           : Net::staConnected() ? "joined" : "joining...");
  statusBar(b, Net::on() ? Net::ip().c_str() : "-");
  lv_obj_t* body = mkBox(lv_scr_act(), 0, H_STATUSBAR, LCD_W, H_BODY, C_BG);
  mkToggleRow(body, 0, "WiFi", Net::on() ? "ON" : "off", Net::on() ? "TURN OFF" : "TURN ON",
              Net::on() ? C_OK : C_SURF2, netToggle);
  mkTapRow(body, 42, "Network", s_netSsid[0] ? s_netSsid : (Net::ssid()[0] ? Net::ssid() : "(tap to type)"), netEditSsid);
  mkTapRow(body, 84, "Password", s_netPass[0] ? "********  (tap to retype)" : "(tap to type)", netEditPass);
  const char* l1; const char* l2; lv_color_t col = C_DIM;
  char t1[80];
  if (Net::on() && Net::apMode()) {
    l1 = "Cannot join the site WiFi — running own network LIFT-DISPLAY";
    l2 = "password liftdisplay · open http://192.168.4.1/ to fix it";
    col = C_FAULT;
  } else if (Net::staConnected()) {
    snprintf(t1, sizeof(t1), "Open http://%s/ in a browser — signal %d dBm", Net::ip().c_str(), Net::rssi());
    l1 = t1;
    l2 = "Status page, settings, protocol map, log. MQTT: HOME ASSIST. in the menu.";
    col = C_OK;
  } else {
    l1 = "WiFi is optional: with it off the display still reads CAN,";
    l2 = "shows the floor and sends calls from this panel exactly the same.";
  }
  mkLbl1(body, 10, 136, l1, &ui12, col, 460, 16);
  mkLbl1(body, 10, 156, l2, &ui12, C_DIM, 460, 16);
  W.netSig = (int8_t)(Net::on() ? (Net::staConnected() ? 1 : Net::apMode() ? 2 : 0) : 3);
  const FootBtn f[3] = {
    {"SCAN",    C_SURF2, C_TXT,   netScanGo, 0},
    {"CONNECT", C_OK,    C_WHITE, netApply,  0},
    {"BACK",    C_SURF2, C_TXT,   goTo,      SCR_MENU},
  };
  footer(f, 3);
}

// ═══════════════════════ HOME ASSISTANT · MQTT ═══════════════════════
static char s_mqHostBuf[56];
static char s_mqUserBuf[32];
static char s_mqPassBuf[48];

static void mqToggle(lv_event_t*) { Mqtt::setEnabled(!Mqtt::on()); UI::show(SCR_MQTT); }
static void mqHostDone() {
  char* c = strrchr(s_mqHostBuf, ':');
  uint16_t port = 1883;
  if (c) { *c = 0; port = (uint16_t)atoi(c + 1); }
  Mqtt::setServer(s_mqHostBuf, port);
}
static void mqHost(lv_event_t*) {
  if (Mqtt::host()[0]) snprintf(s_mqHostBuf, sizeof(s_mqHostBuf), "%s:%u", Mqtt::host(), Mqtt::port());
  else s_mqHostBuf[0] = 0;
  openKeyboard(s_mqHostBuf, sizeof(s_mqHostBuf), "BROKER  host:port", false, SCR_MQTT, mqHostDone);
}
static void mqUserDone() { Mqtt::setAuth(s_mqUserBuf, nullptr); }
static void mqUser(lv_event_t*) {
  snprintf(s_mqUserBuf, sizeof(s_mqUserBuf), "%s", Mqtt::user());
  openKeyboard(s_mqUserBuf, sizeof(s_mqUserBuf), "MQTT USER", false, SCR_MQTT, mqUserDone);
}
static void mqPassDone() { Mqtt::setAuth(Mqtt::user(), s_mqPassBuf); }
static void mqPass(lv_event_t*) {
  s_mqPassBuf[0] = 0;
  openKeyboard(s_mqPassBuf, sizeof(s_mqPassBuf), "MQTT PASSWORD", false, SCR_MQTT, mqPassDone);
}
static void mqRemoteDo(lv_event_t*) {
  Store::cfg().allowRemote = true;
  Store::save();
  Store::logAdd(EV_CONFIG);
  Mqtt::rediscover();               // control entities appear in HA
  UI::show(SCR_MQTT);
}
static void mqRemote(lv_event_t*) {
  if (Store::cfg().allowRemote) {
    Store::cfg().allowRemote = false;
    Store::save();
    Mqtt::rediscover();             // control entities are removed from HA
    UI::show(SCR_MQTT);
    return;
  }
  askConfirm("Allow remote commands?",
             "Home Assistant and the web page (with the code) will be able to call the car "
             "and open the door. They still pass every lock the panel has.",
             "ALLOW", C_FAULT, mqRemoteDo);
}
static void mqDisc(lv_event_t*) { Mqtt::rediscover(); toast("Discovery sent again."); }

static void buildMqtt() {
  statusBar("HOME ASSISTANT · MQTT", Mqtt::baseTopic());
  lv_obj_t* body = mkBox(lv_scr_act(), 0, H_STATUSBAR, LCD_W, H_BODY, C_BG);
  mkToggleRow(body, 0, "MQTT", Mqtt::on() ? "ON" : "off", Mqtt::on() ? "TURN OFF" : "TURN ON",
              Mqtt::on() ? C_OK : C_SURF2, mqToggle);
  char b[64];
  if (Mqtt::host()[0]) snprintf(b, sizeof(b), "%s:%u", Mqtt::host(), Mqtt::port());
  else snprintf(b, sizeof(b), "(tap to set)");
  mkTapRow(body, 42, "Broker", b, mqHost);
  mkTapRow(body, 84, "User", Mqtt::user()[0] ? Mqtt::user() : "(none)", mqUser);
  mkTapRow(body, 126, "Password", Mqtt::hasPass() ? "********  (tap to change)" : "(none)", mqPass);
  mkToggleRow(body, 168, "Remote commands",
              Store::cfg().allowRemote ? "ON — HA + web can call" : "off (recommended)",
              Store::cfg().allowRemote ? "TURN OFF" : "ALLOW",
              Store::cfg().allowRemote ? C_FAULT_BG : C_SURF2, mqRemote);
  W.mqSt = mkLbl1(body, 10, 214, "", &ui12, C_DIM, 460, 16);
  const FootBtn f[2] = {
    {"RESEND DISCOVERY", C_SURF2, C_TXT, mqDisc, 0},
    {"BACK",             C_SURF2, C_TXT, goTo,   SCR_MENU},
  };
  footer(f, 2);
}

static void refreshMqtt() {
  char b[96];
  lv_color_t c = C_DIM;
  if (toastOn())                     { snprintf(b, sizeof(b), "%s", s_toast); c = C_OK; }
  else if (!Mqtt::on())              snprintf(b, sizeof(b), "MQTT is off.");
  else if (!Mqtt::host()[0])         { snprintf(b, sizeof(b), "Set the broker address first."); c = C_FAULT; }
  else if (!Net::staConnected())     { snprintf(b, sizeof(b), "Needs the site WiFi (MENU > NETWORK)."); c = C_FAULT; }
  else if (Mqtt::connected())        { snprintf(b, sizeof(b), "Connected · %lu messages sent · entities appear in HA automatically",
                                                (unsigned long)Mqtt::sentCount()); c = C_OK; }
  else if (Mqtt::lastError()[0])     { snprintf(b, sizeof(b), "%s", Mqtt::lastError()); c = C_FAULT; }
  else                               snprintf(b, sizeof(b), "Connecting to %s:%u...", Mqtt::host(), Mqtt::port());
  setTxt(W.mqSt, b);
  setCol(W.mqSt, c);
}

// ═══════════════════════ DISPLAY ═══════════════════════
static void dspBright(lv_event_t* e) {
  Settings& c = Store::cfg();
  c.brightness = (uint8_t)constrain(c.brightness + BRIGHT_STEP * (int)(intptr_t)lv_event_get_user_data(e),
                                    BRIGHT_MIN, BRIGHT_MAX);
  Display::setBrightness(c.brightness);
  Store::save();
  UI::show(SCR_DISPLAY);
}
static void dspDimSec(lv_event_t* e) {
  Settings& c = Store::cfg();
  c.dimSec = (uint16_t)constrain((int)c.dimSec + DIMSEC_STEP * (int)(intptr_t)lv_event_get_user_data(e), 0, DIMSEC_MAX);
  Display::setSleepTimeout(c.dimSec * 1000UL);
  Store::save();
  UI::show(SCR_DISPLAY);
}
static void dspDimPct(lv_event_t* e) {
  Settings& c = Store::cfg();
  c.dimPct = (uint8_t)constrain(c.dimPct + DIMPCT_STEP * (int)(intptr_t)lv_event_get_user_data(e), DIMPCT_MIN, DIMPCT_MAX);
  Display::setDimLevel(c.dimPct);
  Store::save();
  UI::show(SCR_DISPLAY);
}
static void dspKiosk(lv_event_t*) {
  Store::cfg().kiosk = !Store::cfg().kiosk;
  Store::save();
  UI::show(SCR_DISPLAY);
}
static void dspLed(lv_event_t*) {
  Store::cfg().statusLed = !Store::cfg().statusLed;
  Out::setLedEnabled(Store::cfg().statusLed);
  Store::save();
  UI::show(SCR_DISPLAY);
}
static void dspCal(lv_event_t*) { s_wantCal = true; }
static void dspTouchOffDo(lv_event_t*) {
  Store::cfg().touchOff = true;
  Display::setTouchEnabled(false);
  Store::save();
  Store::logAdd(EV_CONFIG);
  UI::show(SCR_MAIN);
}
static void dspTouchOff(lv_event_t*) {
  askConfirm("Turn the touch screen off?", "The panel will ignore every touch; the lift display keeps "
             "working. Turn it back on on the web page /cfg (Display) or over USB: touch 1",
             "TURN OFF", C_ALARM, dspTouchOffDo);
}

static void buildDisplay() {
  const Settings& c = Store::cfg();
  statusBar("DISPLAY", "");
  lv_obj_t* body = mkBox(lv_scr_act(), 0, H_STATUSBAR, LCD_W, H_BODY, C_BG);
  char b[40];
  snprintf(b, sizeof(b), "%u %%", c.brightness);
  mkFormRow(body, 0, "Brightness", b, dspBright);
  if (!c.dimSec) snprintf(b, sizeof(b), "never");
  else if (c.dimSec % 60) snprintf(b, sizeof(b), "%u s idle", c.dimSec);
  else snprintf(b, sizeof(b), "%u min idle", c.dimSec / 60);
  mkFormRow(body, 42, "Sleep after", b, dspDimSec);
  if (!c.dimPct) snprintf(b, sizeof(b), "backlight off");
  else snprintf(b, sizeof(b), "%u %%", c.dimPct);
  mkFormRow(body, 84, "Sleep brightness", b, dspDimPct);
  mkToggleRow(body, 126, "Display only", c.kiosk ? "ON — no buttons" : "off",
              c.kiosk ? "TURN OFF" : "TURN ON", c.kiosk ? C_OK : C_SURF2, dspKiosk);
  mkToggleRow(body, 168, "Status LED", c.statusLed ? "ON" : "off", c.statusLed ? "TURN OFF" : "TURN ON",
              c.statusLed ? C_OK : C_SURF2, dspLed);
  if (!Display::touchCalibrated())
    mkLbl1(body, 10, 214, "Touch not calibrated — using defaults, buttons may be off.", &ui12, C_FAULT, 460, 16);
  else
    mkLbl1(body, 10, 214, "Display only hides the buttons; hold the screen 2.5 s to open the menu.", &ui12, C_DIM, 460, 16);
  const FootBtn f[3] = {
    {"CALIBRATE",     C_SURF2, C_TXT, dspCal,      0},
    {"DISABLE TOUCH", C_SURF2, C_TXT, dspTouchOff, 0},
    {"BACK",          C_SURF2, C_TXT, goTo,        SCR_MENU},
  };
  footer(f, 3);
}

// ═══════════════════════ SOUND ═══════════════════════
static void audVol(lv_event_t* e) {
  Settings& c = Store::cfg();
  c.volume = (uint8_t)constrain(c.volume + (int)(intptr_t)lv_event_get_user_data(e), VOL_MIN, VOL_MAX);
  Out::setVolume(c.volume);
  Out::beep(1600, 120);
  Store::save();
  UI::show(SCR_AUDIO);
}
static void audChime(lv_event_t*) { Store::cfg().chime = !Store::cfg().chime; Store::save(); UI::show(SCR_AUDIO); }
static void audKey(lv_event_t*)   { Store::cfg().keyBeep = !Store::cfg().keyBeep; Store::save(); UI::show(SCR_AUDIO); }
static void audTest(lv_event_t* e) { Out::chime((uint8_t)(intptr_t)lv_event_get_user_data(e)); }
static void audApi(lv_event_t*) { Store::cfg().audioApi = !Store::cfg().audioApi; Store::save(); UI::show(SCR_AUDIO); }

static void buildAudio() {
  const Settings& c = Store::cfg();
  statusBar("SOUND", "");
  lv_obj_t* body = mkBox(lv_scr_act(), 0, H_STATUSBAR, LCD_W, H_BODY, C_BG);
  char b[24];
  snprintf(b, sizeof(b), "%u / 10", c.volume);
  mkFormRow(body, 0, "Volume", b, audVol);
  mkToggleRow(body, 42, "Arrival chime", c.chime ? "ON" : "off", c.chime ? "TURN OFF" : "TURN ON",
              c.chime ? C_OK : C_SURF2, audChime);
  mkToggleRow(body, 84, "Key beep", c.keyBeep ? "ON" : "off", c.keyBeep ? "TURN OFF" : "TURN ON",
              c.keyBeep ? C_OK : C_SURF2, audKey);
  mkToggleRow(body, 126, "Announcements", c.audioApi ? "ON — HTTP port 81" : "off", c.audioApi ? "TURN OFF" : "TURN ON",
              c.audioApi ? C_OK : C_SURF2, audApi);
  char ab[96];
  snprintf(ab, sizeof(ab), "POST a WAV to http://%s:%d/play  ·  %s", Net::ip().c_str(), AUDIO_PORT, Audio::lastText());
  mkLbl1(body, 10, 178, ab, &ui12, C_DIM, 460, 16);
  lv_obj_t* n = mkLbl(body, 10, 200,
      "Chime: one stroke = the car will go up, two strokes = down.", &ui12, C_DIM, 460);
  lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
  const FootBtn f[3] = {
    {"TEST UP",   C_SURF2, C_TXT, audTest, 1},
    {"TEST DOWN", C_SURF2, C_TXT, audTest, 2},
    {"BACK",      C_SURF2, C_TXT, goTo,    SCR_MENU},
  };
  footer(f, 3);
}

// ═══════════════════════ LOG ═══════════════════════
static void logExport(lv_event_t*) { s_wantExport = 1; Out::beep(1600, 80); }
static void logClearDo(lv_event_t*) { Store::logClear(); UI::show(SCR_LOG); }
static void logClear(lv_event_t*) {
  char msg[120];
  snprintf(msg, sizeof(msg), "%u entries are lost for good. EXPORT CSV over USB first if they matter.",
           Store::logCount());
  askConfirm("Clear the event log?", msg, "CLEAR LOG", C_ALARM, logClearDo);
}
static Screen s_logFrom = SCR_MAIN;
static void logBack(lv_event_t*) { UI::show(s_logFrom); }

static void buildLog() {
  char b[48];
  if (s_prev != SCR_LOG && s_prev != SCR_PIN) s_logFrom = (s_prev == SCR_MENU) ? SCR_MENU : SCR_MAIN;
  snprintf(b, sizeof(b), "EVENT LOG · %u entries", Store::logCount());
  statusBarClock(b);
  lv_obj_t* list = mkList(H_STATUSBAR, H_BODY);
  const uint16_t n = Store::logCount();
  if (!n) mkRow(list, nullptr, "No events yet", nullptr, nullptr, C_DIM, nullptr, 0);
  char day[8], hm[10], tx[80];
  for (uint16_t i = 0; i < n && i < 40; i++) {
    if (!listHasRoom()) break;
    const Event& e = Store::logAt(i);
    Store::logStampShort(e, day, sizeof(day), hm, sizeof(hm));
    Store::logText(e, tx, sizeof(tx));
    const lv_color_t c = (e.kind == EV_FIRE || e.kind == EV_LINK_LOST || e.kind == EV_BUSOFF ||
                          e.kind == EV_FALLBACK) ? C_ALARM
                       : (e.kind == EV_FAULT || e.kind == EV_CMD_DENY || e.kind == EV_OVERLOAD) ? C_FAULT
                       : (e.kind == EV_INSPECT || e.kind == EV_OOS) ? C_ISO : C_TXT;
    mkRow(list, day, tx, nullptr, nullptr, c, nullptr, 0, hm);
  }
  const FootBtn f[3] = {
    {"EXPORT CSV", C_SURF2, C_TXT, logExport, 0},
    {"CLEAR LOG",  C_SURF2, C_TXT, logClear,  0},
    {"BACK",       C_SURF2, C_TXT, logBack,   0},
  };
  footer(f, 3);
}

// ═══════════════════════ SYSTEM ═══════════════════════
uint32_t loopMaxUs();                      // main.cpp

static void abPin(lv_event_t*) { s_pinMode = 1; s_pinMiss = false; s_pinEntry = 0; s_pinDigits = 0; UI::show(SCR_PIN); }
static void abFactoryDo(lv_event_t*) {
  Store::factoryReset();
  delay(150);
  ESP.restart();
}
static void abFactory(lv_event_t*) {
  askConfirm("Factory reset?", "Settings, protocol map, floor labels and the event log are "
             "erased, then the display restarts. WiFi and MQTT settings are kept.",
             "ERASE ALL", C_ALARM, abFactoryDo);
}

static void buildAbout() {
  statusBar("SYSTEM", "v" FW_VERSION);
  lv_obj_t* body = mkBox(lv_scr_act(), 0, H_STATUSBAR, LCD_W, H_BODY, C_BG);
  for (uint8_t i = 0; i < 6; i++) W.ab[i] = mkLbl1(body, 12, 10 + i * 36, "", &ui14, C_TXT, 456, 19);
  const FootBtn f[4] = {
    {"CHANGE CODE", C_SURF2, C_TXT, abPin,     0},
    {"SET CLOCK",   C_SURF2, C_TXT, goTo,      SCR_CLOCK},
    {"RESET ALL",   C_SURF2, C_TXT, abFactory, 0},
    {"BACK",        C_SURF2, C_TXT, goTo,      SCR_MENU},
  };
  footer(f, 4);
}

static void refreshAbout() {
  char b[100];
  snprintf(b, sizeof(b), "Firmware " FW_VERSION " · ESP32-3248S035 · built-in TWAI CAN controller");
  setTxt(W.ab[0], b);
  snprintf(b, sizeof(b), "Network: %s · MQTT: %s", Net::on() ? Net::ip().c_str() : "off",
           !Mqtt::on() ? "off" : Mqtt::connected() ? "connected" : "not connected");
  setTxt(W.ab[1], b);
  snprintf(b, sizeof(b), "Memory: %u KB free (lowest %u KB) · slowest loop %lu ms",
           (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024),
           (unsigned long)(loopMaxUs() / 1000));
  setTxt(W.ab[2], b);
  const uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000ULL);   // no wrap at 49.7 days
  Can::Stats cs; Can::stats(cs);
  snprintf(b, sizeof(b), "Uptime %lud %02lu:%02lu:%02lu · CAN frames received %lu",
           (unsigned long)(up / 86400), (unsigned long)(up / 3600 % 24),
           (unsigned long)(up / 60 % 60), (unsigned long)(up % 60), (unsigned long)cs.rx);
  setTxt(W.ab[3], b);
  snprintf(b, sizeof(b), "Clock: %s (%s)", Clock::stamp(), Net::timeSource());
  setTxt(W.ab[4], b);
  setTxt(W.ab[5], Store::cfg().pin == 1234 ? "Technician code is still the default 1234 — change it."
                                           : "Technician code: set.");
  setCol(W.ab[5], Store::cfg().pin == 1234 ? C_FAULT : C_DIM);
}

// ═══════════════════════ SET CLOCK ═══════════════════════
static struct tm s_clk;

static void clkAdj(lv_event_t* e) {
  const intptr_t v = (intptr_t)lv_event_get_user_data(e);
  const int dir = (v & 1) ? 1 : -1;          // bit 0 = direction, upper bits = field
  switch (v >> 1) {
    case 0: s_clk.tm_hour = (s_clk.tm_hour + 24 + dir) % 24; break;
    case 1: s_clk.tm_min  = (s_clk.tm_min  + 60 + dir) % 60; break;
    case 2: s_clk.tm_mday = ((s_clk.tm_mday - 1 + 31 + dir) % 31) + 1; break;
    case 3: s_clk.tm_mon  = (s_clk.tm_mon  + 12 + dir) % 12; break;
    case 4: s_clk.tm_year = constrain(s_clk.tm_year + dir, 125, 199); break;
  }
  UI::show(SCR_CLOCK);
}

static void clkRow(lv_obj_t* p, int y, const char* label, const char* value, uint8_t field) {
  lv_obj_t* r = rowBase(p, y, label);
  mkLbl1(r, 150, 12, value, &ui14, C_TXT, 180, 19);
  mkBtn(r, LCD_W - 100, 6, 44, 30, "-", C_SURF2, clkAdj, (field << 1) | 0);
  mkBtn(r, LCD_W - 50, 6, 44, 30, "+", C_SURF2, clkAdj, (field << 1) | 1);
}

static void clkSave(lv_event_t*) {
  s_clk.tm_sec = 0;
  Clock::set(s_clk);
  Store::logAdd(EV_TIMESET, 0, 0);
  UI::show(SCR_ABOUT);
}

static void buildClock() {
  // Reload the time EVERY time the screen is entered from elsewhere. esp32Fire
  // kept the first value forever: the second visit showed the old time and
  // SAVE wound the clock back. While +/- rebuilds this screen, keep the edit.
  if (s_prev != SCR_CLOCK) Clock::now(s_clk);
  if (s_clk.tm_year < 125) { s_clk.tm_year = 126; s_clk.tm_mon = 0; s_clk.tm_mday = 1; }
  statusBar("SET CLOCK", Clock::valid() ? "clock is set" : "clock NOT set");
  lv_obj_t* body = mkBox(lv_scr_act(), 0, H_STATUSBAR, LCD_W, H_BODY, C_BG);
  char b[16];
  snprintf(b, sizeof(b), "%02d", s_clk.tm_hour);      clkRow(body, 0,   "Hour",   b, 0);
  snprintf(b, sizeof(b), "%02d", s_clk.tm_min);       clkRow(body, 42,  "Minute", b, 1);
  snprintf(b, sizeof(b), "%02d", s_clk.tm_mday);      clkRow(body, 84,  "Day",    b, 2);
  snprintf(b, sizeof(b), "%02d", s_clk.tm_mon + 1);   clkRow(body, 126, "Month",  b, 3);
  snprintf(b, sizeof(b), "%d", s_clk.tm_year + 1900); clkRow(body, 168, "Year",   b, 4);
  mkLbl1(body, 10, 214, "No battery-backed clock: power loss loses the time unless NTP is reachable.",
         &ui12, C_DIM, 460, 16);
  const FootBtn f[2] = {
    {"SAVE", C_OK,    C_WHITE, clkSave, 0},
    {"BACK", C_SURF2, C_TXT,   goTo,    SCR_ABOUT},
  };
  footer(f, 2);
}

// ═══════════════════════ SPLASH ═══════════════════════
// Doubles as the power-up self test: the installer sees all three LED colours
// and hears the speaker once.
void UI::splash(uint32_t ms) {
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), C_BG, 0);
  lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* badge = mkBox(lv_scr_act(), 196, 28, 88, 88, C_ISO);
  lv_obj_set_style_radius(badge, 18, 0);
  lv_obj_t* ic = mkLbl(badge, 0, 20, IC_ELEVATOR, &ico48, C_WHITE, 88);
  lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t* t = mkLbl(lv_scr_act(), 0, 132, "ELEVATOR", &ui42b, C_TXT, LCD_W);
  lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
  mkBox(lv_scr_act(), 160, 190, 160, 3, C_ARROW);
  lv_obj_t* s = mkLbl(lv_scr_act(), 0, 206, "CAN BUS DISPLAY · MQTT · HOME ASSISTANT", &ui14, C_DIM, LCD_W);
  lv_obj_set_style_text_align(s, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_t* v = mkLbl(lv_scr_act(), 0, 234, "v" FW_VERSION " · ESP32 · TWAI", &ui12, C_LINE, LCD_W);
  lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_CENTER, 0);

  Out::beep(1319, 120);
  const uint32_t t0 = millis();
  uint8_t phase = 255;
  while (millis() - t0 < ms) {
    const uint8_t p = (uint8_t)((millis() - t0) * 3 / ms);          // red → green → blue
    if (p != phase) { phase = p; if (Out::ledEnabled()) Out::ledSolid(p == 0, p == 1, p == 2); }
    lv_timer_handler();
    Out::loop();
    delay(5);
  }
  Out::ledSolid(false, false, false);
}

// ═══════════════════════ screen management ═══════════════════════
// NEVER rebuild a screen inside an LVGL event callback. Almost every screen
// change starts from a touch, and lv_obj_clean() deletes the very button that
// is still dispatching the event — LVGL returns into freed memory and the
// ESP32 resets (esp32Fire bug #1). show() only RECORDS the request; loop()
// builds the screen once outside the event dispatch.
void UI::show(Screen s) { s_pending = s; }

static uint32_t idleLimit(Screen s) {
  switch (s) {
    case SCR_SNIFF: case SCR_FRAME: case SCR_CAN: case SCR_MAP: case SCR_FLOORS: return 600000UL;
    case SCR_KEYBOARD: return 180000UL;
    default: return IDLE_HOME_MS;
  }
}

static void showNow(Screen s) {
  if (s_cur == SCR_WIFISCAN && s != SCR_WIFISCAN) Net::scanClear();   // give the scan list's heap back
  s_prev = s_cur;
  s_cur  = s;
  s_modal = nullptr;
  s_sbClock = false;
  memset(&W, 0, sizeof(W));
  lv_obj_clean(lv_scr_act());
  lv_obj_set_style_bg_color(lv_scr_act(), C_BG, 0);
  lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

  switch (s) {
    case SCR_MAIN:      buildMain();      break;
    case SCR_MENU:      buildMenu();      break;
    case SCR_CALL:      buildCall();      break;
    case SCR_PIN:       buildPin();       break;
    case SCR_CAN:       buildCan();       break;
    case SCR_SNIFF:     buildSniff();     break;
    case SCR_FRAME:     buildFrame();     break;
    case SCR_MAP:       buildMap();       break;
    case SCR_FLOORS:    buildFloors();    break;
    case SCR_FLOORLIST: buildFloorList(); break;
    case SCR_NET:       buildNet();       break;
    case SCR_WIFISCAN:  buildWifiScan();  break;
    case SCR_MQTT:      buildMqtt();      break;
    case SCR_DISPLAY:   buildDisplay();   break;
    case SCR_AUDIO:     buildAudio();     break;
    case SCR_LOG:       buildLog();       break;
    case SCR_ABOUT:     buildAbout();     break;
    case SCR_CLOCK:     buildClock();     break;
    case SCR_KEYBOARD:  buildKeyboard();  break;
    default:            buildMain();      break;
  }
  if (s == SCR_MAIN) refreshMain();
  if (s == SCR_ABOUT) refreshAbout();
  Display::wake();
}

Screen UI::current() { return s_cur; }

void UI::begin() {
  lv_timer_create(arrowTick, 40, nullptr);
  showNow(SCR_MAIN);
}

void UI::loop() {
  // ── Work deferred from event callbacks, done here where it is safe ──
  if (s_wantCal) {
    s_wantCal = false;
    Display::runTouchCalibration(60000);   // blocks AT MOST 60 s for all 4 points
    showNow(SCR_DISPLAY);
    return;
  }
  if (s_wantExport) {
    s_wantExport = 0;
    Store::exportCsv(Serial);
    return;
  }
  if (s_wantCloseModal) {
    s_wantCloseModal = false;
    // A pending rebuild cleans the dialog anyway; deleting it twice would touch freed memory.
    if (s_pending == SCR_COUNT && s_modal) { lv_obj_del(s_modal); s_modal = nullptr; }
  }
  if (s_pending != SCR_COUNT) {
    const Screen s = s_pending;
    s_pending = SCR_COUNT;
    showNow(s);
    return;
  }

  const uint32_t now = millis();

  static bool audioWas = false;
  if (Audio::busy() != audioWas) { audioWas = Audio::busy(); if (audioWas) Display::wake(); }

  // Fire service: keep the screen at full brightness and the indicator in front
  static bool fireWas = false;
  const bool fire = (Lift::mode() == LM_FIRE);
  if (fire != fireWas) {
    fireWas = fire;
    Display::lockSleep(fire);
    if (fire && s_cur != SCR_MAIN) show(SCR_MAIN);   // fire service always takes the screen
  }

  // Left on another screen untouched → back to the indicator. A lobby display
  // found sitting on the call panel is a broken display. Screens that are
  // watched without touching (raw frames, CAN counters, live protocol values)
  // get 10 minutes; the keyboard gets 3.
  if (s_cur != SCR_MAIN && Display::idleMs() > idleLimit(s_cur)) show(SCR_MAIN);

  static uint32_t last = 0;
  if (s_cur == SCR_MAIN) {
    if (now - last >= 100) { last = now; refreshMain(); }
    return;
  }
  if (now - last < 250) return;
  last = now;
  if (s_sbClock) setTxt(W.sbR, statusRight());

  switch (s_cur) {
    case SCR_CALL:    refreshCall();   break;
    case SCR_CAN:     refreshCan();    break;
    case SCR_SNIFF: {
      static uint32_t lastSn = 0;                 // a 48-row list is costly — 3 times a second is enough
      if (now - lastSn >= 330) { lastSn = now; refreshSniff(); }
      break;
    }
    case SCR_FRAME:   refreshFrame();  break;
    case SCR_MAP:     refreshMap();    break;
    case SCR_FLOORS:  refreshFloors(); break;
    case SCR_MQTT:    refreshMqtt();   break;
    case SCR_ABOUT: {
      static uint32_t lastAb = 0;
      if (now - lastAb >= 1000) { lastAb = now; refreshAbout(); }
      break;
    }
    // Rebuild only when the scan result arrives (the scan is asynchronous)
    case SCR_WIFISCAN: {
      static int wasN = -3;
      const int n = Net::scanState();
      if (n != wasN) { wasN = n; showNow(SCR_WIFISCAN); }
      break;
    }
    // Rebuild the NETWORK screen only when the connection state really changes —
    // rebuilding every 250 ms flickers and swallows half-finished touches.
    case SCR_NET: {
      const int8_t sig = (int8_t)(Net::on() ? (Net::staConnected() ? 1 : Net::apMode() ? 2 : 0) : 3);
      if (sig != W.netSig) showNow(SCR_NET);
      break;
    }
    default: break;
  }
}
