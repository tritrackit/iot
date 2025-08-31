// components/services/lora_rx_service.cpp
#include "lora_rx_service.h"
#include "domain/log_entry.h"
#include <Arduino.h>
#include <SD.h>
#include "infra/sd_fs.h"

extern SdFsImpl SDfs; // provided by main.cpp

bool LoraRxService::begin() {
  // Create queue to decouple RF from I2C/SD work
  if (!queue_) queue_ = xQueueCreate(16, sizeof(Item*));
  if (!queue_) return false;
  if (!lora_.begin()) return false;
  lora_.onPacket([this](const std::string& p){
    std::string scanner = "S-UNKNOWN", rfid = p;
    auto k = p.find(',');
    if (k != std::string::npos) { scanner = p.substr(0,k); rfid = p.substr(k+1); }
    Item* it = new Item{ scanner, rfid };
    if (xQueueSendToBack(queue_, &it, 0) != pdPASS){
      delete it;
      Serial.println("[LoRa] queue full; dropping packet");
    }
  });
  return true;
}

void LoraRxService::taskLoop() {
  for(;;) {
    // Poll radio quickly
    lora_.pollOnce();
    // Process a few queued items per loop to avoid starving other tasks
    for (int i=0;i<4;i++){
      Item* it = nullptr;
      if (xQueueReceive(queue_, &it, 0) != pdPASS) break;
      if (!it) continue;
      domain::LogEntry e;
      e.scanner_id = it->scanner;
      e.rfid       = it->rfid;
      e.ts_iso     = rtc_.nowIso();
      e.sent       = false;
      e.message.clear();
      Serial.printf("[LoRa] RX scanner=%s rfid=%s ts=%s\n", e.scanner_id.c_str(), e.rfid.c_str(), e.ts_iso.c_str());
      repo_.append(e);
      SDfs.lock();
      do {
        if (!SDfs.isMounted()) { Serial.println("[LoRa] SD not mounted; skipping SD write"); break; }
        const char* LOGS = "/sd/logs.csv";
        // Use raw SD.* APIs under coarse lock to avoid double-lock deadlocks
        if (!SD.exists(LOGS)){
          File nf = SD.open(LOGS, "w");
          if (!nf) { Serial.println("[LoRa] Failed to create /sd/logs.csv"); break; }
          nf.printf("S-BOOT,INIT,%s,0,\n", e.ts_iso.c_str());
          nf.flush(); nf.close();
        }
        File f = SD.open(LOGS, "a");
        if (!f) { Serial.println("[LoRa] Failed to open /sd/logs.csv for append"); break; }
        int n = f.printf("%s,%s,%s,%d,%s\n",
                         e.scanner_id.c_str(), e.rfid.c_str(), e.ts_iso.c_str(), 0, "");
        f.flush(); f.close();
        if (n <= 0) Serial.println("[LoRa] Append to /sd/logs.csv wrote 0 bytes");
        else Serial.printf("[LoRa] Saved to SD: %s,%s,%s\n", e.scanner_id.c_str(), e.rfid.c_str(), e.ts_iso.c_str());
      } while(false);
      SDfs.unlock();
      delete it;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
