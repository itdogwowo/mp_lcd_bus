#ifndef _ESP32_SPI_BUS_H_
#define _ESP32_SPI_BUS_H_

#include "driver/spi_master.h"
#include "py/obj.h"

#define SPI_MAX_QUEUE_DEPTH    8
#define SPI_DEFAULT_QUEUE_DEPTH 8

typedef struct _mp_lcd_spi_bus_obj_t {
    mp_obj_base_t base;

    spi_device_handle_t handle;
    spi_transaction_t trans[SPI_MAX_QUEUE_DEPTH];
    mp_obj_t ref_bufs[SPI_MAX_QUEUE_DEPTH];
    int queue_head;
    int queue_tail;
    int queue_count;
    int queue_depth;

    int lane_count;
    int data_pins[8];
    int clk_pin;
    int freq;
    int host;

    uint8_t *zero_buf;

    bool initialized;
} mp_lcd_spi_bus_obj_t;

extern const mp_obj_type_t mp_lcd_spi_bus_type;

#endif
