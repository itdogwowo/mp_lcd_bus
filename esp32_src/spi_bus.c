#include "spi_bus.h"

#include "py/runtime.h"
#include "py/objarray.h"
#include "py/mphal.h"
#include "py/gc.h"
#include "esp_heap_caps.h"
#include "soc/soc_caps.h"
#include "soc/gpio_sig_map.h"
#include "esp_rom_gpio.h"
#include "driver/gpio.h"

#include <string.h>

static spi_device_handle_t  s_spi_device[SOC_SPI_PERIPH_NUM];

static uint32_t lane_flag(int n) {
    switch (n) {
        case 2:  return SPI_TRANS_MODE_DIO;
        case 4:  return SPI_TRANS_MODE_QIO;
        case 8:  return SPI_TRANS_MODE_OCT;
        default: return 0;
    }
}

static int enqueue(mp_lcd_spi_bus_obj_t *self, const mp_obj_t buf, void *rx) {
    int idx = self->queue_tail;
    mp_obj_array_t *a = (mp_obj_array_t *)buf;

    memset(&self->trans[idx], 0, sizeof(spi_transaction_t));
    self->trans[idx].length    = a->len * 8;
    self->trans[idx].tx_buffer = a->items;
    self->trans[idx].rx_buffer = rx;
    self->trans[idx].flags     = lane_flag(self->lane_count);
    self->trans[idx].user      = (void *)(uintptr_t)(idx + 1);

    self->ref_bufs[idx] = buf;

    esp_err_t ret = spi_device_queue_trans(self->handle, &self->trans[idx], 0);
    if (ret != ESP_OK) {
        self->ref_bufs[idx] = mp_const_none;
        return -((int)ret);
    }

    self->queue_tail = (self->queue_tail + 1) % SPI_DMA_QUEUE_DEPTH;
    self->queue_count++;
    return idx + 1;
}

static void spi_drain_pending(mp_lcd_spi_bus_obj_t *self) {
    while (self->queue_count > 0) {
        spi_transaction_t *rt;
        if (spi_device_get_trans_result(self->handle, &rt, 0) != ESP_OK) break;
        int done = (int)(uintptr_t)rt->user - 1;
        if (done >= 0 && done < SPI_DMA_QUEUE_DEPTH) {
            self->ref_bufs[done] = mp_const_none;
            self->queue_count--;
        }
    }
}

static void spi_deinit_hardware(mp_lcd_spi_bus_obj_t *self) {
    if (!self->initialized) return;
    spi_bus_remove_device(self->handle);
    spi_bus_free(self->host);
    if (self->host < SOC_SPI_PERIPH_NUM) s_spi_device[self->host] = NULL;

    int8_t pins[9];
    int p = 0;
    pins[p++] = self->clk_pin;
    for (int i = 0; i < self->lane_count; i++) {
        if (self->data_pins[i] != -1) pins[p++] = self->data_pins[i];
    }
    for (int i = 0; i < p; i++) {
        esp_rom_gpio_pad_select_gpio(pins[i]);
        esp_rom_gpio_connect_out_signal(pins[i], SIG_GPIO_OUT_IDX, false, false);
        gpio_set_direction(pins[i], GPIO_MODE_INPUT);
    }

    if (self->zero_buf) { heap_caps_free(self->zero_buf); self->zero_buf = NULL; }
    self->initialized = false;
}

