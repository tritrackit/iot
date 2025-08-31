#pragma once
#include <functional>
#include <string>
#include <SPI.h>

class LoRaPort {
public:
  using Handler = std::function<void(const std::string&)>;
  virtual ~LoRaPort() = default;
  virtual bool begin() = 0;
  virtual void onPacket(Handler) = 0;
  virtual void pollOnce() = 0;
};

LoRaPort* makeLoRaPortArduino(uint8_t ss, uint8_t rst, uint8_t dio0, SPIClass* spi, long freqHz);
