#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED

#include "dsi_bus.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"

#include "py/runtime.h"
#include "py/objarray.h"
#include "py/mphal.h"
#include "py/gc.h"
#include "driver/gpio.h"

#include <string.h>

static mp_lcd_dsi_bus_obj_t *s_last_dsi = NULL;

static bool on_color_trans_done(esp_lcd_panel_handle_t panel,
                                esp_lcd_dpi_panel_event_data_t *edata, void *ctx) {
    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)ctx;
    for (int i = 0; i < self->queue_depth; i++) {
        if (self->ref_bufs[i] != mp_const_none && !self->done_flags[i]) {
            // 流式 write 會拆成多段 DMA2D 拷貝 — 每段完成遞減一次,
            // 最後一段完成才算該 write 完成 (queue 槽釋放)
            if (self->pending_segments[i] > 0) {
                self->pending_segments[i]--;
                if (self->pending_segments[i] == 0) {
                    self->done_flags[i] = true;
                    self->ref_bufs[i] = mp_const_none;
                    self->queue_head = (self->queue_head + 1) % self->queue_depth;
                    self->queue_count--;
                }
            }
            break;
        }
    }
    return false;
}

static void dsi_deinit_hardware(mp_lcd_dsi_bus_obj_t *self) {
    if (!self->initialized) return;
    if (self->dpi_panel) esp_lcd_panel_del(self->dpi_panel);
    if (self->dbi_io) esp_lcd_panel_io_del(self->dbi_io);
    if (self->bus_handle) esp_lcd_del_dsi_bus(self->bus_handle);
    if (self->reset_pin >= 0) {
        gpio_config_t gc = {
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = 1ULL << self->reset_pin,
        };
        gpio_config(&gc);
    }
    self->dpi_panel = NULL;
    self->dbi_io = NULL;
    self->bus_handle = NULL;
    self->initialized = false;
}