static mp_obj_t spi_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *all_args) {
    enum { ARG_data, ARG_clk, ARG_freq, ARG_host, ARG_sck, ARG_mosi, ARG_miso };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_data, MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
        { MP_QSTR_clk,  MP_ARG_INT, {.u_int = -1} },
        { MP_QSTR_freq, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 40000000} },
        { MP_QSTR_host, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 1} },
        { MP_QSTR_sck,  MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
        { MP_QSTR_mosi, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
        { MP_QSTR_miso, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all_kw_array(n_args, n_kw, all_args, MP_ARRAY_SIZE(allowed), allowed, args);

    bool use_data_style = (args[ARG_data].u_obj != MP_OBJ_NULL);

    size_t n;
    mp_obj_t *items;
    int clk_pin;

    if (use_data_style) {
        mp_obj_get_array(args[ARG_data].u_obj, &n, &items);
        if (n < 1 || n > 8) {
            mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("data pin count must be 1-8"));
        }
        clk_pin = args[ARG_clk].u_int;
        if (clk_pin == -1) {
            mp_raise_msg(&mp_type_ValueError, MP_ERROR_TEXT("clk required with data="));
        }
    } else {
        int sck  = args[ARG_sck].u_int;
        int mosi = args[ARG_mosi].u_int;
        if (sck == -1 || mosi == -1) {
            mp_raise_msg(&mp_type_ValueError,
                MP_ERROR_TEXT("use data+clk or sck+mosi"));
        }
        n = 1;
        items = NULL;
        clk_pin = sck;
    }

    int host = args[ARG_host].u_int;
    if (host < 1 || host >= SOC_SPI_PERIPH_NUM) {
        mp_raise_msg_varg(&mp_type_ValueError,
            MP_ERROR_TEXT("host=%d not available (valid: 1..%d)"), host, SOC_SPI_PERIPH_NUM - 1);
    }

    if (s_spi_device[host]) {
        spi_bus_remove_device(s_spi_device[host]);
        s_spi_device[host] = NULL;
    }
    spi_bus_free(host);

    mp_lcd_spi_bus_obj_t *self = m_new_obj(mp_lcd_spi_bus_obj_t);
    self->base.type = &mp_lcd_spi_bus_type;

    self->lane_count = (int)n;
    self->clk_pin = clk_pin;
    self->freq    = args[ARG_freq].u_int;
    self->host    = host;

    int miso_pin = -1;
    if (use_data_style) {
        for (int i = 0; i < 8; i++)
            self->data_pins[i] = (i < (int)n) ? mp_obj_get_int(items[i]) : -1;
    } else {
        self->data_pins[0] = args[ARG_mosi].u_int;
        miso_pin = args[ARG_miso].u_int;
        self->data_pins[1] = miso_pin;
        for (int i = 2; i < 8; i++) self->data_pins[i] = -1;
    }

    self->zero_buf = (uint8_t *)heap_caps_calloc(1, 32768, MALLOC_CAP_DMA);
    if (!self->zero_buf) {
        m_del_obj(mp_lcd_spi_bus_obj_t, self);
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("DMA zero buffer alloc failed"));
    }

    spi_bus_config_t bcfg = {
        .mosi_io_num   = self->data_pins[0],
        .miso_io_num   = self->data_pins[1],
        .sclk_io_num   = self->clk_pin,
        .quadwp_io_num = self->data_pins[2],
        .quadhd_io_num = self->data_pins[3],
        .data4_io_num  = self->data_pins[4],
        .data5_io_num  = self->data_pins[5],
        .data6_io_num  = self->data_pins[6],
        .data7_io_num  = self->data_pins[7],
        .max_transfer_sz = 32768,
        .flags          = SPICOMMON_BUSFLAG_MASTER,
    };

    uint32_t dflags = 0;
    if (self->lane_count >= 2) dflags |= SPI_DEVICE_HALFDUPLEX;

    spi_device_interface_config_t dcfg = {
        .clock_speed_hz = (uint32_t)self->freq,
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = SPI_DMA_QUEUE_DEPTH,
        .flags = dflags,
        .command_bits = (self->lane_count >= 2) ? 8 : 0,
        .address_bits = (self->lane_count >= 2) ? 24 : 0,
    };

    esp_err_t ret = spi_bus_initialize(self->host, &bcfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        heap_caps_free(self->zero_buf);
        m_del_obj(mp_lcd_spi_bus_obj_t, self);
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("spi_bus_initialize err=0x%x"), ret);
    }
    ret = spi_bus_add_device(self->host, &dcfg, &self->handle);
    if (ret != ESP_OK) {
        heap_caps_free(self->zero_buf);
        spi_bus_free(self->host);
        m_del_obj(mp_lcd_spi_bus_obj_t, self);
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("spi_bus_add_device err=0x%x"), ret);
    }

    memset(self->trans, 0, sizeof(self->trans));
    memset(self->ref_bufs, 0, sizeof(self->ref_bufs));
    self->queue_head = self->queue_tail = self->queue_count = 0;
    self->initialized = true;

    s_spi_device[host] = self->handle;

    return MP_OBJ_FROM_PTR(self);
}


