#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Global SPI bus mutex for peripherals sharing VSPI
SemaphoreHandle_t spi_bus_mutex();
inline void spi_lock(){ xSemaphoreTake(spi_bus_mutex(), portMAX_DELAY); }
inline void spi_unlock(){ xSemaphoreGive(spi_bus_mutex()); }

