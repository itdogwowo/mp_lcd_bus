#include "soc/soc_caps.h"

#if SOC_LCD_I80_SUPPORTED

#include "i80_bus.h"
#include "esp_lcd_types.h"
#include "hal/lcd_types.h"
#include "esp_heap_caps.h"

#include "py/runtime.h"
#include "py/objarray.h"
#include "py/mphal.h"
#include "py/gc.h"
#include "soc/gpio_sig_map.h"
#include "esp_rom_gpio.h"
#include "driver/gpio.h"

#include <string.h>

static esp_lcd_i80_bus_handle_t  s_last_i80_bus = NULL;
static esp_lcd_panel_io_handle_t s_last_i80_panel_io = NULL;

static bool on_color_done(esp_lcd_panel_io_handle_t panel_io,
                          esp_lcd_panel_io_event_data_t *edata, void *ctx) {
    mp_lcd_i80_bus_obj_t *self = (mp_lcd_i80_bus_obj_t *)ctx;
    for (int i = 0; i < I80_DMA_QUEUE_DEPTH; i++) {
        if (self->ref_bufs[i] != mp_const_none && !self->done_flags[i]) {
            self->done_flags[i] = true;
            self->ref_bufs[i] = mp_const_none;
            self->queue_head = (self->queue_head + 1) % I80_DMA_QUEUE_DEPTH;
            self->queue_count--;
            break;
        }
    }
    return false;
}


static mp_obj_t i80_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_data, ARG_wr, ARG_dc, ARG_cs, ARG_freq };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_data, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_wr,   MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_dc,   MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_cs,   MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
        { MP_QSTR_freq, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 10000000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);

    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(args[ARG_data].u_obj, &n, &items);
    if (n != 8 && n != 16)
        mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("I80 data pins must be 8 or 16"));

    mp_lcd_i80_bus_obj_t *self = m_new_obj(mp_lcd_i80_bus_obj_t);
    self->base.type = &mp_lcd_i80_bus_type;
    self->lane_count = (int)n;
    self->wr_pin = args[ARG_wr].u_int;
    self->dc_pin = args[ARG_dc].u_int;
    self->cs_pin = args[ARG_cs].u_int;
    self->freq   = args[ARG_freq].u_int;

    for (int i = 0; i < 16; i++)
        self->data_pins[i] = (i < (int)n) ? mp_obj_get_int(items[i]) : -1;

    // 清理前一次殘留（soft reboot 安全網）
    if (s_last_i80_panel_io) { esp_lcd_panel_io_del(s_last_i80_panel_io); s_last_i80_panel_io = NULL; }
    if (s_last_i80_bus)      { esp_lcd_del_i80_bus(s_last_i80_bus);        s_last_i80_bus = NULL; }

    esp_lcd_i80_bus_config_t bcfg = {
        .dc_gpio_num = self->dc_pin,
        .wr_gpio_num = self->wr_pin,
        .clk_src     = LCD_CLK_SRC_PLL160M,
        .bus_width   = (size_t)n,
        .max_transfer_bytes = 32768,
        .psram_trans_align = 64,
        .sram_trans_align  = 64,
    };
    for (int i = 0; i < 16; i++) bcfg.data_gpio_nums[i] = self->data_pins[i];

    esp_err_t ret = esp_lcd_new_i80_bus(&bcfg, &self->bus_handle);
    if (ret != ESP_OK) {
        m_del_obj(mp_lcd_i80_bus_obj_t, self);
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("esp_lcd_new_i80_bus err=0x%x"), ret);
    }

    esp_lcd_panel_io_i80_config_t iocfg = {
        .cs_gpio_num   = self->cs_pin,
        .pclk_hz       = (uint32_t)self->freq,
        .trans_queue_depth = I80_DMA_QUEUE_DEPTH,
        .on_color_trans_done = on_color_done,
        .user_ctx      = self,
        .lcd_cmd_bits  = 8,
        .lcd_param_bits = 8,
        .dc_levels = { .dc_idle_level = 0, .dc_cmd_level = 0, .dc_dummy_level = 0, .dc_data_level = 1 },
    };

    ret = esp_lcd_new_panel_io_i80(self->bus_handle, &iocfg, &self->panel_io);
    if (ret != ESP_OK) {
        esp_lcd_del_i80_bus(self->bus_handle);
        m_del_obj(mp_lcd_i80_bus_obj_t, self);
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("esp_lcd_new_panel_io_i80 err=0x%x"), ret);
    }

    memset(self->ref_bufs, 0, sizeof(self->ref_bufs));
    memset(self->done_flags, 0, sizeof(self->done_flags));
    self->queue_head = self->queue_tail = self->queue_count = 0;
    self->initialized = true;

    s_last_i80_bus = self->bus_handle;
    s_last_i80_panel_io = self->panel_io;

    return MP_OBJ_FROM_PTR(self);
}


