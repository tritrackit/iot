#include "spi_lock.h"

static SemaphoreHandle_t g_spi_mutex = nullptr;

SemaphoreHandle_t spi_bus_mutex(){
  if (!g_spi_mutex){
    g_spi_mutex = xSemaphoreCreateMutex();
  }
  return g_spi_mutex;
}

