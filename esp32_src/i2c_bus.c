#include "i2c_bus.h"
#include "py/runtime.h"
#include "py/objarray.h"
#include "soc/gpio_sig_map.h"
#include "esp_rom_gpio.h"
#include "driver/gpio.h"

static esp_lcd_panel_io_handle_t s_i2c_panel_io;

static mp_obj_t i2c_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_sda, ARG_scl, ARG_addr, ARG_freq };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_sda,  MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_scl,  MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_addr, MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_freq, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 400000} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_i2c_bus_obj_t *self = m_new_obj(mp_lcd_i2c_bus_obj_t);
    self->base.type = &mp_lcd_i2c_bus_type;

    self->sda_pin = args[ARG_sda].u_int;
    self->scl_pin = args[ARG_scl].u_int;
    self->addr    = args[ARG_addr].u_int;
    self->freq    = args[ARG_freq].u_int;
    self->host    = 0;

    self->bus_config.mode = I2C_MODE_MASTER;
    self->bus_config.sda_io_num = self->sda_pin;
    self->bus_config.scl_io_num = self->scl_pin;
    self->bus_config.sda_pullup_en = true;
    self->bus_config.scl_pullup_en = true;
    self->bus_config.master.clk_speed = (uint32_t)self->freq;
    self->bus_config.clk_flags = I2C_SCLK_SRC_FLAG_FOR_NOMAL;
    self->bus_handle = (esp_lcd_i2c_bus_handle_t)((uint32_t)self->host);

    if (s_i2c_panel_io) { esp_lcd_panel_io_del(s_i2c_panel_io); s_i2c_panel_io = NULL; }
    i2c_driver_delete(self->host);
    if (i2c_param_config(self->host, &self->bus_config) != ESP_OK) {
        m_del_obj(mp_lcd_i2c_bus_obj_t, self);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("i2c_param_config"));
    }
    if (i2c_driver_install(self->host, I2C_MODE_MASTER, 0, 0, 0) != ESP_OK) {
        m_del_obj(mp_lcd_i2c_bus_obj_t, self);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("i2c_driver_install"));
    }

    esp_lcd_panel_io_i2c_config_t iocfg = {
        .dev_addr = (uint32_t)self->addr,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .flags = { .dc_low_on_data = false, .disable_control_phase = false },
    };

    if (esp_lcd_new_panel_io_i2c(self->bus_handle, &iocfg, &self->panel_io) != ESP_OK) {
        i2c_driver_delete(self->host);
        m_del_obj(mp_lcd_i2c_bus_obj_t, self);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("esp_lcd_new_panel_io_i2c"));
    }

    s_i2c_panel_io = self->panel_io;
    self->initialized = true;
    return MP_OBJ_FROM_PTR(self);
}


static mp_obj_t i2c_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_buf };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_buf,  MP_ARG_OBJ | MP_ARG_REQUIRED },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_i2c_bus_obj_t *self = (mp_lcd_i2c_bus_obj_t *)args[ARG_self].u_obj;
    mp_obj_array_t *a = (mp_obj_array_t *)args[ARG_buf].u_obj;

    if (esp_lcd_panel_io_tx_color(self->panel_io, -1, a->items, a->len) != ESP_OK)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("i2c write failed"));
    return mp_obj_new_int(0);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(i2c_write_obj, 2, i2c_write);


static mp_obj_t i2c_readinto(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_buf, ARG_cmd };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_buf,  MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_cmd,  MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_i2c_bus_obj_t *self = (mp_lcd_i2c_bus_obj_t *)args[ARG_self].u_obj;
    mp_obj_array_t *a = (mp_obj_array_t *)args[ARG_buf].u_obj;

    if (esp_lcd_panel_io_rx_param(self->panel_io, (int)args[ARG_cmd].u_int, a->items, a->len) != ESP_OK)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("i2c read failed"));
    return mp_obj_new_int(0);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(i2c_readinto_obj, 2, i2c_readinto);


static mp_obj_t i2c_is_busy(mp_obj_t self_in) { return mp_const_false; }
static MP_DEFINE_CONST_FUN_OBJ_1(i2c_is_busy_obj, i2c_is_busy);

static mp_obj_t i2c_pending(mp_obj_t self_in) { return MP_OBJ_NEW_SMALL_INT(0); }
static MP_DEFINE_CONST_FUN_OBJ_1(i2c_pending_obj, i2c_pending);

static mp_obj_t i2c_lane_count(mp_obj_t self_in) { return MP_OBJ_NEW_SMALL_INT(1); }
static MP_DEFINE_CONST_FUN_OBJ_1(i2c_lane_count_obj, i2c_lane_count);

static mp_obj_t i2c_wait(size_t n_args, const mp_obj_t *pos_args) {
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(i2c_wait_obj, 1, 3, i2c_wait);

static mp_obj_t i2c_wait_all(size_t n_args, const mp_obj_t *pos_args) {
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(i2c_wait_all_obj, 0, 2, i2c_wait_all);


static mp_obj_t i2c_deinit(mp_obj_t self_in) {
    mp_lcd_i2c_bus_obj_t *self = (mp_lcd_i2c_bus_obj_t *)self_in;
    if (!self->initialized) return mp_const_none;
    if (s_i2c_panel_io == self->panel_io) s_i2c_panel_io = NULL;
    esp_lcd_panel_io_del(self->panel_io);
    i2c_driver_delete(self->host);

    int8_t pins[2] = {self->sda_pin, self->scl_pin};
    for (int i = 0; i < 2; i++) {
        esp_rom_gpio_pad_select_gpio(pins[i]);
        esp_rom_gpio_connect_out_signal(pins[i], SIG_GPIO_OUT_IDX, false, false);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
    }

    self->initialized = false;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(i2c_deinit_obj, i2c_deinit);


static const mp_rom_map_elem_t i2c_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write),      MP_ROM_PTR(&i2c_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto),   MP_ROM_PTR(&i2c_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_busy),    MP_ROM_PTR(&i2c_is_busy_obj) },
    { MP_ROM_QSTR(MP_QSTR_pending),    MP_ROM_PTR(&i2c_pending_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait),       MP_ROM_PTR(&i2c_wait_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_all),   MP_ROM_PTR(&i2c_wait_all_obj) },
    { MP_ROM_QSTR(MP_QSTR_lane_count), MP_ROM_PTR(&i2c_lane_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),     MP_ROM_PTR(&i2c_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),    MP_ROM_PTR(&i2c_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(i2c_locals_dict, i2c_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    mp_lcd_i2c_bus_type,
    MP_QSTR_I2CBus,
    MP_TYPE_FLAG_NONE,
    make_new, i2c_make_new,
    locals_dict, (mp_obj_dict_t *)&i2c_locals_dict
);
