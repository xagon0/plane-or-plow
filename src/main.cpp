#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <esp_system.h>

#include "config.h"
#include "network.h"
#include "ambient.h"
#include "backlight.h"
#include "status.h"
#include "timekeep.h"
#include "diagnostics.h"
#include "touch_input.h"
#include "feed_http.h"
#include "weather.h"

// Rendering and bounded JSON/PNG completion callbacks run here. TLS and
// socket waits use their own 16 KiB worker stack, never the LVGL task.
SET_LOOP_TASK_STACK_SIZE(16384);

// --- Pin Definitions ---
#define TFT_CS    39
#define TFT_SCK   48
#define TFT_SDA   47
#define TFT_DE    18
#define TFT_VSYNC 17
#define TFT_HSYNC 16
#define TFT_PCLK  21
#define TOUCH_SDA 19
#define TOUCH_SCL 45

// --- Display dimensions ---
#define SCREEN_W 480
#define SCREEN_H 480

// --- Display Setup ---
Arduino_DataBus *bus = new Arduino_SWSPI(
    GFX_NOT_DEFINED, TFT_CS, TFT_SCK, TFT_SDA, GFX_NOT_DEFINED
);

Arduino_ESP32RGBPanel *rgbpanel = new Arduino_ESP32RGBPanel(
    TFT_DE, TFT_VSYNC, TFT_HSYNC, TFT_PCLK,
    11, 12, 13, 14, 0,
    8, 20, 3, 46, 9, 10,
    4, 5, 6, 7, 15,
    1, 10, 8, 50,
    1, 10, 8, 20
);

Arduino_RGB_Display *gfx = new Arduino_RGB_Display(
    SCREEN_W, SCREEN_H, rgbpanel, 0, true,
    bus, GFX_NOT_DEFINED,
    st7701_type9_init_operations, sizeof(st7701_type9_init_operations)
);

// --- LVGL buffers ---
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1;
static lv_color_t *buf2;

// --- LVGL display flush callback ---
void lvgl_flush_cb(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
    gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
    gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif

    lv_disp_flush_ready(disp);
}

// --- LVGL touch read callback ---
void lvgl_touchpad_read(lv_indev_drv_t *indev, lv_indev_data_t *data) {
    TouchPoint point;
    if (touch_input_read(point)) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = point.x;
        data->point.y = point.y;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("ESP32-4848S040 LVGL Setup");

    status_boot_report();
    if (!feed_http_begin()) {
        Serial.println("Feed worker startup failed; watchdog will restart");
        while (true) delay(100);
    }

    // Initialize display
    diagnostics_phase("setup-display");
    if (!gfx->begin()) {
        Serial.println("Display init FAILED!");
        while (1) delay(100);
    }
    gfx->fillScreen(BLACK);
    pinMode(BL_PIN, OUTPUT);
    digitalWrite(BL_PIN, HIGH);   // keep screen on during WiFi connect
    Serial.println("Display OK");

    // Initialize touch
    diagnostics_phase("setup-touch");
    touch_input_init(TOUCH_SDA, TOUCH_SCL, SCREEN_W, SCREEN_H);

    // Initialize LVGL
    diagnostics_phase("setup-lvgl");
    lv_init();

    // Allocate draw buffers in PSRAM (double-buffered, 1/10th screen each)
    size_t buf_size = SCREEN_W * (SCREEN_H / 10);
    buf1 = (lv_color_t *)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    buf2 = (lv_color_t *)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) {
        diagnostics_phase("setup-draw-buffer-failed");
        Serial.println("LVGL draw buffer allocation FAILED; watchdog will restart");
        while (true) delay(100);
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, buf_size);

    // Register display driver
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = SCREEN_W;
    disp_drv.ver_res = SCREEN_H;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // Register touch input driver
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    // Dark theme — the scope draws its own colours, this just keeps LVGL's
    // defaults from putting light chrome behind them.
    lv_theme_t *th = lv_theme_default_init(
        lv_disp_get_default(),
        lv_color_hex(0x2E88E0),
        lv_color_hex(0xE48A1C),
        true,
        LV_FONT_DEFAULT
    );
    lv_disp_set_theme(lv_disp_get_default(), th);
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x000000), 0);

    // --- Scope first, then the radio ---
    // ambient_init() composites the static background, so painting it before
    // network_init() (which blocks for up to 15s on the WiFi handshake) means
    // the scope is already on screen and fading up while we connect.
    diagnostics_phase("setup-ambient");
    ambient_init();
    backlight_init();

    for (int i = 0; i < 140; i++) {   // ~700ms: first flush + backlight fade-in
        lv_timer_handler();
        backlight_update();
        delay(5);
    }

    diagnostics_checkpoint();
    diagnostics_phase("setup-network");
    network_init();
    diagnostics_checkpoint();
    diagnostics_phase("setup-services");
    time_init();          // after WiFi: SNTP started earlier just fails
    status_init();

    ambient_reset_frame_timing();
    status_mark_settled();
    Serial.println("Plane or Plow — scope ready");
    diagnostics_phase("loop-ready");
    diagnostics_checkpoint();
}

void loop() {
    diagnostics_phase("lvgl");
    lv_timer_handler();
    diagnostics_checkpoint();
    diagnostics_phase("feed-completion");
    feed_http_tick();
    weather_tick();
    diagnostics_phase("network-expiry");
    network_tick();
    diagnostics_phase("backlight-clock");
    backlight_update();
    time_tick();
    diagnostics_phase("status-server");
    status_update();
    diagnostics_phase("loop-idle");
    diagnostics_update();
    diagnostics_checkpoint();
    delay(5);
}
