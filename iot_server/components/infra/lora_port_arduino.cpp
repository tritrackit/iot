#include "lora_port.h"
#include <Arduino.h>
#include <LoRa.h>
#include "spi_lock.h"

class LoRaPortArduino : public LoRaPort {
  Handler h_; long freq_;
public:
  explicit LoRaPortArduino(long f): freq_(f) {}
  bool begin() override {
    spi_lock();
    bool ok = LoRa.begin(freq_);
    if (ok){
      // Match sender radio params
      LoRa.setSyncWord(0x42);
      LoRa.setSignalBandwidth(125E3);
      LoRa.setSpreadingFactor(7);
      LoRa.setCodingRate4(5);
      LoRa.setPreambleLength(8);   // default, explicit for clarity
      LoRa.disableCrc();           // sender does not enable CRC
      LoRa.setGain(6);             // max LNA gain for better RX sensitivity
      LoRa.receive();              // ensure continuous RX mode
      Serial.printf("[LoRaRF] init OK f=%ld BW=125k SF7 CR4/5 SW=0x42 CRC=off GAIN=6\n", freq_);
    }
    spi_unlock();
    return ok;
  }
  void onPacket(Handler h) override { h_ = std::move(h); }
  void pollOnce() override {
    spi_lock();
    int plen = LoRa.parsePacket();
    if (plen <= 0){ spi_unlock(); return; }
    // Expect a 5-byte header {net,dst,src,seq,len}, followed by len bytes of payload
    uint8_t hdr[5] = {0};
    std::string payload;
    bool hadHeader = false;
    if (plen >= 5){
      // Read 5-byte header robustly
      int got = 0;
      while (got < 5 && LoRa.available()) {
        hdr[got++] = (uint8_t)LoRa.read();
      }
      if (got < 5) {
        // Header incomplete; treat as payload-only
        while (LoRa.available()) payload.push_back((char)LoRa.read());
        spi_unlock();
        Serial.printf("[LoRaRF] RX payload='%s' (incomplete header)\n", payload.c_str());
        if (h_) h_(payload);
        return;
      }
      uint8_t payLen = hdr[4];
      int remain = plen - 5;
      int toRead = (payLen <= remain) ? payLen : remain;
      payload.reserve(toRead);
      for (int i=0;i<toRead && LoRa.available();++i) payload.push_back((char)LoRa.read());
      // Drain any trailing bytes if header len < actual packet
      while (LoRa.available()) (void)LoRa.read();
      hadHeader = true;
    } else {
      // Fallback: no header; read everything as payload
      while (LoRa.available()) payload.push_back((char)LoRa.read());
    }
    spi_unlock();
    // Debug print raw RX
    if (hadHeader){
      Serial.printf("[LoRaRF] RX net=0x%02X dst=0x%02X src=0x%02X seq=%u len=%u rssi=%d snr=%.1f payload='%s'\n",
                    hdr[0], hdr[1], hdr[2], (unsigned)hdr[3], (unsigned)hdr[4], LoRa.packetRssi(), LoRa.packetSnr(), payload.c_str());
    } else {
      Serial.printf("[LoRaRF] RX rssi=%d snr=%.1f payload='%s' (no header)\n", LoRa.packetRssi(), LoRa.packetSnr(), payload.c_str());
    }
    if (h_) h_(payload);
  }
};

LoRaPort* makeLoRaPortArduino(uint8_t ss, uint8_t rst, uint8_t dio0, SPIClass* spi, long freqHz){
  LoRa.setSPI(*spi);
  LoRa.setPins(ss, rst, dio0);
  LoRa.setSPIFrequency(2000000); // be conservative for shared bus/wiring length
  return new LoRaPortArduino(freqHz);
}
