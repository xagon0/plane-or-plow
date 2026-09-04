#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <Wire.h>
#include <TAMC_GT911.h>
#include <lvgl.h>
#include <esp_system.h>

#include "config.h"
#include "network.h"
#include "ambient.h"
#include "backlight.h"
#include "status.h"
#include "timekeep.h"

// Every API fetch runs an mbedTLS handshake on the Arduino loop task, whose
// stack defaults to 8 KB. Measured peak was ~6.3 KB of that, and a reconnect
// takes the deepest path because it cannot resume a TLS session — too little
// margin for three separate HTTPS clients. This macro overrides the weak
// getArduinoLoopTaskStackSize() in the framework; a -D build flag does not
// work, because the framework archive is cached and not rebuilt.
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

// --- Touch Setup ---
TAMC_GT911 touch(TOUCH_SDA, TOUCH_SCL, -1, -1, SCREEN_W, SCREEN_H);

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
    touch.read();
    if (touch.isTouched) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touch.points[0].x;
        data->point.y = touch.points[0].y;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("ESP32-4848S040 LVGL Setup");

    status_boot_report();

    // Initialize display
    if (!gfx->begin()) {
        Serial.println("Display init FAILED!");
        while (1) delay(100);
    }
    gfx->fillScreen(BLACK);
    pinMode(BL_PIN, OUTPUT);
    digitalWrite(BL_PIN, HIGH);   // keep screen on during WiFi connect
    Serial.println("Display OK");

    // Initialize touch
    Wire.begin(TOUCH_SDA, TOUCH_SCL);
    touch.begin();
    touch.setRotation(ROTATION_INVERTED);
    Serial.println("Touch OK");

    // Initialize LVGL
    lv_init();

    // Allocate draw buffers in PSRAM (double-buffered, 1/10th screen each)
    size_t buf_size = SCREEN_W * (SCREEN_H / 10);
    buf1 = (lv_color_t *)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    buf2 = (lv_color_t *)heap_caps_malloc(buf_size * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
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
    ambient_init();
    backlight_init();

    for (int i = 0; i < 140; i++) {   // ~700ms: first flush + backlight fade-in
        lv_timer_handler();
        backlight_update();
        delay(5);
    }

    network_init();
    time_init();          // after WiFi: SNTP started earlier just fails
    status_init();

    status_mark_settled();
    Serial.println("Plane or Plow — scope ready");
}

void loop() {
    lv_timer_handler();
    backlight_update();
    time_tick();
    status_update();
    delay(5);
}
