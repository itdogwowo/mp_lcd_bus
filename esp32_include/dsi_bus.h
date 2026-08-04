#include "soc/soc_caps.h"

#ifndef _ESP32_DSI_BUS_H_
#define _ESP32_DSI_BUS_H_

#include "py/obj.h"

// The type symbol is always provided (stub on chips without MIPI DSI),
// so modlcd_bus.c can reference it unconditionally.
extern const mp_obj_type_t mp_lcd_dsi_bus_type;

#if SOC_MIPI_DSI_SUPPORTED

#include "esp_lcd_mipi_dsi.h"

// 佇列槽上限（陣列大小），實際深度由建構子 queue_depth 決定（1..此值）
#define DSI_MAX_QUEUE_DEPTH    8
#define DSI_DEFAULT_QUEUE_DEPTH 4
#define DSI_MAX_FBS         3

typedef struct _mp_lcd_dsi_bus_obj_t {
    mp_obj_base_t base;

    esp_lcd_dsi_bus_handle_t  bus_handle;
    esp_lcd_panel_io_handle_t dbi_io;
    esp_lcd_panel_handle_t    dpi_panel;

    mp_obj_t ref_bufs[DSI_MAX_QUEUE_DEPTH];
    bool done_flags[DSI_MAX_QUEUE_DEPTH];
    int pending_segments[DSI_MAX_QUEUE_DEPTH];  // 每槽剩餘 DMA 段數 (流式 write 分段)
    int queue_head, queue_tail, queue_count;
    int queue_depth;

    // 視窗狀態 — set_window() 設定, write(buf) 流式寫入 (面板 RAMWR 模型)
    int win_x0, win_y0, win_x1, win_y1;
    int pos_x, pos_y;    // 流式寫入位置 (像素, 窗內)

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