static mp_obj_t spi_write(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_buf, ARG_cmd, ARG_addr, ARG_multiline };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self,      MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_buf,       MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_cmd,       MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
        { MP_QSTR_addr,      MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
        { MP_QSTR_multiline, MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_spi_bus_obj_t *self = (mp_lcd_spi_bus_obj_t *)args[ARG_self].u_obj;

    if (args[ARG_cmd].u_int >= 0) {
        spi_drain_pending(self);

        spi_transaction_t t;
        memset(&t, 0, sizeof(t));
        if (args[ARG_multiline].u_bool) {
            t.flags |= SPI_TRANS_MULTILINE_CMD | SPI_TRANS_MULTILINE_ADDR;
        }
        t.cmd  = (uint16_t)args[ARG_cmd].u_int;
        t.addr = (uint32_t)args[ARG_addr].u_int;

        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(args[ARG_buf].u_obj, &bufinfo, MP_BUFFER_READ);
        t.tx_buffer = bufinfo.buf;
        t.length    = bufinfo.len * 8;

        spi_device_polling_transmit(self->handle, &t);
        return mp_const_none;
    }

    if (self->queue_count >= SPI_DMA_QUEUE_DEPTH)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("queue full"));

    int tid = enqueue(self, args[ARG_buf].u_obj, NULL);
    if (tid < 0) {
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("queue failed err=0x%x"), -tid);
    }
    return mp_obj_new_int(tid);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(spi_write_obj, 2, spi_write);


static mp_obj_t spi_readinto(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_buf, ARG_write_val };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self,      MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_buf,       MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_write_val, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = 0} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_spi_bus_obj_t *self = (mp_lcd_spi_bus_obj_t *)args[ARG_self].u_obj;
    if (self->queue_count >= SPI_DMA_QUEUE_DEPTH)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("queue full"));

    mp_obj_array_t *rx = (mp_obj_array_t *)args[ARG_buf].u_obj;
    uint8_t val = (uint8_t)args[ARG_write_val].u_int;
    if (val != 0) memset(self->zero_buf, val, rx->len);

    int idx = self->queue_tail;
    memset(&self->trans[idx], 0, sizeof(spi_transaction_t));
    self->trans[idx].length    = rx->len * 8;
    self->trans[idx].tx_buffer = self->zero_buf;
    self->trans[idx].rx_buffer = rx->items;
    self->trans[idx].flags     = lane_flag(self->lane_count);
    self->trans[idx].user      = (void *)(uintptr_t)(idx + 1);
    self->ref_bufs[idx] = args[ARG_buf].u_obj;

    esp_err_t ret = spi_device_queue_trans(self->handle, &self->trans[idx], 0);
    if (ret != ESP_OK) {
        self->ref_bufs[idx] = mp_const_none;
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("readinto queue failed err=0x%x"), ret);
    }
    self->queue_tail = (self->queue_tail + 1) % SPI_DMA_QUEUE_DEPTH;
    self->queue_count++;
    return mp_obj_new_int(idx + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(spi_readinto_obj, 2, spi_readinto);


static mp_obj_t spi_write_readinto(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_wbuf, ARG_rbuf };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_wbuf, MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_rbuf, MP_ARG_OBJ | MP_ARG_REQUIRED },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_spi_bus_obj_t *self = (mp_lcd_spi_bus_obj_t *)args[ARG_self].u_obj;
    if (self->queue_count >= SPI_DMA_QUEUE_DEPTH)
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("queue full"));

    mp_obj_array_t *w = (mp_obj_array_t *)args[ARG_wbuf].u_obj;
    mp_obj_array_t *r = (mp_obj_array_t *)args[ARG_rbuf].u_obj;
    size_t len = w->len; if (r->len < len) len = r->len;

    int idx = self->queue_tail;
    memset(&self->trans[idx], 0, sizeof(spi_transaction_t));
    self->trans[idx].length    = len * 8;
    self->trans[idx].tx_buffer = w->items;
    self->trans[idx].rx_buffer = r->items;
    self->trans[idx].flags     = lane_flag(self->lane_count);
    self->trans[idx].user      = (void *)(uintptr_t)(idx + 1);
    self->ref_bufs[idx] = args[ARG_rbuf].u_obj;

    esp_err_t ret = spi_device_queue_trans(self->handle, &self->trans[idx], 0);
    if (ret != ESP_OK) {
        self->ref_bufs[idx] = mp_const_none;
        mp_raise_msg_varg(&mp_type_RuntimeError,
            MP_ERROR_TEXT("write_readinto queue failed err=0x%x"), ret);
    }
    self->queue_tail = (self->queue_tail + 1) % SPI_DMA_QUEUE_DEPTH;
    self->queue_count++;
    return mp_obj_new_int(idx + 1);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(spi_write_readinto_obj, 3, spi_write_readinto);


static mp_obj_t spi_is_busy(mp_obj_t self_in) {
    mp_lcd_spi_bus_obj_t *self = (mp_lcd_spi_bus_obj_t *)self_in;
    return mp_obj_new_bool(self->queue_count > 0);
}
static MP_DEFINE_CONST_FUN_OBJ_1(spi_is_busy_obj, spi_is_busy);

