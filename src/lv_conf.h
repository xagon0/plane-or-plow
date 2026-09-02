#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* Color settings */
#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP   0

/* Memory settings */
#define LV_MEM_CUSTOM      1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC   malloc
#define LV_MEM_CUSTOM_FREE    free
#define LV_MEM_CUSTOM_REALLOC realloc

/* Display settings */
#define LV_HOR_RES_MAX     480
#define LV_VER_RES_MAX     480
#define LV_DPI_DEF          130

/* Tick */
#define LV_TICK_CUSTOM      1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/* Logging */
#define LV_USE_LOG           0

/* GPU - none */
#define LV_USE_GPU_ESP32_DMA2D 0

/* Fonts */
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_24  1
#define LV_FONT_MONTSERRAT_28  1
#define LV_FONT_MONTSERRAT_32  1
#define LV_FONT_MONTSERRAT_36  1
#define LV_FONT_MONTSERRAT_48  1
#define LV_FONT_DEFAULT         &lv_font_montserrat_16

/* Widgets */
#define LV_USE_ARC           1
#define LV_USE_BAR           1
#define LV_USE_BTN           1
#define LV_USE_BTNMATRIX     1
#define LV_USE_CANVAS        1
#define LV_USE_CHECKBOX      1
#define LV_USE_DROPDOWN      1
#define LV_USE_IMG           1
#define LV_USE_LABEL         1
#define LV_USE_LINE          1
#define LV_USE_ROLLER        1
#define LV_USE_SLIDER        1
#define LV_USE_SWITCH        1
#define LV_USE_TEXTAREA      1
#define LV_USE_TABLE         1

/* Extra widgets */
#define LV_USE_ANIMIMG       1
#define LV_USE_CALENDAR      0
#define LV_USE_CHART         0
#define LV_USE_COLORWHEEL    0
#define LV_USE_IMGBTN        1
#define LV_USE_KEYBOARD      0
#define LV_USE_LED           1
#define LV_USE_LIST          1
#define LV_USE_MENU          0
#define LV_USE_METER         0
#define LV_USE_MSGBOX        1
#define LV_USE_SPAN          1
#define LV_USE_SPINBOX       0
#define LV_USE_SPINNER       1
#define LV_USE_TABVIEW       0
#define LV_USE_TILEVIEW      0
#define LV_USE_WIN           0

/* Themes */
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_USE_THEME_BASIC   1

/* Layouts */
#define LV_USE_FLEX          1
#define LV_USE_GRID          1

#endif /* LV_CONF_H */
