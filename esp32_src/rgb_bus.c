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

#include <stdio.h>
#include <string.h>

static esp_lcd_panel_handle_t s_last_rgb_panel = NULL;

static bool on_vsync(esp_lcd_panel_handle_t panel,
                     const esp_lcd_rgb_panel_event_data_t *edata, void *ctx) {
    mp_lcd_rgb_bus_obj_t *self = (mp_lcd_rgb_bus_obj_t *)ctx;
    for (int i = 0; i < RGB_DMA_QUEUE_DEPTH; i++) {
        if (self->ref_bufs[i] != mp_const_none && !self->done_flags[i]) {
            self->done_flags[i] = true;
            self->ref_bufs[i] = mp_const_none;
            self->queue_head = (self->queue_head + 1) % RGB_DMA_QUEUE_DEPTH;
            self->queue_count--;
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
        { MP_QSTR_refresh_on_demand, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_bb_size_px,        MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);

    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(args[ARG_data].u_obj, &n, &items);
    if (n != 8 && n != 16)
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("RGB data pins must be 8 or 16"));

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
        char msg[64];
        snprintf(msg, sizeof(msg), "esp_lcd_new_rgb_panel err=0x%x", ret);
        m_del_obj(mp_lcd_rgb_bus_obj_t, self);
        mp_raise_msg(&mp_type_RuntimeError, msg);
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
    self->initialized = true;

    s_last_rgb_panel = self->panel_handle;

    return MP_OBJ_FROM_PTR(self);
}


static mp_obj_t rgb_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
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

    mp_lcd_rgb_bus_obj_t *self = (mp_lcd_rgb_bus_obj_t *)args[ARG_self].u_obj;
    mp_obj_array_t *a = (mp_obj_array_t *)args[ARG_buf].u_obj;

    if (self->queue_count >= RGB_DMA_QUEUE_DEPTH)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("queue full"));

    int idx = self->queue_tail;
    self->ref_bufs[idx] = args[ARG_buf].u_obj;
    self->done_flags[idx] = false;

    int x = args[ARG_x].u_int;
    int y = args[ARG_y].u_int;
    int w = args[ARG_w].u_int ? args[ARG_w].u_int : self->panel_w;
    int h = args[ARG_h].u_int ? args[ARG_h].u_int : self->panel_h;

    if (esp_lcd_panel_draw_bitmap(self->panel_handle, x, y, x + w, y + h, a->items) != ESP_OK) {
        self->ref_bufs[idx] = mp_const_none;
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("draw_bitmap failed"));
    }

    self->queue_tail = (self->queue_tail + 1) % RGB_DMA_QUEUE_DEPTH;
    self->queue_count++;
    return mp_obj_new_int(idx + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(rgb_write_obj, 2, rgb_write);


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

    if (idx < 0 || idx >= RGB_DMA_QUEUE_DEPTH) return mp_const_false;
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
    gc_collect();
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
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(rgb_deinit_obj, rgb_deinit);


static const mp_rom_map_elem_t rgb_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write),      MP_ROM_PTR(&rgb_write_obj) },
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
