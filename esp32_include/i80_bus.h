#include "soc/soc_caps.h"

#if SOC_LCD_I80_SUPPORTED

#ifndef _ESP32_I80_BUS_H_
#define _ESP32_I80_BUS_H_

#include "esp_lcd_panel_io.h"
#include "py/obj.h"

#define I80_DMA_QUEUE_DEPTH 4

typedef struct _mp_lcd_i80_bus_obj_t {
    mp_obj_base_t base;

    esp_lcd_i80_bus_handle_t bus_handle;
    esp_lcd_panel_io_handle_t panel_io;

    mp_obj_t ref_bufs[I80_DMA_QUEUE_DEPTH];
    bool done_flags[I80_DMA_QUEUE_DEPTH];
    int queue_head, queue_tail, queue_count;

    int lane_count;
    int data_pins[16];
    int wr_pin, dc_pin, cs_pin, freq;

    bool initialized;
} mp_lcd_i80_bus_obj_t;

extern const mp_obj_type_t mp_lcd_i80_bus_type;

#endif
#endif
