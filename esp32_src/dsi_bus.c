#include "soc/soc_caps.h"

#if SOC_MIPI_DSI_SUPPORTED

#include "dsi_bus.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_heap_caps.h"

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
    for (int i = 0; i < DSI_DMA_QUEUE_DEPTH; i++) {
        if (self->ref_bufs[i] != mp_const_none && !self->done_flags[i]) {
            self->done_flags[i] = true;
            self->ref_bufs[i] = mp_const_none;
            self->queue_head = (self->queue_head + 1) % DSI_DMA_QUEUE_DEPTH;
            self->queue_count--;
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
        ARG_in_color_format, ARG_fb_count, ARG_reset_pin, ARG_virtual_channel,
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
        { MP_QSTR_reset_pin,          MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
        { MP_QSTR_virtual_channel,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
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
    self->reset_pin = args[ARG_reset_pin].u_int;
    self->bits_per_pixel = bpp;
    self->num_fbs = nfbs;
    self->fb_size = (size_t)self->panel_w * self->panel_h * bpp / 8;

    esp_err_t ret;

    esp_lcd_dsi_bus_config_t bc = {
        .bus_id = 0,
        .num_data_lanes = (uint8_t)lanes,
        .lane_bit_rate_mbps = mp_obj_get_float(args[ARG_lane_bit_rate_mbps].u_obj),
    };
    ret = esp_lcd_new_dsi_bus(&bc, &self->bus_handle);
    if (ret != ESP_OK) goto err_bus;

    esp_lcd_dbi_io_config_t dbi = {
        .virtual_channel = (uint8_t)args[ARG_virtual_channel].u_int,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
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

    ret = esp_lcd_panel_reset(self->dpi_panel);
    if (ret != ESP_OK) goto err_panel;
    ret = esp_lcd_panel_init(self->dpi_panel);
    if (ret != ESP_OK) goto err_panel;

    memset(self->ref_bufs, 0, sizeof(self->ref_bufs));
    memset(self->done_flags, 0, sizeof(self->done_flags));
    self->queue_head = self->queue_tail = self->queue_count = 0;
    self->initialized = true;

    s_last_dsi = self;

    return MP_OBJ_FROM_PTR(self);

err_panel:
    esp_lcd_panel_del(self->dpi_panel);
err_io:
    esp_lcd_panel_io_del(self->dbi_io);
err_bus:
    esp_lcd_del_dsi_bus(self->bus_handle);
    m_del_obj(mp_lcd_dsi_bus_obj_t, self);
    mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("DSI init failed err=0x%x"), ret);
}


static mp_obj_t dsi_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_buf, ARG_x, ARG_y, ARG_w, ARG_h };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_buf,  MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_x,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_y,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_w,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_h,    MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)args[ARG_self].u_obj;
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(args[ARG_buf].u_obj, &bufinfo, MP_BUFFER_READ);

    if (self->queue_count >= DSI_DMA_QUEUE_DEPTH)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("queue full"));

    int idx = self->queue_tail;
    self->ref_bufs[idx] = args[ARG_buf].u_obj;
    self->done_flags[idx] = false;

    int x = args[ARG_x].u_int;
    int y = args[ARG_y].u_int;
    int w = args[ARG_w].u_int ? args[ARG_w].u_int : self->panel_w;
    int h = args[ARG_h].u_int ? args[ARG_h].u_int : self->panel_h;

    // draw_bitmap copies into the internal frame buffer (synchronously),
    // on_color_trans_done is fired once the copy finishes.
    esp_err_t ret = esp_lcd_panel_draw_bitmap(self->dpi_panel, x, y, x + w, y + h, bufinfo.buf);
    if (ret != ESP_OK) {
        self->ref_bufs[idx] = mp_const_none;
        mp_raise_msg_varg(&mp_type_RuntimeError, MP_ERROR_TEXT("draw_bitmap failed err=0x%x"), ret);
    }

    self->queue_tail = (self->queue_tail + 1) % DSI_DMA_QUEUE_DEPTH;
    self->queue_count++;
    return mp_obj_new_int(idx + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dsi_write_obj, 2, dsi_write);


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

    if (idx < 0 || idx >= DSI_DMA_QUEUE_DEPTH) return mp_const_false;
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
    gc_collect();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(dsi_wait_all_obj, 1, dsi_wait_all);


static mp_obj_t dsi_deinit(mp_obj_t self_in) {
    mp_lcd_dsi_bus_obj_t *self = (mp_lcd_dsi_bus_obj_t *)self_in;
    if (s_last_dsi == self) s_last_dsi = NULL;
    dsi_deinit_hardware(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(dsi_deinit_obj, dsi_deinit);


static const mp_rom_map_elem_t dsi_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write),        MP_ROM_PTR(&dsi_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_cmd),          MP_ROM_PTR(&dsi_cmd_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_busy),      MP_ROM_PTR(&dsi_is_busy_obj) },
    { MP_ROM_QSTR(MP_QSTR_pending),      MP_ROM_PTR(&dsi_pending_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait),         MP_ROM_PTR(&dsi_wait_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_all),     MP_ROM_PTR(&dsi_wait_all_obj) },
    { MP_ROM_QSTR(MP_QSTR_lane_count),   MP_ROM_PTR(&dsi_lane_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_frame_buffer), MP_ROM_PTR(&dsi_frame_buffer_obj) },
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