static mp_obj_t dsi_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum {
        ARG_lanes, ARG_width, ARG_height,
        ARG_lane_bit_rate_mbps, ARG_dpi_clk_mhz,
        ARG_hsync_pulse_width, ARG_hsync_back_porch, ARG_hsync_front_porch,
        ARG_vsync_pulse_width, ARG_vsync_back_porch, ARG_vsync_front_porch,
        ARG_in_color_format, ARG_fb_count, ARG_rst, ARG_virtual_channel,
        ARG_cmd_bits, ARG_param_bits,
        ARG_use_dma2d, ARG_queue_depth,
    };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_lanes,              MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_width,              MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_height,             MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_lane_bit_rate_mbps, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_dpi_clk_mhz,        MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_hsync_pulse_width,  MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
        { MP_QSTR_hsync_back_porch,   MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 10} },
        { MP_QSTR_hsync_front_porch,  MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 10} },
        { MP_QSTR_vsync_pulse_width,  MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
        { MP_QSTR_vsync_back_porch,   MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 10} },
        { MP_QSTR_vsync_front_porch,  MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 10} },
        { MP_QSTR_in_color_format,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 16} },
        { MP_QSTR_fb_count,           MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 2} },
        { MP_QSTR_rst,                MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
        { MP_QSTR_virtual_channel,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_cmd_bits,           MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 8} },
        { MP_QSTR_param_bits,         MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 8} },
        { MP_QSTR_use_dma2d,          MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
        { MP_QSTR_queue_depth,        MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = DSI_DEFAULT_QUEUE_DEPTH} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);

    int lanes = args[ARG_lanes].u_int;
    if (lanes < 1 || lanes > 4)
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("DSI data lanes must be 1-4"));

    int bpp = args[ARG_in_color_format].u_int;
    if (bpp != 16 && bpp != 24)
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("in_color_format must be 16 (RGB565) or 24 (RGB888)"));

    int nfbs = args[ARG_fb_count].u_int;
    if (nfbs < 1 || nfbs > DSI_MAX_FBS)
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("fb_count must be 1-%d"), DSI_MAX_FBS);

    int qdepth = args[ARG_queue_depth].u_int;
    if (qdepth < 1 || qdepth > DSI_MAX_QUEUE_DEPTH)
        mp_raise_msg_varg(&mp_type_ValueError, MP_ERROR_TEXT("queue_depth must be 1-%d"), DSI_MAX_QUEUE_DEPTH);

    // only one DSI controller on the chip, tear down a previous instance if any
    if (s_last_dsi) {
        dsi_deinit_hardware(s_last_dsi);
        s_last_dsi = NULL;
    }

    mp_lcd_dsi_bus_obj_t *self = m_new_obj(mp_lcd_dsi_bus_obj_t);
    memset(self, 0, sizeof(*self));
    self->base.type = &mp_lcd_dsi_bus_type;
    self->lane_count = lanes;
    self->panel_w = args[ARG_width].u_int;
    self->panel_h = args[ARG_height].u_int;
    self->reset_pin = args[ARG_rst].u_int;
    self->bits_per_pixel = bpp;
    self->num_fbs = nfbs;
    self->fb_size = (size_t)self->panel_w * self->panel_h * bpp / 8;
    self->queue_depth = qdepth;

    esp_err_t ret;

    esp_lcd_dsi_bus_config_t bc = {
        .bus_id = 0,
        .num_data_lanes = (uint8_t)lanes,
        .phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT,
        .lane_bit_rate_mbps = mp_obj_get_float(args[ARG_lane_bit_rate_mbps].u_obj),
    };
    ret = esp_lcd_new_dsi_bus(&bc, &self->bus_handle);
    if (ret != ESP_OK) goto err_bus;

    esp_lcd_dbi_io_config_t dbi = {
        .virtual_channel = (uint8_t)args[ARG_virtual_channel].u_int,
        .lcd_cmd_bits = (uint8_t)args[ARG_cmd_bits].u_int,
        .lcd_param_bits = (uint8_t)args[ARG_param_bits].u_int,
    };
    ret = esp_lcd_new_panel_io_dbi(self->bus_handle, &dbi, &self->dbi_io);
    if (ret != ESP_OK) goto err_bus;

    float dpi_clk_mhz = 30.0f;
    if (args[ARG_dpi_clk_mhz].u_obj != MP_OBJ_NULL)
        dpi_clk_mhz = mp_obj_get_float(args[ARG_dpi_clk_mhz].u_obj);

    esp_lcd_dpi_panel_config_t dc = {
        .virtual_channel = (uint8_t)args[ARG_virtual_channel].u_int,
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = dpi_clk_mhz,
        .pixel_format = (bpp == 24) ? LCD_COLOR_PIXEL_FORMAT_RGB888 : LCD_COLOR_PIXEL_FORMAT_RGB565,
        .in_color_format = (bpp == 24) ? LCD_COLOR_FMT_RGB888 : LCD_COLOR_FMT_RGB565,
        .num_fbs = (uint8_t)nfbs,
        .video_timing = {
            .h_size = (uint32_t)self->panel_w,
            .v_size = (uint32_t)self->panel_h,
            .hsync_pulse_width = (uint32_t)args[ARG_hsync_pulse_width].u_int,
            .hsync_back_porch  = (uint32_t)args[ARG_hsync_back_porch].u_int,
            .hsync_front_porch = (uint32_t)args[ARG_hsync_front_porch].u_int,
            .vsync_pulse_width = (uint32_t)args[ARG_vsync_pulse_width].u_int,
            .vsync_back_porch  = (uint32_t)args[ARG_vsync_back_porch].u_int,
            .vsync_front_porch = (uint32_t)args[ARG_vsync_front_porch].u_int,
        },
        .flags = {
            .use_dma2d = args[ARG_use_dma2d].u_bool,
        },
    };
    ret = esp_lcd_new_panel_dpi(self->bus_handle, &dc, &self->dpi_panel);
    if (ret != ESP_OK) goto err_io;

    // grab the internal frame buffers (allocated by the driver in PSRAM)
    // note: fb_num must match the number of output pointers passed
    void *fb0 = NULL, *fb1 = NULL, *fb2 = NULL;
    if (esp_lcd_dpi_panel_get_frame_buffer(self->dpi_panel, (uint32_t)nfbs, &fb0, &fb1, &fb2) != ESP_OK)
        goto err_panel;
    self->fbs[0] = fb0;
    self->fbs[1] = fb1;
    self->fbs[2] = fb2;

    esp_lcd_dpi_panel_event_callbacks_t cbs = { .on_color_trans_done = on_color_trans_done };
    ret = esp_lcd_dpi_panel_register_event_callbacks(self->dpi_panel, &cbs, self);
    if (ret != ESP_OK) goto err_panel;

    // optional hardware reset for the panel
    // (esp_lcd DPI panel has no reset_gpio support, so we manage it manually)
    if (self->reset_pin >= 0) {
        gpio_config_t gc = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << self->reset_pin,
        };
        gpio_config(&gc);
        gpio_set_level(self->reset_pin, 0);
        mp_hal_delay_ms(20);
        gpio_set_level(self->reset_pin, 1);
        mp_hal_delay_ms(50);
    }

    // Note: esp_lcd_panel_reset() is NOT called — DPI panel driver doesn't
    // register a .reset callback and would return ESP_ERR_NOT_SUPPORTED.
    // The hardware reset above (if rst>=0) is the only reset mechanism.
    ret = esp_lcd_panel_init(self->dpi_panel);
    if (ret != ESP_OK) goto err_panel;

    memset(self->ref_bufs, 0, sizeof(self->ref_bufs));
    memset(self->done_flags, 0, sizeof(self->done_flags));
    memset(self->pending_segments, 0, sizeof(self->pending_segments));
    self->queue_head = self->queue_tail = self->queue_count = 0;
    // 視窗預設全螢幕, 流式位置從 (0,0) 開始
    self->win_x0 = self->win_y0 = 0;
    self->win_x1 = self->panel_w - 1;
    self->win_y1 = self->panel_h - 1;
    self->pos_x = self->pos_y = 0;
    self->initialized = true;

    s_last_dsi = self;

    return MP_OBJ_FROM_PTR(self);

