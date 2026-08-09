#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED

#include "dsi_bus.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_memory_utils.h"

#include "py/runtime.h"
#include "py/objarray.h"
#include "py/mphal.h"
#include "py/gc.h"
#include "driver/gpio.h"

#include <string.h>

static mp_lcd_dsi_bus_obj_t *s_last_dsi = NULL;

// ── PSRAM 判定 + bounce ──────────────────────────────────────────────
// PPA 內部會對 src 呼叫 esp_cache_msync(C2M)，只接受 PSRAM cacheable
// 位址；DRAM src 會觸發 ESP_ERR_INVALID_ARG 並刷屏 ERROR。
// → src 在 DRAM 時先整段 memcpy 到 PSRAM bounce，再交給 PPA。
// → src 在 PSRAM 時零拷貝直接進 PPA（硬體搬運，CPU 不碰像素）。
static inline bool _dsi_is_psram(const void *p) {
    return p != NULL && esp_ptr_external_ram(p);
}

static bool _dsi_ensure_bounce(mp_lcd_dsi_bus_obj_t *self, size_t need) {
    if (self->bounce_buf != NULL && self->bounce_size >= need) return true;
    uint8_t *nb = (uint8_t *)heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (nb == NULL) return false;
    if (self->bounce_buf != NULL) heap_caps_free(self->bounce_buf);
    self->bounce_buf = nb;
    self->bounce_size = need;
    return true;
}

static bool on_color_trans_done(esp_lcd_panel_handle_t panel,
                                esp_lcd_dpi_panel_event_data_t *edata, void *ctx) {
    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)ctx;
    // 只認 DMA 槽 (slot_kind==0); 跳過 present 槽 (slot_kind==1)。
    // ⚠ 不能用「第一個未完成槽就 break」— DMA 與 present 槽可能交錯排隊,
    //   頭上若不是自己的來源要跳過去找, 否則互相卡死。
    for (int i = 0; i < self->queue_depth; i++) {
        if (self->ref_bufs[i] != mp_const_none && !self->done_flags[i]
            && self->slot_kind[i] == 0) {
            // 流式 write 會拆成多段 DMA2D 拷貝 — 每段完成遞減一次,
            // 最後一段完成才算該 write 完成 (queue 槽釋放)
            if (self->pending_segments[i] > 0) {
                self->pending_segments[i]--;
                if (self->pending_segments[i] == 0) {
                    self->done_flags[i] = true;
                    self->ref_bufs[i] = mp_const_none;
                    self->slot_kind[i] = 0;      // 釋放即歸零 (下次入隊也會重設)
                    self->queue_head = (self->queue_head + 1) % self->queue_depth;
                    self->queue_count--;
                }
            }
            break;   // 一次 trans-done 只對應一段
        }
    }
    return false;
}

