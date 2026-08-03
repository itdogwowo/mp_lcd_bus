#ifndef _ESP32_I2C_BUS_H_
#define _ESP32_I2C_BUS_H_

#include "esp_lcd_panel_io.h"
#include "driver/i2c.h"
#include "py/obj.h"

typedef struct _mp_lcd_i2c_bus_obj_t {
    mp_obj_base_t base;

    int host;
    int sda_pin, scl_pin;
    int addr, freq;

    i2c_config_t bus_config;
    esp_lcd_i2c_bus_handle_t bus_handle;
    esp_lcd_panel_io_handle_t panel_io;

    bool initialized;
} mp_lcd_i2c_bus_obj_t;

extern const mp_obj_type_t mp_lcd_i2c_bus_type;

#endif