static mp_obj_t i80_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_buf, ARG_cmd };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_buf,  MP_ARG_OBJ | MP_ARG_KW_ONLY },
        { MP_QSTR_cmd,  MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_i80_bus_obj_t *self = (mp_lcd_i80_bus_obj_t *)args[ARG_self].u_obj;
    int cmd = args[ARG_cmd].u_int;

    if (self->queue_count >= I80_DMA_QUEUE_DEPTH)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("queue full"));

    int idx = self->queue_tail;

    if (args[ARG_buf].u_obj != MP_OBJ_NULL) {
        mp_obj_array_t *a = (mp_obj_array_t *)args[ARG_buf].u_obj;
        self->ref_bufs[idx] = args[ARG_buf].u_obj;
        self->done_flags[idx] = false;
        if (esp_lcd_panel_io_tx_color(self->panel_io, cmd, a->items, a->len) != ESP_OK) {
            self->ref_bufs[idx] = mp_const_none;
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("tx_color failed"));
        }
    } else {
        // cmd only — use tx_param (synchronous, no DMA queue needed)
        if (esp_lcd_panel_io_tx_param(self->panel_io, cmd, NULL, 0) != ESP_OK)
            mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("tx_param failed"));
        return mp_const_none;
    }

    self->queue_tail = (self->queue_tail + 1) % I80_DMA_QUEUE_DEPTH;
    self->queue_count++;
    return mp_obj_new_int(idx + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(i80_write_obj, 1, i80_write);





static mp_obj_t i80_is_busy(mp_obj_t self_in) {
    return mp_obj_new_bool(((mp_lcd_i80_bus_obj_t *)self_in)->queue_count > 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(i80_is_busy_obj, i80_is_busy);

static mp_obj_t i80_pending(mp_obj_t self_in) {
    return mp_obj_new_int(((mp_lcd_i80_bus_obj_t *)self_in)->queue_count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(i80_pending_obj, i80_pending);

static mp_obj_t i80_lane_count(mp_obj_t self_in) {
    return mp_obj_new_int(((mp_lcd_i80_bus_obj_t *)self_in)->lane_count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(i80_lane_count_obj, i80_lane_count);


static mp_obj_t i80_wait(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_trans_id, ARG_timeout_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self,       MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_trans_id,   MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_timeout_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_i80_bus_obj_t *self = (mp_lcd_i80_bus_obj_t *)args[ARG_self].u_obj;
    int tid = args[ARG_trans_id].u_int;
    int to  = args[ARG_timeout_ms].u_int;
    int idx = tid - 1;

    if (idx < 0 || idx >= I80_DMA_QUEUE_DEPTH) return mp_const_false;
    if (self->ref_bufs[idx] == mp_const_none) return mp_const_true;

    mp_uint_t deadline = mp_hal_ticks_ms() + (to < 0 ? 10000 : to);
    while (!self->done_flags[idx]) {
        if (to >= 0 && mp_hal_ticks_ms() > deadline) return mp_const_false;
        mp_hal_delay_ms(1);
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(i80_wait_obj, 2, i80_wait);


static mp_obj_t i80_wait_all(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_timeout_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self,       MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_timeout_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_i80_bus_obj_t *self = (mp_lcd_i80_bus_obj_t *)args[ARG_self].u_obj;
    int to = args[ARG_timeout_ms].u_int;
    mp_uint_t deadline = mp_hal_ticks_ms() + (to < 0 ? 10000 : to);

    while (self->queue_count > 0) {
        if (to >= 0 && mp_hal_ticks_ms() > deadline) return mp_const_false;
        mp_hal_delay_ms(1);
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(i80_wait_all_obj, 1, i80_wait_all);


static void i80_reset_gpios(mp_lcd_i80_bus_obj_t *self) {
    // 把用過的 I80 腳全部恢復成 floating input，避免 soft reboot 後殘留
    int pins[18];
    int p = 0;
    pins[p++] = self->wr_pin;
    pins[p++] = self->dc_pin;
    for (int i = 0; i < 16 && i < self->lane_count; i++) {
        if (self->data_pins[i] >= 0) pins[p++] = self->data_pins[i];
    }
    for (int i = 0; i < p; i++) {
        esp_rom_gpio_pad_select_gpio(pins[i]);
        esp_rom_gpio_connect_out_signal(pins[i], SIG_GPIO_OUT_IDX, false, false);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
    }
}


static mp_obj_t i80_deinit(mp_obj_t self_in) {
    mp_lcd_i80_bus_obj_t *self = (mp_lcd_i80_bus_obj_t *)self_in;
    if (!self->initialized) return mp_const_none;

    // 先暫存，避免 NULL 後無法比對
    esp_lcd_i80_bus_handle_t bus_h = self->bus_handle;
    esp_lcd_panel_io_handle_t io_h = self->panel_io;

    self->bus_handle = NULL;
    self->panel_io  = NULL;
    self->initialized = false;

    if (io_h) esp_lcd_panel_io_del(io_h);
    if (bus_h) esp_lcd_del_i80_bus(bus_h);

    // 清除全域 tracking
    if (s_last_i80_bus == bus_h)       s_last_i80_bus = NULL;
    if (s_last_i80_panel_io == io_h)   s_last_i80_panel_io = NULL;

    // 釋放 DMA buffer references
    for (int i = 0; i < I80_DMA_QUEUE_DEPTH; i++) {
        self->ref_bufs[i] = mp_const_none;
        self->done_flags[i] = false;
    }
    self->queue_head = self->queue_tail = self->queue_count = 0;

    // GPIO 復位 — 同 SPI 做法
    i80_reset_gpios(self);

    gc_collect();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(i80_deinit_obj, i80_deinit);


static const mp_rom_map_elem_t i80_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write),      MP_ROM_PTR(&i80_write_obj) },

    { MP_ROM_QSTR(MP_QSTR_is_busy),    MP_ROM_PTR(&i80_is_busy_obj) },
    { MP_ROM_QSTR(MP_QSTR_pending),    MP_ROM_PTR(&i80_pending_obj) },
    { MP_ROM_QSTR(MP_QSTR_lane_count), MP_ROM_PTR(&i80_lane_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait),       MP_ROM_PTR(&i80_wait_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_all),   MP_ROM_PTR(&i80_wait_all_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),     MP_ROM_PTR(&i80_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),    MP_ROM_PTR(&i80_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(i80_locals_dict, i80_locals_dict_table);


MP_DEFINE_CONST_OBJ_TYPE(
    mp_lcd_i80_bus_type,
    MP_QSTR_I80Bus,
    MP_TYPE_FLAG_NONE,
    make_new, i80_make_new,
    locals_dict, &i80_locals_dict
);

#endif /*SOC_LCD_I80_SUPPORTED*/
