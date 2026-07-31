#include "soc/soc_caps.h"

#ifndef _ESP32_DSI_BUS_H_
#define _ESP32_DSI_BUS_H_

#include "py/obj.h"

// The type symbol is always provided (stub on chips without MIPI DSI),
// so modlcd_bus.c can reference it unconditionally.
extern const mp_obj_type_t mp_lcd_dsi_bus_type;

#if SOC_MIPI_DSI_SUPPORTED

#include "esp_lcd_mipi_dsi.h"

#define DSI_DMA_QUEUE_DEPTH 4
#define DSI_MAX_FBS         3

typedef struct _mp_lcd_dsi_bus_obj_t {
    mp_obj_base_t base;

    esp_lcd_dsi_bus_handle_t  bus_handle;
    esp_lcd_panel_io_handle_t dbi_io;
    esp_lcd_panel_handle_t    dpi_panel;

    mp_obj_t ref_bufs[DSI_DMA_QUEUE_DEPTH];
    bool done_flags[DSI_DMA_QUEUE_DEPTH];
    int queue_head, queue_tail, queue_count;

    int lane_count;
    int panel_w, panel_h;
    int reset_pin;
    int bits_per_pixel;
    int num_fbs;
    size_t fb_size;
    void *fbs[DSI_MAX_FBS];

    bool initialized;
} mp_lcd_dsi_bus_obj_t;

#endif // SOC_MIPI_DSI_SUPPORTED

#endif // _ESP32_DSI_BUS_H_
