#include "modlcd_bus.h"
#include "spi_bus.h"
#include "i2c_bus.h"
#include "i80_bus.h"
#include "rgb_bus.h"

static const mp_map_elem_t mp_module_lcd_bus_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(MP_QSTR_lcd_bus) },
    { MP_ROM_QSTR(MP_QSTR_SPIBus),   (mp_obj_t)&mp_lcd_spi_bus_type },
    { MP_ROM_QSTR(MP_QSTR_I2CBus),   (mp_obj_t)&mp_lcd_i2c_bus_type },
    { MP_ROM_QSTR(MP_QSTR_I80Bus),   (mp_obj_t)&mp_lcd_i80_bus_type },
    { MP_ROM_QSTR(MP_QSTR_RGBBus),   (mp_obj_t)&mp_lcd_rgb_bus_type },
};

static MP_DEFINE_CONST_DICT(mp_module_lcd_bus_globals, mp_module_lcd_bus_globals_table);

const mp_obj_module_t mp_module_lcd_bus = {
    .base    = {&mp_type_module},
    .globals = (mp_obj_dict_t *)&mp_module_lcd_bus_globals,
};

MP_REGISTER_MODULE(MP_QSTR_lcd_bus, mp_module_lcd_bus);
