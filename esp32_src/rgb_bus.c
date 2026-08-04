#include "soc/soc_caps.h"

#if SOC_LCD_RGB_SUPPORTED

#include "rgb_bus.h"
#include "esp_lcd_panel_ops.h"
#include "esp_heap_caps.h"

#include "py/runtime.h"
#include "py/objarray.h"
#include "py/mphal.h"
#include "py/gc.h"
#include "soc/gpio_sig_map.h"
#include "esp_rom_gpio.h"
#include "driver/gpio.h"

#include <string.h>

static esp_lcd_panel_handle_t s_last_rgb_panel = NULL;

static bool on_vsync(esp_lcd_panel_handle_t panel,
                     const esp_lcd_rgb_panel_event_data_t *edata, void *ctx) {
    mp_lcd_rgb_bus_obj_t *self = (mp_lcd_rgb_bus_obj_t *)ctx;
    for (int i = 0; i < self->queue_depth; i++) {
        if (self->ref_bufs[i] != mp_const_none && !self->done_flags[i]) {
            // 流式 write 拆多段 — 每段完成 (vsync) 遞減一次,
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


static mp_obj_t rgb_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum {
        ARG_data, ARG_hsync, ARG_vsync, ARG_de, ARG_pclk,
        ARG_width, ARG_height, ARG_freq, ARG_disp,
        ARG_hsync_front_porch, ARG_hsync_back_porch, ARG_hsync_pulse_width,
        ARG_vsync_front_porch, ARG_vsync_back_porch, ARG_vsync_pulse_width,
        ARG_hsync_idle_low, ARG_vsync_idle_low,
        ARG_de_idle_high, ARG_pclk_idle_high, ARG_pclk_active_neg,
        ARG_disp_active_low, ARG_refresh_on_demand, ARG_bb_size_px,
        ARG_queue_depth,
    };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_data,              MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_hsync,             MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_vsync,             MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_de,                MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_pclk,              MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_width,             MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_height,            MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_freq,              MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 8000000} },
        { MP_QSTR_disp,              MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
        { MP_QSTR_hsync_front_porch, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_hsync_back_porch,  MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_hsync_pulse_width, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
        { MP_QSTR_vsync_front_porch, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_vsync_back_porch,  MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_vsync_pulse_width, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
        { MP_QSTR_hsync_idle_low,    MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_vsync_idle_low,    MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_de_idle_high,      MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_pclk_idle_high,    MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_pclk_active_neg,   MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_disp_active_low,   MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_refresh_on_demand, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
        { MP_QSTR_bb_size_px,        MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_queue_depth,       MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = RGB_DEFAULT_QUEUE_DEPTH} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);

    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(args[ARG_data].u_obj, &n, &items);
    if (n != 8 && n != 16)
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("RGB data pins must be 8 or 16"));

    int qdepth = args[ARG_queue_depth].u_int;
    if (qdepth < 1 || qdepth > RGB_MAX_QUEUE_DEPTH)
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("queue_depth must be 1-%d"), RGB_MAX_QUEUE_DEPTH);

    mp_lcd_rgb_bus_obj_t *self = m_new_obj(mp_lcd_rgb_bus_obj_t);
    self->base.type = &mp_lcd_rgb_bus_type;
    self->lane_count = (int)n;
    self->hsync_pin = args[ARG_hsync].u_int;
    self->vsync_pin = args[ARG_vsync].u_int;
    self->de_pin    = args[ARG_de].u_int;
    self->pclk_pin  = args[ARG_pclk].u_int;
    self->disp_pin  = args[ARG_disp].u_int;
    self->panel_w   = args[ARG_width].u_int;
    self->panel_h   = args[ARG_height].u_int;
    self->freq      = args[ARG_freq].u_int;
    self->queue_depth = qdepth;

    for (int i = 0; i < 16; i++)
        self->data_pins[i] = (i < (int)n) ? mp_obj_get_int(items[i]) : -1;

    esp_lcd_rgb_panel_config_t pc = {
        .clk_src = LCD_CLK_SRC_PLL160M,
        .timings = {
            .pclk_hz         = (uint32_t)self->freq,
            .h_res           = (uint32_t)self->panel_w,
            .v_res           = (uint32_t)self->panel_h,
            .hsync_back_porch  = (uint32_t)args[ARG_hsync_back_porch].u_int,
            .hsync_front_porch = (uint32_t)args[ARG_hsync_front_porch].u_int,
            .hsync_pulse_width = (uint32_t)args[ARG_hsync_pulse_width].u_int,
            .vsync_back_porch  = (uint32_t)args[ARG_vsync_back_porch].u_int,
            .vsync_front_porch = (uint32_t)args[ARG_vsync_front_porch].u_int,
            .vsync_pulse_width = (uint32_t)args[ARG_vsync_pulse_width].u_int,
            .flags = {
                .hsync_idle_low  = (uint32_t)args[ARG_hsync_idle_low].u_bool,
                .vsync_idle_low  = (uint32_t)args[ARG_vsync_idle_low].u_bool,
                .de_idle_high    = (uint32_t)args[ARG_de_idle_high].u_bool,
                .pclk_active_neg = (uint32_t)args[ARG_pclk_active_neg].u_bool,
                .pclk_idle_high  = (uint32_t)args[ARG_pclk_idle_high].u_bool,
            },
        },
        .data_width         = (size_t)n,
        .bits_per_pixel     = 16,
        .num_fbs            = 0,
        .bounce_buffer_size_px = (size_t)args[ARG_bb_size_px].u_int,
        .sram_trans_align   = 64,
        .psram_trans_align  = 64,
        .hsync_gpio_num     = self->hsync_pin,
        .vsync_gpio_num     = self->vsync_pin,
        .de_gpio_num        = self->de_pin,
        .pclk_gpio_num      = self->pclk_pin,
        .disp_gpio_num      = self->disp_pin,
        .flags = {
            .disp_active_low   = (uint32_t)args[ARG_disp_active_low].u_bool,
            .refresh_on_demand = (uint32_t)args[ARG_refresh_on_demand].u_bool,
            .fb_in_psram       = 0,
            .no_fb             = true,
        },
    };
    for (int i = 0; i < 16; i++) pc.data_gpio_nums[i] = self->data_pins[i];

    if (s_last_rgb_panel) { esp_lcd_panel_del(s_last_rgb_panel); s_last_rgb_panel = NULL; }

    esp_err_t ret = esp_lcd_new_rgb_panel(&pc, &self->panel_handle);
    if (ret != ESP_OK) {
        m_del_obj(mp_lcd_rgb_bus_obj_t, self);
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("esp_lcd_new_rgb_panel err=0x%x"), ret);
    }

    esp_lcd_rgb_panel_event_callbacks_t cbs = { .on_vsync = on_vsync };
    if (esp_lcd_rgb_panel_register_event_callbacks(self->panel_handle, &cbs, self) != ESP_OK) {
        esp_lcd_panel_del(self->panel_handle);
        m_del_obj(mp_lcd_rgb_bus_obj_t, self);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("register callback"));
    }
    if (esp_lcd_panel_reset(self->panel_handle) != ESP_OK) {
        esp_lcd_panel_del(self->panel_handle);
        m_del_obj(mp_lcd_rgb_bus_obj_t, self);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("panel_reset"));
    }
    if (esp_lcd_panel_init(self->panel_handle) != ESP_OK) {
        esp_lcd_panel_del(self->panel_handle);
        m_del_obj(mp_lcd_rgb_bus_obj_t, self);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("panel_init"));
    }

    memset(self->ref_bufs, 0, sizeof(self->ref_bufs));
    memset(self->done_flags, 0, sizeof(self->done_flags));
    self->queue_head = self->queue_tail = self->queue_count = 0;
    memset(self->pending_segments, 0, sizeof(self->pending_segments));
    // 視窗預設全螢幕, 流式位置從 (0,0) 開始
    self->win_x0 = self->win_y0 = 0;
    self->win_x1 = self->panel_w - 1;
    self->win_y1 = self->panel_h - 1;
    self->pos_x = self->pos_y = 0;
    self->initialized = true;

    s_last_rgb_panel = self->panel_handle;

    return MP_OBJ_FROM_PTR(self);
}