err_panel:
    // ⚠ 修復：reset GPIO 已被配置為 OUTPUT，錯誤路徑需釋放回 INPUT
    if (self->reset_pin >= 0) {
        gpio_config_t gc = {
            .mode = GPIO_MODE_INPUT,
            .pin_bit_mask = 1ULL << self->reset_pin,
        };
        gpio_config(&gc);
    }
    if (self->dpi_panel) esp_lcd_panel_del(self->dpi_panel);
err_io:
    if (self->dbi_io) esp_lcd_panel_io_del(self->dbi_io);
err_bus:
    // ⚠ 修復：esp_lcd_new_dsi_bus 失敗時 bus_handle 為 NULL，直接 del 會 assert/crash
    if (self->bus_handle) esp_lcd_del_dsi_bus(self->bus_handle);
    m_del_obj(mp_lcd_dsi_bus_obj_t, self);
    mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("DSI init failed err=0x%x"), ret);
}


static mp_obj_t dsi_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_buf, ARG_x, ARG_y, ARG_w, ARG_h };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_buf,  MP_ARG_OBJ | MP_ARG_REQUIRED },
        // 統一 API: x/y/w/h 是舊式顯式區域 (保留向後相容)。
        // 新用法: set_window() 設定視窗後 write(buf) 流式寫入。
        { MP_QSTR_x,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
        { MP_QSTR_y,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
        { MP_QSTR_w,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
        { MP_QSTR_h,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)args[ARG_self].u_obj;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_buf].u_obj, &bufinfo, MP_BUFFER_READ);

    // ⚠ deinit 後 dpi_panel 為 NULL，直接使用會 use-after-free
    if (self->dpi_panel == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("bus deinitialized"));
    }
    if (self->queue_count >= self->queue_depth)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("queue full"));

    int idx = self->queue_tail;
    self->ref_bufs[idx] = args[ARG_buf].u_obj;
    self->done_flags[idx] = false;

    bool explicit_region = (args[ARG_x].u_int != -1 || args[ARG_y].u_int != -1 ||
                            args[ARG_w].u_int != -1 || args[ARG_h].u_int != -1);

    int bpp = self->bits_per_pixel / 8;    // bytes per pixel
    size_t px_total = bufinfo.len / (size_t)bpp;   // 可寫像素數 (尾巴忽略)

    // 段表: 流式 write 可能跨行, 每段 = 一行內的矩形 DMA2D 拷貝。
    // 段數上限 = 視窗行數 (每段至少 1 像素/行) — 動態算, 用大一點的靜態陣列。
    #define DSI_MAX_SEGS 600
    int seg_x[DSI_MAX_SEGS], seg_y[DSI_MAX_SEGS], seg_w[DSI_MAX_SEGS], seg_h[DSI_MAX_SEGS];
    int nsegs = 0;

    if (explicit_region) {
        // ── 舊式顯式區域: 一次拷貝, 不影響視窗/位置狀態 ──
        int x = args[ARG_x].u_int;
        int y = args[ARG_y].u_int;
        int w = args[ARG_w].u_int;
        int h = args[ARG_h].u_int;
        if (x == -1) x = 0;
        if (y == -1) y = 0;
        if (w == -1 || w == 0) w = self->panel_w - x;
        if (h == -1 || h == 0) h = self->panel_h - y;
        // clip 到面板範圍
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (x + w > self->panel_w) w = self->panel_w - x;
        if (y + h > self->panel_h) h = self->panel_h - y;
        if (w > 0 && h > 0 && px_total > 0) {
            seg_x[0] = x; seg_y[0] = y;
            seg_w[0] = w; seg_h[0] = h;
            nsegs = 1;
        }
    } else {
        // ── 流式模式: 從視窗內 pos 開始寫, 逐段前進 (面板 RAMWR 模型) ──
        // 分段策略: 行首對齊且剩餘 ≥ 一整行 → 合併成矩形 (全螢幕 = 1 段,
        // 一次 DMA2D); 行中開始 → 寫到行尾 (1 行)。避免逐行分段拖垮吞吐。
        size_t px_left = px_total;
        int px = self->pos_x;
        int py = self->pos_y;
        int region_w = self->win_x1 - self->win_x0 + 1;
        int region_h = self->win_y1 - self->win_y0 + 1;
        while (px_left > 0) {
            if (py > self->win_y1) {
                // 寫滿視窗 → 自動 wrap 回視窗起點 (連續整幀/分批輸入天然成立)
                px = self->win_x0;
                py = self->win_y0;
            }
            int row_left = self->win_x1 - px + 1;  // 本行剩餘像素
            if (row_left <= 0) {                   // 行尾換行
                px = self->win_x0;
                py++;
                continue;
            }
            size_t n_px;
            int seg_h_ = 1;
            if (px == self->win_x0 && px_left >= (size_t)region_w) {
                // 行首 → 合併完整行成矩形 (不超過視窗底部)
                size_t n_rows = px_left / (size_t)region_w;
                int rows_avail = region_h - (py - self->win_y0);
                if ((size_t)rows_avail < n_rows) n_rows = rows_avail;
                if (n_rows == 0) n_rows = 1;       // 至少一行
                n_px = n_rows * (size_t)region_w;
                seg_h_ = (int)n_rows;
            } else {
                // 行中開始 (或剩餘不足一行) → 寫到行尾
                n_px = px_left;
                if (n_px > (size_t)row_left) n_px = row_left;
            }
            if (nsegs < DSI_MAX_SEGS) {
                seg_x[nsegs] = px;
                seg_y[nsegs] = py;
                seg_w[nsegs] = (int)n_px;
                seg_h[nsegs] = seg_h_;
                nsegs++;
            } else {
                break;   // 段表滿 (理論上不會: 每段至少 1 像素/行)
            }
            px += (int)n_px;
            px_left -= n_px;
            if (px > self->win_x1) {
                px = self->win_x0;
                py++;
            }
        }
        // 更新流式位置 (只算已排程的段)
        if (nsegs > 0) {
            self->pos_x = seg_x[nsegs - 1] + seg_w[nsegs - 1];
            self->pos_y = seg_y[nsegs - 1];
            if (self->pos_x > self->win_x1) {
                self->pos_x = self->win_x0;
                self->pos_y++;
            }
        }
    }

    if (nsegs == 0) {
        // 空寫入 (0 像素) — 直接完成
        self->ref_bufs[idx] = mp_const_none;
        self->done_flags[idx] = true;
        self->queue_tail = (self->queue_tail + 1) % self->queue_depth;
        self->queue_count++;
        return mp_obj_new_int(idx + 1);
    }

    self->pending_segments[idx] = nsegs;
    self->queue_tail = (self->queue_tail + 1) % self->queue_depth;
    self->queue_count++;

    // 逐段 DMA2D 拷貝。DSI driver 一次只允許一個 in-flight 拷貝
    // (draw_sem), 所以中間段要等上一段完成才發下一段;
    // 最後一段非同步 (回呼會清 queue 槽)。
    const uint8_t *src = (const uint8_t *)bufinfo.buf;
    size_t src_off = 0;
    for (int k = 0; k < nsegs; k++) {
        size_t seg_bytes = (size_t)seg_w[k] * seg_h[k] * (size_t)bpp;
        esp_err_t ret = esp_lcd_panel_draw_bitmap(
            self->dpi_panel, seg_x[k], seg_y[k],
            seg_x[k] + seg_w[k], seg_y[k] + seg_h[k],
            src + src_off);
        if (ret != ESP_OK) {
            // 失敗: 釋放 queue 槽 (清除剩餘段計數)
            self->pending_segments[idx] = 0;
            self->done_flags[idx] = true;
            self->ref_bufs[idx] = mp_const_none;
            self->queue_head = (self->queue_head + 1) % self->queue_depth;
            self->queue_count--;
            mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("draw_bitmap failed err=0x%x"), ret);
        }
        src_off += seg_bytes;
        if (k < nsegs - 1) {
            // 等本段完成 (回呼遞減 pending_segments) 才能發下一段
            int expect = nsegs - (k + 1);
            while (self->pending_segments[idx] > expect) {
                mp_hal_delay_ms(1);
            }
        }
    }
    return mp_obj_new_int(idx + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dsi_write_obj, 2, dsi_write);