// 幀邊界 (VSYNC) callback — present() 提交 page-flip 後,
// 在此處確認「上一幀掃完、新 fb 已生效」, 該槽才算完成。
// 只認 present 槽 (slot_kind==1); 跳過 DMA 槽。
static bool on_refresh_done(esp_lcd_panel_handle_t panel,
                            esp_lcd_dpi_panel_event_data_t *edata, void *ctx) {
    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)ctx;
    for (int i = 0; i < self->queue_depth; i++) {
        if (self->ref_bufs[i] != mp_const_none && !self->done_flags[i]
            && self->slot_kind[i] == 1) {
            self->done_flags[i] = true;
            self->ref_bufs[i] = mp_const_none;
            self->slot_kind[i] = 0;      // ⚠ 必重置: 不重置則下次 write() 輪到此槽
            self->queue_head = (self->queue_head + 1) % self->queue_depth;   // 會被當 present 槽跳過 → 卡死
            self->queue_count--;
            // 3-fb 管線: 若完成的是 blit_pipeline 提交的 slot,
            // 釋放 pending → 該 fb 從「待翻」變「顯示中」,上一個顯示中 fb 變可寫。
            // (DMA EOF callback 在 IDF 重 arm 到新 fb 之後觸發,故此時翻頁已生效)
            if (self->pl_pending_tid == i) {
                self->pl_pending_fb = -1;
                self->pl_pending_tid = -1;
            }
            break;   // 一個幀邊界只對應一個 present 槽
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
    // PPA client 釋放 (SRM engine ref count 遞減; 最後一個 client 會關 PPA 時鐘)
    if (self->ppa_client) {
        ppa_unregister_client(self->ppa_client);
        self->ppa_client = NULL;
    }
    if (self->bounce_buf != NULL) {
        heap_caps_free(self->bounce_buf);
        self->bounce_buf = NULL;
        self->bounce_size = 0;
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

    esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done = on_color_trans_done,
        .on_refresh_done     = on_refresh_done,
    };
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

    // 註冊 PPA SRM client — write() 用它做硬體 2D blit (src → 後台 fb)。
    // use_dma2d=True (預設) 時才註冊；False 或註冊失敗 → write() 自動退回
    // CPU memcpy 路徑 (功能不受影響，只是 blit 佔 CPU)。
    // max_pending_trans_num=1: write() 用 BLOCKING 模式, 一次一筆即可。
    self->ppa_client = NULL;
    if (args[ARG_use_dma2d].u_bool) {
        ppa_client_config_t ppa_cfg = {
            .oper_type = PPA_OPERATION_SRM,
            .max_pending_trans_num = 1,
        };
        if (ppa_register_client(&ppa_cfg, &self->ppa_client) != ESP_OK) {
            self->ppa_client = NULL;   // 退回 CPU 路徑
        }
    }

    memset(self->ref_bufs, 0, sizeof(self->ref_bufs));
    memset(self->done_flags, 0, sizeof(self->done_flags));
    memset(self->pending_segments, 0, sizeof(self->pending_segments));
    memset(self->slot_kind, 0, sizeof(self->slot_kind));
    self->queue_head = self->queue_tail = self->queue_count = 0;
    self->cur_fb = 0;       // fb0 為初始顯示中; back_buffer()/present() 用 1-cur_fb 選離屏
    // 3-fb 管線初始狀態: 無 pending, 可寫 fb = 1 (fb0 顯示中, fb1 可寫, fb2 備用)
    self->pl_free_fb = 1;
    self->pl_pending_fb = -1;
    self->pl_pending_tid = -1;
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

    bool explicit_region = (args[ARG_x].u_int != -1 || args[ARG_y].u_int != -1 ||
                            args[ARG_w].u_int != -1 || args[ARG_h].u_int != -1);

    int bpp = self->bits_per_pixel / 8;    // bytes per pixel
    size_t px_total = bufinfo.len / (size_t)bpp;   // 可寫像素數 (尾巴忽略)

    // 段表: 流式 write 可能跨行, 每段 = 一個 2D 矩形 (PPA 硬體 blit / CPU memcpy)。
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
        // 以「窗內線性偏移」追蹤位置，最簡且正確：
        //   L = (py-win_y0)*region_w + (px-win_x0)，範圍 [0, region_w*region_h)
        // 每段產出一個「真正的 2D 矩形」(seg_w × seg_h)：
        //   • 行首對齊且剩餘 ≥ 一整行 → 合併連續完整行 (w=region_w, h=n_rows)
        //   • 否則 → 寫到當前行尾 (w=剩餘像素, h=1)
        // 注意：合併行段的寬必須是 region_w（不是 n_rows*region_w），
        // 否則 PPA/CPU 2D blit 會把多行誤當超寬單行 → 畫面錯位。
        size_t region_w = (size_t)(self->win_x1 - self->win_x0 + 1);
        size_t region_h = (size_t)(self->win_y1 - self->win_y0 + 1);
        size_t region_px = region_w * region_h;
        size_t px_left = px_total;
        // 目前線性位置 (wrap 到窗內)
        size_t L = (size_t)(self->pos_y - self->win_y0) * region_w
                 + (size_t)(self->pos_x - self->win_x0);
        L %= region_px;
        while (px_left > 0 && nsegs < DSI_MAX_SEGS) {
            size_t row_off = L % region_w;        // 行內偏移
            size_t row     = L / region_w;        // 行號 (窗內)
            size_t seg_w_, seg_h_, n_px;
            if (row_off == 0 && px_left >= region_w) {
                // 行首對齊 → 合併連續完整行
                size_t n_rows = px_left / region_w;
                size_t rows_left = region_h - row;
                if (n_rows > rows_left) n_rows = rows_left;
                seg_w_ = region_w;
                seg_h_ = n_rows;
                n_px   = n_rows * region_w;
            } else {
                // 行中 (或剩餘不足一行) → 寫到當前行尾
                size_t row_left = region_w - row_off;
                n_px = px_left < row_left ? px_left : row_left;
                seg_w_ = n_px;
                seg_h_ = 1;
            }
            seg_x[nsegs] = self->win_x0 + (int)row_off;
            seg_y[nsegs] = self->win_y0 + (int)row;
            seg_w[nsegs] = (int)seg_w_;
            seg_h[nsegs] = (int)seg_h_;
            nsegs++;
            L = (L + n_px) % region_px;   // 前進並 wrap
            px_left -= n_px;
        }
        // 更新流式位置 (線性位置還原成 x/y)
        self->pos_x = self->win_x0 + (int)(L % region_w);
        self->pos_y = self->win_y0 + (int)(L / region_w);
    }

    if (nsegs == 0) {
        return mp_obj_new_int(0);    // 空寫入: 回傳 "0 tid" (同步完成, 不需 wait)
    }

    // ──────────────────────────────────────────────────────────────
    // 合約 (P4 + DSI + fb_count>=2 雙緩)：
    //   write(buf) 把像素搬到「後台 frame buffer」，絕不碰前台 → 零撕裂。
    //   present() 才在 VSYNC 原子切換。
    //
    // 搬運引擎 (依序選擇):
    //   1. PPA SRM (P4 的 DMA2D 硬體): 2D 硬體 blit, CPU 不碰像素。
    //      PPA 內部自動處理 cache (src C2M + dst M2C invalidate)。
    //      ⚠ PPA 的內部 msync 只接受 PSRAM 位址 → src 在 DRAM 時
    //        先 memcpy 到 PSRAM bounce (一次 CPU 拷貝, 之後仍是硬體搬)。
    //   2. CPU 逐行 memcpy (無 PPA / PPA 失敗時退回; fb_count==1 也可用)。
    // ──────────────────────────────────────────────────────────────
    int target_fb = (self->num_fbs >= 2) ? (1 - self->cur_fb) : 0;
    uint8_t *dst_fb = (uint8_t *)self->fbs[target_fb];
    const uint8_t *src = (const uint8_t *)bufinfo.buf;
    size_t src_off = 0;
    const int stride = self->panel_w * bpp;   // frame buffer 一行的 bytes
    ppa_srm_color_mode_t cm = (bpp == 3) ? PPA_SRM_COLOR_MODE_RGB888
                                         : PPA_SRM_COLOR_MODE_RGB565;
    // CPU fallback 寫過的外接矩形 (只 flush 這些區域 — 不能 flush PPA 寫過的
    // 區域: CPU cache 裡是舊值, C2M 會把舊值蓋回 PSRAM 覆蓋 PPA 的 DMA 結果)
    int crx = self->panel_w, cry = self->panel_h, crx2 = 0, cry2 = 0;

    for (int k = 0; k < nsegs; k++) {
        int x = seg_x[k], y = seg_y[k], w = seg_w[k], h = seg_h[k];
        size_t seg_bytes = (size_t)w * h * bpp;
        const void *blit_src = src + src_off;
        src_off += seg_bytes;

        bool done = false;
        if (self->ppa_client != NULL) {
            // DRAM src → PPA 內部 msync 會報 invalid addr → 先 bounce 到 PSRAM
            if (!_dsi_is_psram(blit_src)) {
                if (_dsi_ensure_bounce(self, seg_bytes)) {
                    memcpy(self->bounce_buf, blit_src, seg_bytes);
                    blit_src = self->bounce_buf;
                } else {
                    blit_src = NULL;   // bounce OOM → 本段退回 CPU
                }
            }
            if (blit_src != NULL) {
                ppa_srm_oper_config_t op = {0};
                op.in.buffer = blit_src;
                op.in.pic_w = w;
                op.in.pic_h = h;
                op.in.block_w = w;
                op.in.block_h = h;
                op.in.block_offset_x = 0;
                op.in.block_offset_y = 0;
                op.in.srm_cm = cm;
                op.out.buffer = dst_fb;
                op.out.buffer_size = self->fb_size;
                op.out.pic_w = self->panel_w;
                op.out.pic_h = self->panel_h;
                op.out.block_offset_x = x;
                op.out.block_offset_y = y;
                op.out.srm_cm = cm;
                op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
                op.scale_x = 1.0f;
                op.scale_y = 1.0f;
                op.alpha_update_mode = PPA_ALPHA_NO_CHANGE;
                op.mode = PPA_TRANS_MODE_BLOCKING;   // 返回即完成, bounce 可安全重用
                if (ppa_do_scale_rotate_mirror(self->ppa_client, &op) == ESP_OK) {
                    done = true;
                }
            }
        }
        if (!done) {
            // CPU fallback: 逐行 memcpy (無 PPA、bounce OOM 或 PPA 報錯時)
            uint8_t *dp = dst_fb + (size_t)y * stride + (size_t)x * bpp;
            const uint8_t *sp = src + (src_off - seg_bytes);
            size_t row_bytes = (size_t)w * bpp;
            for (int r = 0; r < h; r++) {
                memcpy(dp, sp, row_bytes);
                dp += stride;
                sp += row_bytes;
            }
            // 累計 CPU 寫入的外接矩形
            if (x < crx) crx = x;
            if (y < cry) cry = y;
            if (x + w > crx2) crx2 = x + w;
            if (y + h > cry2) cry2 = y + h;
        }
    }

    // CPU fallback 寫後台 fb 走 write-back cache → 局部 C2M flush (僅 CPU 區域)。
    // (PPA 路徑已自己處理 cache, 不需再 flush; present() 也會再做整幀 msync)
    {
        int rw = crx2 - crx, rh = cry2 - cry;
        if (rw > 0 && rh > 0) {
            uint8_t *start = dst_fb + ((size_t)cry * self->panel_w + crx) * bpp;
            size_t bytes = (size_t)rw * rh * bpp;
            size_t max_bytes = dst_fb + self->fb_size - start;
            if (bytes > max_bytes) bytes = max_bytes;
            if (bytes > 0) {
                esp_cache_msync(start, bytes,
                                ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
            }
        }
    }

    // write() 同步完成，不佔用 queue 槽。
    // 回傳 tid=0 表示「不需要 wait」；上層呼叫 wait(0) 直接通過。
    return mp_obj_new_int(0);
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
    //
    // ⚠ size 必須是矩形 (x..x+w, y..y+h) 佔用的位元組，不能用 panel_w * h：
    // 否則 x>0 / w<panel_w 時 start+size 會越過本 fb 範圍，esp_cache_msync
    // 回 ESP_ERR_INVALID_ARG (103)，也就是用戶看到的 invalid addr 錯誤。
    int bpp_bytes = self->bits_per_pixel / 8;
    uint8_t *fb_base = (uint8_t *)self->fbs[idx];
    // 按 cache line 把 size 擴到「所有受影響的行」— cache 是以 cache-line
    // 為單位回寫，行內局部寫入也需要該行完整的 cache 線被寫回。
    // 為安全起見：從 (y 行起始) 到 (y+h-1 行結束) 的整行區間做 msync，
    // 因為 start 可能不對齊，UNALIGNED flag 已處理頭尾不對齊；
    // 但 size 只覆蓋 w*h*bpp 也正確 (UNALIGNED 會把頭尾對齊到 cache line)。
    size_t size = (size_t)w * h * bpp_bytes;
    uint8_t *start = fb_base + ((size_t)y * self->panel_w + x) * bpp_bytes;
    // ⚠ 邊界防衛：start+size 不超過本 fb 尾端（避免 esp_cache_msync invalid addr）
    size_t max_size = fb_base + self->fb_size - start;
    if (size > max_size) size = max_size;
    if (start >= fb_base && start < fb_base + self->fb_size && size > 0) {
        esp_cache_msync(start, size, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
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

    // tid == 0: dsi_write 同步完成（新合約），不需要等，直接 true
    if (tid == 0) return mp_const_true;

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
            self->slot_kind[i] = 0;
        }
        self->queue_head = self->queue_tail = self->queue_count = 0;
        gc_collect();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dsi_wait_all_obj, 1, dsi_wait_all);


// back_buffer() — 回傳目前「離屏」framebuffer 的零拷貝 view。
// C 內部依 cur_fb 決定離屏 (1-cur_fb), 上層不需知道哪塊在顯示、也不傳 index。
// 用法: bus.back_buffer()[:] = data  (或 framebuf 直接以它為 backing)。
static mp_obj_t dsi_back_buffer(mp_obj_t self_in) {
    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)self_in;
    if (self->num_fbs < 2 || self->fbs[0] == NULL || self->fbs[1] == NULL)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("page-flip needs fb_count>=2"));
    int back = 1 - self->cur_fb;
    return mp_obj_new_bytearray_by_ref(self->fb_size, self->fbs[back]);
}
static MP_DEFINE_CONST_FUN_OBJ_1(dsi_back_buffer_obj, dsi_back_buffer);