static mp_obj_t rgb_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
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

    mp_lcd_rgb_bus_obj_t *self = (mp_lcd_rgb_bus_obj_t *)args[ARG_self].u_obj;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_buf].u_obj, &bufinfo, MP_BUFFER_READ);

    if (self->panel_handle == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("bus deinitialized"));
    }
    if (self->queue_count >= self->queue_depth)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("queue full"));

    int idx = self->queue_tail;
    self->ref_bufs[idx] = args[ARG_buf].u_obj;
    self->done_flags[idx] = false;

    bool explicit_region = (args[ARG_x].u_int != -1 || args[ARG_y].u_int != -1 ||
                            args[ARG_w].u_int != -1 || args[ARG_h].u_int != -1);

    const int bpp = 2;   // RGB565
    size_t px_total = bufinfo.len / (size_t)bpp;

    #define RGB_MAX_SEGS 600
    int seg_x[RGB_MAX_SEGS], seg_y[RGB_MAX_SEGS], seg_w[RGB_MAX_SEGS], seg_h[RGB_MAX_SEGS];
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
        size_t px_left = px_total;
        int px = self->pos_x;
        int py = self->pos_y;
        while (px_left > 0) {
            if (py > self->win_y1) {
                // 寫滿視窗 → 自動 wrap 回視窗起點 (連續整幀/分批輸入天然成立)
                px = self->win_x0;
                py = self->win_y0;
            }
            int row_left = self->win_x1 - px + 1;
            if (row_left <= 0) {
                px = self->win_x0;
                py++;
                continue;
            }
            size_t n_px = px_left;
            if (n_px > (size_t)row_left) n_px = row_left;
            if (nsegs < RGB_MAX_SEGS) {
                seg_x[nsegs] = px;
                seg_y[nsegs] = py;
                seg_w[nsegs] = (int)n_px;
                seg_h[nsegs] = 1;
                nsegs++;
            } else {
                break;
            }
            px += (int)n_px;
            px_left -= n_px;
            if (px > self->win_x1) {
                px = self->win_x0;
                py++;
            }
        }
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
        self->ref_bufs[idx] = mp_const_none;
        self->done_flags[idx] = true;
        self->queue_tail = (self->queue_tail + 1) % self->queue_depth;
        self->queue_count++;
        return mp_obj_new_int(idx + 1);
    }

    self->pending_segments[idx] = nsegs;
    self->queue_tail = (self->queue_tail + 1) % self->queue_depth;
    self->queue_count++;

    // RGB draw_bitmap 是同步拷貝 (可連續呼叫) — 直接連發所有段
    const uint8_t *src = (const uint8_t *)bufinfo.buf;
    size_t src_off = 0;
    for (int k = 0; k < nsegs; k++) {
        if (esp_lcd_panel_draw_bitmap(self->panel_handle,
                                      seg_x[k], seg_y[k],
                                      seg_x[k] + seg_w[k], seg_y[k] + seg_h[k],
                                      src + src_off) != ESP_OK) {
            self->pending_segments[idx] = 0;
            self->done_flags[idx] = true;
            self->ref_bufs[idx] = mp_const_none;
            self->queue_head = (self->queue_head + 1) % self->queue_depth;
            self->queue_count--;
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("draw_bitmap failed"));
        }
        src_off += (size_t)seg_w[k] * seg_h[k] * (size_t)bpp;
    }
    return mp_obj_new_int(idx + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(rgb_write_obj, 2, rgb_write);


static mp_obj_t rgb_set_window(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
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

    mp_lcd_rgb_bus_obj_t *self = (mp_lcd_rgb_bus_obj_t *)args[ARG_self].u_obj;
    if (self->panel_handle == NULL) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("bus deinitialized"));
    }
    int x0 = args[ARG_x0].u_int;
    int y0 = args[ARG_y0].u_int;
    int x1 = args[ARG_x1].u_int;
    int y1 = args[ARG_y1].u_int;
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
static MP_DEFINE_CONST_FUN_OBJ_KW(rgb_set_window_obj, 1, rgb_set_window);


