#include "py/obj.h"
#include "py/runtime.h"

static mp_obj_t spi_make_new(const mp_obj_type_t *t, size_t n_args, size_t n_kw, const mp_obj_t *all) {
    mp_raise_msg(&mp_type_NotImplementedError, MP_ERROR_TEXT("LCD SPI bus is not supported on this MCU"));
}

MP_DEFINE_CONST_OBJ_TYPE(
    mp_lcd_spi_bus_type,
    MP_QSTR_SPIBus,
    MP_TYPE_FLAG_NONE,
    make_new, spi_make_new
);
