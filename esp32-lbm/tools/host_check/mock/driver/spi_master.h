#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum { SPI1_HOST, SPI2_HOST, SPI3_HOST } spi_host_device_t;
typedef struct spi_device_t *spi_device_handle_t;
#define SPI_DMA_CH_AUTO 3
typedef struct {
    int mosi_io_num, miso_io_num, sclk_io_num, quadwp_io_num, quadhd_io_num;
    int max_transfer_sz;
} spi_bus_config_t;
typedef struct {
    int clock_speed_hz;
    uint8_t mode;
    int spics_io_num;
    int queue_size;
} spi_device_interface_config_t;
typedef struct {
    size_t length;
    const void *tx_buffer;
    void *rx_buffer;
} spi_transaction_t;
esp_err_t spi_bus_initialize(spi_host_device_t host, const spi_bus_config_t *cfg, int dma);
esp_err_t spi_bus_add_device(spi_host_device_t host, const spi_device_interface_config_t *cfg, spi_device_handle_t *h);
esp_err_t spi_device_polling_transmit(spi_device_handle_t h, spi_transaction_t *t);