static mp_obj_t rgb_is_busy(mp_obj_t self_in) {
    return mp_obj_new_bool(((mp_lcd_rgb_bus_obj_t *)self_in)->queue_count > 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(rgb_is_busy_obj, rgb_is_busy);

static mp_obj_t rgb_pending(mp_obj_t self_in) {
    return mp_obj_new_int(((mp_lcd_rgb_bus_obj_t *)self_in)->queue_count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(rgb_pending_obj, rgb_pending);

static mp_obj_t rgb_lane_count(mp_obj_t self_in) {
    return mp_obj_new_int(((mp_lcd_rgb_bus_obj_t *)self_in)->lane_count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(rgb_lane_count_obj, rgb_lane_count);


static mp_obj_t rgb_wait(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_trans_id, ARG_timeout_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self,       MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_trans_id,   MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_timeout_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_rgb_bus_obj_t *self = (mp_lcd_rgb_bus_obj_t *)args[ARG_self].u_obj;
    int tid = args[ARG_trans_id].u_int;
    int to  = args[ARG_timeout_ms].u_int;
    int idx = tid - 1;

    if (idx < 0 || idx >= self->queue_depth) return mp_const_false;
    if (self->ref_bufs[idx] == mp_const_none) return mp_const_true;

    mp_uint_t deadline = mp_hal_ticks_ms() + (to < 0 ? 10000 : to);
    while (!self->done_flags[idx]) {
        if (to >= 0 && mp_hal_ticks_ms() > deadline) return mp_const_false;
        mp_hal_delay_ms(1);
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(rgb_wait_obj, 2, rgb_wait);


static mp_obj_t rgb_wait_all(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_timeout_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self,       MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_timeout_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_rgb_bus_obj_t *self = (mp_lcd_rgb_bus_obj_t *)args[ARG_self].u_obj;
    int to = args[ARG_timeout_ms].u_int;
    mp_uint_t deadline = mp_hal_ticks_ms() + (to < 0 ? 10000 : to);

    while (self->queue_count > 0) {
        if (to >= 0 && mp_hal_ticks_ms() > deadline) break;
        mp_hal_delay_ms(1);
    }
    // ⚠ 效能修復（同 spi_bus）：正常 drain 完畢不需立即 GC — 每幀 flush 都會呼叫
    // wait_all，full GC 在 PSRAM heap + 大 frame buffer 上可達 ~200ms（實測），
    // 是「每幀慢 200ms」的主因。僅在逾時殘留（queue 未清空）時才強制釋放 + gc_collect。
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
static MP_DEFINE_CONST_FUN_OBJ_KW(rgb_wait_all_obj, 1, rgb_wait_all);


static mp_obj_t rgb_deinit(mp_obj_t self_in) {
    mp_lcd_rgb_bus_obj_t *self = (mp_lcd_rgb_bus_obj_t *)self_in;
    if (!self->initialized) return mp_const_none;
    if (s_last_rgb_panel == self->panel_handle) s_last_rgb_panel = NULL;
    esp_lcd_panel_del(self->panel_handle);

    int8_t pins[21];
    int p = 0;
    pins[p++] = self->hsync_pin;
    pins[p++] = self->vsync_pin;
    pins[p++] = self->de_pin;
    pins[p++] = self->pclk_pin;
    if (self->disp_pin != -1) pins[p++] = self->disp_pin;
    for (int i = 0; i < self->lane_count; i++) {
        if (self->data_pins[i] != -1) pins[p++] = self->data_pins[i];
    }
    for (int i = 0; i < p; i++) {
        esp_rom_gpio_pad_select_gpio(pins[i]);
        esp_rom_gpio_connect_out_signal(pins[i], SIG_GPIO_OUT_IDX, false, false);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
    }

    self->initialized = false;
    // ⚠ 修復：deinit 後 panel_handle 清 NULL，避免 use-after-free；
    //    並清 ref_bufs / done_flags（原先殘留釘住 buffer 引用 → GC 洩漏）
    self->panel_handle = NULL;
    for (int i = 0; i < self->queue_depth; i++) {
        self->ref_bufs[i] = mp_const_none;
        self->done_flags[i] = false;
        self->pending_segments[i] = 0;
    }
    self->queue_count = 0;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(rgb_deinit_obj, rgb_deinit);


static const mp_rom_map_elem_t rgb_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write),      MP_ROM_PTR(&rgb_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_window), MP_ROM_PTR(&rgb_set_window_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_busy),    MP_ROM_PTR(&rgb_is_busy_obj) },
    { MP_ROM_QSTR(MP_QSTR_pending),    MP_ROM_PTR(&rgb_pending_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait),       MP_ROM_PTR(&rgb_wait_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_all),   MP_ROM_PTR(&rgb_wait_all_obj) },
    { MP_ROM_QSTR(MP_QSTR_lane_count), MP_ROM_PTR(&rgb_lane_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),     MP_ROM_PTR(&rgb_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),    MP_ROM_PTR(&rgb_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(rgb_locals_dict, rgb_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    mp_lcd_rgb_bus_type,
    MP_QSTR_RGBBus,
    MP_TYPE_FLAG_NONE,
    make_new, rgb_make_new,
    locals_dict, (mp_obj_dict_t *)&rgb_locals_dict
);

#endif