// present([timeout_ms]) — 整頁原子更新: 把 back_buffer() 指向的離屏 fb 提交為
// 顯示目標, 在幀邊界 (VSYNC) 原子切換 (IDF 的 page-flip 優化: draw_bitmap 來源是
// 內部 fb → 只 cache writeback + 切 cur_fb_index, 不拷貝)。
//
// 流程全在 C 內部: 選離屏 → draw_bitmap 觸發切換 → 入 queue 槽
// (slot_kind=1, 等幀邊界) → 回 tid。Python 端用既有 wait(tid)/wait_all() 等完成,
// 與 write() 完全同款契約, 零新 wait API。
//
// ⚠ 效能關鍵 (2026-08): 不要在這裡做顯式 esp_cache_msync — IDF draw_bitmap 的
// no-copy 路徑 (src == 內部 fb) 自己會做全幀 C2M (esp_lcd_panel_dpi.c)。
// 再做一次 = 每幀重複寫回 1.2MB PSRAM (~6-8ms), 使每幀工作量超過幀週期,
// wait 相位鎖定在 2×幀週期 (65Hz → 31ms/幀 → 32 FPS)。
static mp_obj_t dsi_present(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_timeout_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self,       MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_timeout_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)args[ARG_self].u_obj;
    if (self->dpi_panel == NULL)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("bus deinitialized"));
    if (self->num_fbs < 2 || self->fbs[0] == NULL || self->fbs[1] == NULL)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("page-flip needs fb_count>=2"));
    if (self->queue_count >= self->queue_depth)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("queue full"));

    int idx = self->queue_tail;
    int back = 1 - self->cur_fb;
    // 入隊 (與 write() 同款: 先佔槽再 draw_bitmap)
    self->ref_bufs[idx] = MP_OBJ_FROM_PTR(self);   // 非 none 即可; GC 不影響 (內部 fb 不靠此釘住)
    self->done_flags[idx] = false;
    self->slot_kind[idx] = 1;          // 等幀邊界 (on_refresh_done)
    self->pending_segments[idx] = 0;   // 不走 DMA 段計數
    self->cur_fb = back;               // 離屏現在是顯示目標
    self->queue_tail = (self->queue_tail + 1) % self->queue_depth;
    self->queue_count++;

    // IDF 範圍檢查命中內部 fb → 只做 cache writeback + 切 cur_fb_index (不拷貝)。
    // cache 一致性由 IDF no-copy 路徑的 esp_cache_msync 負責 — 見上方效能註解。
    esp_err_t ret = esp_lcd_panel_draw_bitmap(
        self->dpi_panel, 0, 0, self->panel_w, self->panel_h, self->fbs[back]);
    if (ret != ESP_OK) {
        // 失敗: 釋放槽位, 別卡住 queue
        self->ref_bufs[idx] = mp_const_none;
        self->done_flags[idx] = true;
        self->slot_kind[idx] = 0;
        self->queue_head = (self->queue_head + 1) % self->queue_depth;
        self->queue_count--;
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("present (draw_bitmap) failed err=0x%x"), ret);
    }

    // 非同步 (與 write() 一致, 回 tid); 等/不等由上層決定 (wait(tid) 或繼續)。
    (void)args[ARG_timeout_ms].u_int;
    return mp_obj_new_int(idx + 1);    // tid — 與 write() 同契約
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dsi_present_obj, 1, dsi_present);


