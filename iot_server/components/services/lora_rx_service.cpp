// components/services/lora_rx_service.cpp
#include "lora_rx_service.h"
#include "domain/log_entry.h"
#include <Arduino.h>
#include <SD.h>
#include "infra/sd_fs.h"
#include <cctype>   // std::isalnum

extern SdFsImpl SDfs; // provided by main.cpp

static constexpr const char* kLogsPath_SD = "/logs.csv"; // <-- SD.* expects root-relative

// --- Local validation helpers ---
static bool parseAndValidate(const std::string& in,
                             std::string& scanner,
                             std::string& rfid) {
  // quick overall length guard
  if (in.size() < 6 || in.size() > 64) return false;

  // split "scanner,rfid"
  auto k = in.find(',');
  if (k == std::string::npos) return false;

  scanner = in.substr(0, k);
  rfid    = in.substr(k + 1);

  // scanner: [A-Za-z0-9_-]{1,32}
  if (scanner.empty() || scanner.size() > 32) return false;
  for (unsigned char ch : scanner) {
    if (!(std::isalnum(ch) || ch == '_' || ch == '-')) return false;
  }

  // RFID: 8..32 hex chars; accept lowercase and normalize to uppercase
  if (rfid.size() < 8 || rfid.size() > 32) return false;
  for (char& ch : rfid) {
    if (ch >= 'a' && ch <= 'f') ch = char(ch - 'a' + 'A');
    if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F'))) return false;
  }
  return true;
}

bool LoraRxService::begin() {
  // Create queue to decouple RF from I2C/SD work
  if (!queue_) queue_ = xQueueCreate(16, sizeof(Item*));
  if (!queue_) return false;
  if (!lora_.begin()) return false;

  lora_.onPacket([this](const std::string& p){
    std::string scanner, rfid;
    if (!parseAndValidate(p, scanner, rfid)) {
      // Drop noise / malformed frames to avoid S-UNKNOWN pollution
      Serial.printf("[LoRa] Ignored invalid payload '%s'\n", p.c_str());
      return;
    }
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
    for (int i = 0; i < 4; i++) {
      Item* it = nullptr;
      if (xQueueReceive(queue_, &it, 0) != pdPASS) break;
      if (!it) continue;

      domain::LogEntry e;
      e.scanner_id = it->scanner;
      e.rfid       = it->rfid;
      e.ts_iso     = rtc_.nowIso();
      e.sent       = false;
      e.message.clear();

      Serial.printf("[LoRa] RX scanner=%s rfid=%s ts=%s\n",
        e.scanner_id.c_str(), e.rfid.c_str(), e.ts_iso.c_str());

      // Append to in-memory repo
      repo_.append(e);

      // Append to SD CSV
      SDfs.lock();
      do {
        if (!SDfs.isMounted()) { Serial.println("[LoRa] SD not mounted; skipping SD write"); break; }

        // Create file with header once
        if (!SD.exists(kLogsPath_SD)) {
          File nf = SD.open(kLogsPath_SD, FILE_WRITE);
          if (!nf) { Serial.printf("[LoRa] Failed to create %s\n", kLogsPath_SD); break; }
          nf.println("scanner,rfid,timestamp,code,message");
          nf.close();
        }

        File f = SD.open(kLogsPath_SD, FILE_APPEND);
        if (!f) { Serial.printf("[LoRa] Failed to open %s for append\n", kLogsPath_SD); break; }

        int n = f.printf("%s,%s,%s,%d,%s\n",
                         e.scanner_id.c_str(),
                         e.rfid.c_str(),
                         e.ts_iso.c_str(),
                         0,
                         "");
        f.flush(); f.close();

        if (n <= 0) Serial.printf("[LoRa] Append wrote 0 bytes to %s\n", kLogsPath_SD);
        else Serial.printf("[LoRa] Saved to SD: %s,%s,%s\n",
                           e.scanner_id.c_str(), e.rfid.c_str(), e.ts_iso.c_str());
      } while (false);
      SDfs.unlock();

      delete it;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
