// components/services/lora_rx_service.h
#pragma once
#include "infra/lora_port.h"
#include "infra/log_repo.h"
#include "infra/rtc_clock.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

class LoraRxService {
  LoRaPort& lora_;
  LogRepo&  repo_;
  RtcClock& rtc_;
  struct Item { std::string scanner; std::string rfid; };
  QueueHandle_t queue_ = nullptr;
public:
  LoraRxService(LoRaPort& l, LogRepo& r, RtcClock& t) : lora_(l), repo_(r), rtc_(t) {}
  bool begin();
  void taskLoop();
};