// blit_pipeline(buf) — 3-fb 管線整幀更新 (fb_count>=3 才能用)。
//
// 三角色同時存在,達成「CPU 拷貝與 DMA 掃描重疊」逼近面板上限 (~65 FPS):
//   cur_fb        正在掃描顯示 (DMA 讀)
//   pl_pending_fb 已提交等下一個 VSYNC (IDF draw_bitmap 同步 msync + 設 cur_fb_index)
//   pl_free_fb    CPU 正在 memcpy 寫入 (本函式目標)
//
// 流程 (每次呼叫):
//   1. 若有 pending (上一幀還在等 VSYNC), 等 on_refresh_done 釋放 (pl_pending_fb==-1)
//      → IDF DPI 同一時間只允許 1 個 pending flip, 這步確保不會覆蓋上一個
//   2. memcpy(buf → fbs[pl_free_fb])        ← 14.5ms, 與 DMA 掃上一幀並行
//   3. draw_bitmap(fbs[pl_free_fb])          ← IDF 同步 msync + 設 cur_fb_index
//   4. 入 queue slot (slot_kind=1, 復用 present 路徑) → 回 tid
//   5. pl_pending_fb/tid 設為本幀; pl_free_fb 輪轉到下一個閒置 fb
//
// 上層契約: blit_pipeline(buf) 回 tid, blit_pipeline_wait() 或 wait(tid) 等翻頁完成。
// 撕裂: 永遠只寫不在(顯示中/pending)的 fb → 零撕裂; 翻頁在 VSYNC 原子切換。
static mp_obj_t dsi_blit_pipeline(mp_obj_t self_in, mp_obj_t buf_in) {
    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)self_in;
    if (self->dpi_panel == NULL)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("bus deinitialized"));
    if (self->num_fbs < 3 || self->fbs[2] == NULL)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("blit_pipeline needs fb_count>=3"));

    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buf_in, &bufinfo, MP_BUFFER_READ);
    size_t copy_bytes = bufinfo.len < self->fb_size ? bufinfo.len : self->fb_size;

    // ── 1. 等上一個 pending 翻頁完成 (IDF 單 flip 限制) ──
    //    on_refresh_done 會清 pl_pending_*; 用 1ms 輪詢 (與 wait() 同粒度)。
    //    極端情況(首幀 / 剛 drain) pl_pending_fb 已是 -1 → 零等待直入。
    mp_uint_t deadline = mp_hal_ticks_ms() + 10000;   // 10s 保守逾時
    while (self->pl_pending_fb != -1) {
        if (mp_hal_ticks_ms() > deadline)
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("blit_pipeline: pending flip timeout"));
        mp_hal_delay_ms(1);
    }

    // ── 2. 取閒置 fb (≠ cur_fb 顯示中, ≠ pl_pending_fb 待翻) ──
    int dst = -1;
    for (int i = 0; i < self->num_fbs; i++) {
        if (i != self->cur_fb && i != self->pl_pending_fb) {   // pl_pending_fb 此時必為 -1
            dst = i;
            break;
        }
    }
    if (dst < 0)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("blit_pipeline: no free fb"));

    // ── 3. CPU memcpy 整幀到閒置 fb (與 DMA 掃 cur_fb 並行, 不阻塞顯示) ──
    memcpy(self->fbs[dst], bufinfo.buf, copy_bytes);
    // 不需自己做 esp_cache_msync — 下一步 draw_bitmap 的 no-copy 路徑會做全幀 C2M。

    // ── 4. 提交 page-flip (IDF 同步 msync + 設 cur_fb_index) ──
    if (self->queue_count >= self->queue_depth)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("queue full"));

    int idx = self->queue_tail;
    self->ref_bufs[idx] = MP_OBJ_FROM_PTR(self);
    self->done_flags[idx] = false;
    self->slot_kind[idx] = 1;          // 等 DMA EOF (on_refresh_done)
    self->pending_segments[idx] = 0;
    self->cur_fb = dst;                // 離屏現在是顯示目標 (IDF 會同步設 cur_fb_index)
    self->queue_tail = (self->queue_tail + 1) % self->queue_depth;
    self->queue_count++;

    esp_err_t ret = esp_lcd_panel_draw_bitmap(
        self->dpi_panel, 0, 0, self->panel_w, self->panel_h, self->fbs[dst]);
    if (ret != ESP_OK) {
        // 失敗: 釋放槽位, 別卡住 queue
        self->ref_bufs[idx] = mp_const_none;
        self->done_flags[idx] = true;
        self->slot_kind[idx] = 0;
        self->queue_head = (self->queue_head + 1) % self->queue_depth;
        self->queue_count--;
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("blit_pipeline (draw_bitmap) failed err=0x%x"), ret);
    }

    // ── 5. 更新管線狀態: 本幀變 pending, 輪轉 pl_free_fb ──
    self->pl_pending_fb = dst;
    self->pl_pending_tid = idx;
    // pl_free_fb 留給下次開頭重算 (step 2 的迴圈會找); 這裡不預存避免過期。

    return mp_obj_new_int(idx + 1);    // tid — 同 present() 契約, wait(tid) 等翻頁完成
}
static MP_DEFINE_CONST_FUN_OBJ_2(dsi_blit_pipeline_obj, dsi_blit_pipeline);


