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
    int queue_head, queue_tail, queue_count, queue_depth;

    int lane_count;
    int data_pins[16];
    int hsync_pin, vsync_pin, de_pin, pclk_pin, disp_pin;
    int panel_w, panel_h, freq;

    bool initialized;
} mp_lcd_rgb_bus_obj_t;

extern const mp_obj_type_t mp_lcd_rgb_bus_type;

#endif
#endif