static mp_obj_t spi_pending(mp_obj_t self_in) {
    mp_lcd_spi_bus_obj_t *self = (mp_lcd_spi_bus_obj_t *)self_in;
    return mp_obj_new_int(self->queue_count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(spi_pending_obj, spi_pending);

static mp_obj_t spi_lane_count(mp_obj_t self_in) {
    return mp_obj_new_int(((mp_lcd_spi_bus_obj_t *)self_in)->lane_count);
}
static MP_DEFINE_CONST_FUN_OBJ_1(spi_lane_count_obj, spi_lane_count);


static mp_obj_t spi_wait(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_trans_id, ARG_timeout_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self,       MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_trans_id,   MP_ARG_INT | MP_ARG_REQUIRED },
        { MP_QSTR_timeout_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_spi_bus_obj_t *self = (mp_lcd_spi_bus_obj_t *)args[ARG_self].u_obj;
    int tid = args[ARG_trans_id].u_int;
    int to = args[ARG_timeout_ms].u_int;
    int idx = tid - 1;
    if (idx < 0 || idx >= SPI_DMA_QUEUE_DEPTH) return mp_const_false;
    if (self->ref_bufs[idx] == mp_const_none) return mp_const_true;

    TickType_t tt = (to < 0) ? portMAX_DELAY : pdMS_TO_TICKS(to);
    while (self->ref_bufs[idx] != mp_const_none) {
        spi_transaction_t *rt;
        if (spi_device_get_trans_result(self->handle, &rt, tt) != ESP_OK)
            return mp_const_false;
        int done = (int)(uintptr_t)rt->user - 1;
        if (done >= 0 && done < SPI_DMA_QUEUE_DEPTH) {
            self->ref_bufs[done] = mp_const_none;
            self->queue_count--;
        }
    }
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(spi_wait_obj, 2, spi_wait);


static mp_obj_t spi_wait_all(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_self, ARG_timeout_ms };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_self,       MP_ARG_OBJ | MP_ARG_REQUIRED },
        { MP_QSTR_timeout_ms, MP_ARG_INT | MP_ARG_KW_ONLY, {.u_int = -1} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed), allowed, args);

    mp_lcd_spi_bus_obj_t *self = (mp_lcd_spi_bus_obj_t *)args[ARG_self].u_obj;
    int to = args[ARG_timeout_ms].u_int;

    while (self->queue_count > 0) {
        TickType_t tt = (to < 0) ? portMAX_DELAY : (to > 0) ? pdMS_TO_TICKS(to) : 0;
        spi_transaction_t *rt;
        if (spi_device_get_trans_result(self->handle, &rt, tt) != ESP_OK) break;
        int done = (int)(uintptr_t)rt->user - 1;
        if (done >= 0 && done < SPI_DMA_QUEUE_DEPTH) {
            self->ref_bufs[done] = mp_const_none;
            self->queue_count--;
        }
    }
    gc_collect();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(spi_wait_all_obj, 1, spi_wait_all);


static mp_obj_t spi_deinit(mp_obj_t self_in) {
    mp_lcd_spi_bus_obj_t *self = (mp_lcd_spi_bus_obj_t *)self_in;
    spi_drain_pending(self);
    spi_deinit_hardware(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(spi_deinit_obj, spi_deinit);


static const mp_rom_map_elem_t spi_locals_table[] = {
    { MP_ROM_QSTR(MP_QSTR_write),          MP_ROM_PTR(&spi_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto),       MP_ROM_PTR(&spi_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_write_readinto), MP_ROM_PTR(&spi_write_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_is_busy),        MP_ROM_PTR(&spi_is_busy_obj) },
    { MP_ROM_QSTR(MP_QSTR_pending),        MP_ROM_PTR(&spi_pending_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait),           MP_ROM_PTR(&spi_wait_obj) },
    { MP_ROM_QSTR(MP_QSTR_wait_all),       MP_ROM_PTR(&spi_wait_all_obj) },
    { MP_ROM_QSTR(MP_QSTR_lane_count),     MP_ROM_PTR(&spi_lane_count_obj) },
    { MP_ROM_QSTR(MP_QSTR_deinit),         MP_ROM_PTR(&spi_deinit_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),        MP_ROM_PTR(&spi_deinit_obj) },
};
static MP_DEFINE_CONST_DICT(spi_locals_dict, spi_locals_table);

MP_DEFINE_CONST_OBJ_TYPE(
    mp_lcd_spi_bus_type,
    MP_QSTR_SPIBus,
    MP_TYPE_FLAG_NONE,
    make_new, spi_make_new,
    locals_dict, (mp_obj_dict_t *)&spi_locals_dict
);