static mp_obj_t dsi_set_window(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_x0, ARG_y0, ARG_x1, ARG_y1 };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_x0,   MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_y0,   MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_x1,   MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_y1,   MP_ARG_INT | MP_ARG_REQUIRED },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)args[ARG_self].u_obj;
    if (self->dpi_panel == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("bus deinitialized"));
    }
    int x0 = args[ARG_x0].u_int;
    int y0 = args[ARG_y0].u_int;
    int x1 = args[ARG_x1].u_int;
    int y1 = args[ARG_y1].u_int;
    // clip 到面板範圍
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= self->panel_w) x1 = self->panel_w - 1;
    if (y1 >= self->panel_h) y1 = self->panel_h - 1;
    if (x1 < x0 || y1 < y0)
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("invalid window"));

    // 視窗語義 = 面板 CASET/PASET: 設定後 write() 從視窗左上角流式寫入
    self->win_x0 = x0;
    self->win_y0 = y0;
    self->win_x1 = x1;
    self->win_y1 = y1;
    self->pos_x = x0;
    self->pos_y = y0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dsi_set_window_obj, 1, dsi_set_window);


static mp_obj_t dsi_cmd(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_cmd, ARG_param };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self,  MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_cmd,   MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_param, MP_ARG_OBJ | MP_ARG_KW_ONLY, {.u_obj = MP_OBJ_NULL} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)args[ARG_self].u_obj;

    // ⚠ deinit 後 dbi_io 為 NULL，直接使用會 use-after-free
    if (self->dbi_io == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("bus deinitialized"));
    }

    const void *param = NULL;
    size_t param_size = 0;
    if (args[ARG_param].u_obj != MP_OBJ_NULL) {
        mp_buffer_info_t pi;
        mp_get_buffer_raise(args[ARG_param].u_obj, &pi, MP_BUFFER_READ);
        param = pi.buf;
        param_size = pi.len;
    }

    esp_err_t ret = esp_lcd_panel_io_tx_param(self->dbi_io, args[ARG_cmd].u_int, param, param_size);
    if (ret != ESP_OK)
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("tx_param failed err=0x%x"), ret);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dsi_cmd_obj, 2, dsi_cmd);


