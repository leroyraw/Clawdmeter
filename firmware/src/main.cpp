#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <esp_heap_caps.h>

#include "data.h"
#include "ui.h"
#include "net.h"
#include "claude_poll.h"
#include "wifi_creds.h"
#include "splash.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/touch_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"
#include "hal/sound_hal.h"

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Touch policy is driven by IDLE_WAKE_ON_TOUCH:
//   true  → a press edge while asleep wakes the device and the first touch is
//           swallowed (mirrors the button wake-consumption); a press while
//           awake counts as activity.
//   false → touch never counts as activity and is fully swallowed while the
//           panel is dark, so pets/sleeves can't wake it overnight and LVGL
//           can't quietly toggle splash<->usage on a black panel.
static void my_touch_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    uint16_t x, y;
    bool pressed;
    touch_hal_read(&x, &y, &pressed);
    const bool raw_pressed = pressed;

    if (IDLE_WAKE_ON_TOUCH) {
        static bool touch_was = false;
        static bool touch_wake_swallowed = false;
        if (raw_pressed && !touch_was) {
            // Press edge — consume as wake if asleep.
            if (idle_consume_wake_press()) {
                touch_wake_swallowed = true;
                pressed = false;
            }
        } else if (!raw_pressed && touch_was) {
            // Release edge.
            if (touch_wake_swallowed) {
                touch_wake_swallowed = false;
                pressed = false;
            }
        } else if (raw_pressed && touch_wake_swallowed) {
            // Held finger through wake — keep hiding until release.
            pressed = false;
        }
        touch_was = raw_pressed;
    } else if (idle_is_asleep()) {
        pressed = false;
    }

    if (pressed) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            else if (strcmp(cmd_buf, "buzz") == 0)  sound_hal_play_reset();
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

// The 60s usage poll runs a TLS handshake + JSON inside loop(); the default
// 8 KB loop-task stack leaves little headroom for mbedTLS.
SET_LOOP_TASK_STACK_SIZE(16 * 1024);

void setup() {
    Serial.begin(115200);
    Serial.printf("Boot: reset reason %d\n", (int)esp_reset_reason());
    delay(300);
    Serial.println("{\"ready\":true}");

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle

    power_hal_init();
    imu_hal_init();
    sound_hal_init();
    touch_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    lv_indev_t* indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, my_touch_cb);

    // ui_init() must run before net_init(): net_init() paints the WiFi
    // setup/connecting hint onto the panel (via ui_update_net_status +
    // lv_timer_handler pumping — see pump_ui() in net.cpp) while blocked
    // on the captive portal or the WiFi-connect wait, and it needs the
    // LVGL widgets built to do that instead of leaving the panel black.
    ui_init();

    // Blocks until connected to WiFi (or falls into the captive-portal
    // provisioning loop, which never returns until creds are saved + the
    // device reboots). net_init()/provisioning_run() pump lv_timer_handler()
    // during that block so the setup/connecting screen stays live.
    net_init();
    input_hal_init();

    ui_update_net_status(net_state(), net_ip());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    ui_show_screen(SCREEN_SPLASH);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data over WiFi...\n",
        board_caps().name, W, H);
}

static NetState last_net = (NetState)-1;

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    net_tick();
    power_hal_tick();
    imu_hal_tick();
    sound_hal_tick();
    splash_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    //   PRIMARY   → wake only; held ~3s → wipe WiFi creds and re-provision
    //   SECONDARY → (unused; was HID mode-toggle, dropped with BLE)
    //   PWR       → on splash: cycle animations; on usage: cycle brightness
    // First press from sleep is consumed as a wake-only event by
    // idle_consume_wake_press(); activity bookkeeping happens inside it so
    // no separate idle_note_activity() call is needed here.
    {
        static bool primary_was = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now != primary_was) {
            if (primary_now) idle_consume_wake_press();
            primary_was = primary_now;
        }

        // BOOT held ~3s → wipe WiFi creds and reboot into the captive portal.
        static uint32_t primary_down_ms = 0;
        if (primary_now && primary_down_ms == 0) primary_down_ms = millis();
        if (!primary_now) primary_down_ms = 0;
        if (primary_down_ms && millis() - primary_down_ms > 3000) {
            Serial.println("BOOT long-press: clearing WiFi creds, re-provisioning");
            wifi_creds_clear();
            ESP.restart();
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                // On splash: cycle animations. On the usage view: cycle
                // screen brightness (single non-splash view, no more screens).
                if (ui_get_current_screen() == SCREEN_SPLASH) splash_next();
                else                                          brightness_cycle();
            }
        }
    }

    NetState ns = net_state();
    if (ns != last_net) {
        last_net = ns;
        ui_update_net_status(ns, net_ip());
    }

    // On-device usage poll (replaces the host daemon's /usage POST once
    // logged in): once every 60s while online + logged in, plus immediately
    // on first entering that state. claude_poll() blocks ~1-3s on HTTPS —
    // an acceptable once-a-minute stall for a single-purpose display.
    // A failed poll retries after 10s instead of a full minute, so a
    // transient error right after boot doesn't leave the screen empty.
    {
        static uint32_t last_poll = 0;
        static uint32_t poll_interval = 60000;
        static bool     polled_since_ok = false;
        if (ns == NET_ONLINE_OK) {
            if (!polled_since_ok || millis() - last_poll >= poll_interval) {
                last_poll = millis();
                polled_since_ok = true;
                UsageData u{};
                bool ok = claude_poll(u);
                if (ok) apply_usage(u);
                else    Serial.println("usage poll failed, retrying in 10s");
                poll_interval = ok ? 60000 : 10000;
            }
        } else {
            polled_since_ok = false;
        }
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();

    // Usage updates now arrive over the HTTP /usage POST handler in net.cpp,
    // which parses the JSON and calls ui_update() directly (also drives the
    // session-reset chime + splash rate-matching that used to live here).

    delay(5);
}
