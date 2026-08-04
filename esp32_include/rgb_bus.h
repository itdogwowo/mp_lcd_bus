#include "soc/soc_caps.h"

#if SOC_LCD_RGB_SUPPORTED

#ifndef _ESP32_RGB_BUS_H_
#define _ESP32_RGB_BUS_H_

#include "esp_lcd_panel_rgb.h"
#include "py/obj.h"

#define RGB_MAX_QUEUE_DEPTH    8
#define RGB_DEFAULT_QUEUE_DEPTH 2

typedef struct _mp_lcd_rgb_bus_obj_t {
    mp_obj_base_t base;

    esp_lcd_panel_handle_t panel_handle;

    mp_obj_t ref_bufs[RGB_MAX_QUEUE_DEPTH];
    bool done_flags[RGB_MAX_QUEUE_DEPTH];
    int pending_segments[RGB_MAX_QUEUE_DEPTH];  // 每槽剩餘段數 (流式 write 分段)
    int queue_head, queue_tail, queue_count, queue_depth;

    // 視窗狀態 — set_window() 設定, write(buf) 流式寫入 (面板 RAMWR 模型)
    int win_x0, win_y0, win_x1, win_y1;
    int pos_x, pos_y;    // 流式寫入位置 (像素, 窗內)

    int lane_count;
    int data_pins[16];
    int hsync_pin, vsync_pin, de_pin, pclk_pin, disp_pin;
    int panel_w, panel_h, freq;

    bool initialized;
} mp_lcd_rgb_bus_obj_t;

extern const mp_obj_type_t mp_lcd_rgb_bus_type;

#endif
#endif