static mp_obj_t dsi_set_pattern(mp_obj_t self_in, mp_obj_t pat_in) {
    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)self_in;
    // ⚠ deinit 後 dpi_panel 為 NULL，直接使用會 use-after-free
    if (self->dpi_panel == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("bus deinitialized"));
    }
    int pat = mp_obj_get_int(pat_in);
    if (pat < 0 || pat > 3)
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("pattern must be 0-3"));
    esp_err_t ret = esp_lcd_dpi_panel_set_pattern(self->dpi_panel, (mipi_dsi_pattern_type_t)pat);
    if (ret != ESP_OK)
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("set_pattern failed err=0x%x"), ret);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(dsi_set_pattern_obj, dsi_set_pattern);


static mp_obj_t dsi_frame_buffer(mp_obj_t self_in, mp_obj_t idx_in) {
    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)self_in;
    int i = mp_obj_get_int(idx_in);
    if (i < 0 || i >= self->num_fbs)
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("frame buffer index out of range"));
    return mp_obj_new_bytearray_by_ref(self->fb_size, self->fbs[i]);
}
static MP_DEFINE_CONST_FUN_OBJ_2(dsi_frame_buffer_obj, dsi_frame_buffer);


static mp_obj_t dsi_flush(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_idx, ARG_x, ARG_y, ARG_w, ARG_h };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_idx,  MP_ARG_INT, {.u_int = 0} },
        { MP_QSTR_x,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_y,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_w,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_h,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)args[ARG_self].u_obj;
    // ⚠ deinit 後 dpi_panel 為 NULL，直接使用會 use-after-free
    if (self->dpi_panel == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("bus deinitialized"));
    }
    int idx = args[ARG_idx].u_int;
    if (idx < 0 || idx >= self->num_fbs || self->fbs[idx] == NULL)
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("frame buffer index out of range"));

    int x = args[ARG_x].u_int;
    int y = args[ARG_y].u_int;
    int w = args[ARG_w].u_int ? args[ARG_w].u_int : self->panel_w;
    int h = args[ARG_h].u_int ? args[ARG_h].u_int : self->panel_h;

    // clip 到面板範圍（與 esp_lcd_dpi_panel_draw_bitmap 同慣例）
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > self->panel_w) w = self->panel_w - x;
    if (y + h > self->panel_h) h = self->panel_h - y;
    if (w <= 0 || h <= 0) return mp_const_none;

    // ⚠ ESP32-P4：frame buffer 在 PSRAM，CPU 寫入走 write-back L2 cache，
    // 而 DPI DMA 是直接讀 PSRAM（不經 cache）。frame_buffer() 零拷貝直寫
    // 之後若不 esp_cache_msync() 把髒行寫回，DMA 會一直讀到舊內容 —
    // 症狀就是「殘留/黑帶/閃爍」。flush() 就是把指定區域的髒 cache line
    // 寫回 PSRAM（DIR_C2M），與 esp_lcd DPI driver 內部做法相同。
    uint8_t *start = (uint8_t *)self->fbs[idx] + (y * self->panel_w + x) * self->bits_per_pixel / 8;
    size_t size = (size_t)h * self->panel_w * self->bits_per_pixel / 8;
    esp_cache_msync(start, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dsi_flush_obj, 1, dsi_flush);