// _pl_state() — 診斷用:回傳 (pl_free_fb, pl_pending_fb, pl_pending_tid, cur_fb, queue_count)
// 讓 Python 端能觀察 3-fb 管線內部狀態, 定位卡住原因。
static mp_obj_t dsi_pl_state(mp_obj_t self_in) {
    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)self_in;
    mp_obj_t t[5] = {
        mp_obj_new_int(self->pl_free_fb),
        mp_obj_new_int(self->pl_pending_fb),
        mp_obj_new_int(self->pl_pending_tid),
        mp_obj_new_int(self->cur_fb),
        mp_obj_new_int(self->queue_count),
    };
    return mp_obj_new_tuple(5, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(dsi_pl_state_obj, dsi_pl_state);


static mp_obj_t dsi_deinit(mp_obj_t self_in) {
    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)self_in;
    if (s_last_dsi == self) s_last_dsi = NULL;
    dsi_deinit_hardware(self);
    // ⚠ 修復：清 ref_bufs（原先殘留釘住 buffer 引用 → GC 洩漏）
    for (int i = 0; i < self->queue_depth; i++) {
        self->ref_bufs[i] = mp_const_none;
        self->done_flags[i] = true;
        self->pending_segments[i] = 0;
        self->slot_kind[i] = 0;
    }
    self->queue_count = 0;
    // 重置 3-fb 管線狀態 (避免 deinit 後殘留 pending 阻擋)
    self->pl_free_fb = 1;
    self->pl_pending_fb = -1;
    self->pl_pending_tid = -1;
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
    { MP_ROM_QSTR(MP_QSTR_back_buffer),  MP_ROM_PTR(&dsi_back_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_present),      MP_ROM_PTR(&dsi_present_obj) },
    { MP_ROM_QSTR(MP_QSTR_blit_pipeline), MP_ROM_PTR(&dsi_blit_pipeline_obj) },
    { MP_ROM_QSTR(MP_QSTR__pl_state),     MP_ROM_PTR(&dsi_pl_state_obj) },
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