static mp_obj_t dsi_is_busy(mp_obj_t self_in) {
    return mp_obj_new_bool(((mp_lcd_dsi_bus_obj_t *)self_in)->queue_count > 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(dsi_is_busy_obj, dsi_is_busy);

static mp_obj_t dsi_pending(mp_obj_t self_in) {
    return mp_obj_new_int(((mp_lcd_dsi_bus_obj_t *)self_in)->queue_count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(dsi_pending_obj, dsi_pending);

static mp_obj_t dsi_lane_count(mp_obj_t self_in) {
    return mp_obj_new_int(((mp_lcd_dsi_bus_obj_t *)self_in)->lane_count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(dsi_lane_count_obj, dsi_lane_count);


static mp_obj_t dsi_wait(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_trans_id, ARG_timeout_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self,       MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_trans_id,   MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_timeout_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)args[ARG_self].u_obj;
    int tid = args[ARG_trans_id].u_int;
    int to  = args[ARG_timeout_ms].u_int;
    int idx = tid - 1;

    if (idx < 0 || idx >= self->queue_depth) return mp_const_false;
    if (self->done_flags[idx]) return mp_const_true;

    mp_uint_t deadline = mp_hal_ticks_ms() + (to < 0 ? 10000 : to);
    while (!self->done_flags[idx]) {
        if (to >= 0 && mp_hal_ticks_ms() > deadline) return mp_const_false;
        mp_hal_delay_ms(1);
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dsi_wait_obj, 2, dsi_wait);


static mp_obj_t dsi_wait_all(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_timeout_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self,       MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_timeout_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)args[ARG_self].u_obj;
    int to = args[ARG_timeout_ms].u_int;
    mp_uint_t deadline = mp_hal_ticks_ms() + (to < 0 ? 10000 : to);

    while (self->queue_count > 0) {
        if (to >= 0 && mp_hal_ticks_ms() > deadline) break;
        mp_hal_delay_ms(1);
    }
    // ⚠ 效能修復（同 spi_bus/rgb_bus）：正常 drain 完畢不需立即 GC —
    // 每幀 flush 都會呼叫 wait_all，full GC 在 PSRAM heap + 大 frame buffer
    // 上可達 ~200ms。僅在逾時殘留（queue 未清空）時才強制釋放 + gc_collect。
    if (self->queue_count > 0) {
        for (int i = 0; i < self->queue_depth; i++) {
            self->ref_bufs[i] = mp_const_none;
            self->done_flags[i] = true;
            self->pending_segments[i] = 0;
        }
        self->queue_head = self->queue_tail = self->queue_count = 0;
        gc_collect();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dsi_wait_all_obj, 1, dsi_wait_all);


static mp_obj_t dsi_deinit(mp_obj_t self_in) {
    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)self_in;
    if (s_last_dsi == self) s_last_dsi = NULL;
    dsi_deinit_hardware(self);
    // ⚠ 修復：清 ref_bufs（原先殘留釘住 buffer 引用 → GC 洩漏）
    for (int i = 0; i < self->queue_depth; i++) {
        self->ref_bufs[i] = mp_const_none;
        self->done_flags[i] = true;
        self->pending_segments[i] = 0;
    }
    self->queue_count = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(dsi_deinit_obj, dsi_deinit);


static const mp_rom_map_elem_t dsi_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write),        MP_ROM_PTR(&dsi_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_window),   MP_ROM_PTR(&dsi_set_window_obj) },
    { MP_ROM_QSTR(MP_QSTR_cmd),          MP_ROM_PTR(&dsi_cmd_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_busy),      MP_ROM_PTR(&dsi_is_busy_obj) },
    { MP_ROM_QSTR(MP_QSTR_pending),      MP_ROM_PTR(&dsi_pending_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait),         MP_ROM_PTR(&dsi_wait_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_all),     MP_ROM_PTR(&dsi_wait_all_obj) },
    { MP_ROM_QSTR(MP_QSTR_lane_count),   MP_ROM_PTR(&dsi_lane_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_frame_buffer), MP_ROM_PTR(&dsi_frame_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush),        MP_ROM_PTR(&dsi_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_pattern),  MP_ROM_PTR(&dsi_set_pattern_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),       MP_ROM_PTR(&dsi_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),      MP_ROM_PTR(&dsi_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(dsi_locals_dict, dsi_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    mp_lcd_dsi_bus_type,
    MP_QSTR_DSIBus,
    MP_TYPE_FLAG_NONE,
    make_new, dsi_make_new,
    locals_dict, (mp_obj_dict_t *)&dsi_locals_dict
);

#else // !SOC_MIPI_DSI_SUPPORTED

#include "py/obj.h"
#include "py/runtime.h"

// Stub for chips without MIPI DSI (e.g. ESP32-S3): the type exists so the
// module links, but constructing it raises NotImplementedError.
static mp_obj_t dsi_make_new(const mp_obj_type_t *t, size_t n_args, size_t n_kw, const mp_obj_t *all) {
    mp_raise_msg(&mp_type_NotImplementedError, MP_ERROR_TEXT("LCD DSI bus is not supported on this MCU"));
}

MP_DEFINE_CONST_OBJ_TYPE(
    mp_lcd_dsi_bus_type,
    MP_QSTR_DSIBus,
    MP_TYPE_FLAG_NONE,
    make_new, dsi_make_new
);

#endif // SOC_MIPI_DSI_SUPPORTED
