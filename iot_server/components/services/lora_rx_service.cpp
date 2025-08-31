#include "lora_rx_service.h"
#include "domain/log_entry.h"
#include <Arduino.h>
#include <SD.h>
#include "infra/sd_fs.h"
#include <cctype>   // std::isalnum

extern SdFsImpl SDfs; // provided by main.cpp

static constexpr const char* kSpoolDir = "/spool";

// --- Local validation helpers ---
static bool parseAndValidate(const std::string& in,
                             std::string& scanner,
                             std::string& rfid) {
  if (in.size() < 6 || in.size() > 64) return false;
  auto k = in.find(',');
  if (k == std::string::npos) return false;

  scanner = in.substr(0, k);
  rfid    = in.substr(k + 1);

  // scanner: [A-Za-z0-9_-]{1,32}
  if (scanner.empty() || scanner.size() > 32) return false;
  for (unsigned char ch : scanner) {
    if (!(std::isalnum(ch) || ch == '_' || ch == '-')) return false;
  }

  // rfid: 8..32 hex (normalize upper)
  if (rfid.size() < 8 || rfid.size() > 32) return false;
  for (char& ch : rfid) {
    if (ch >= 'a' && ch <= 'f') ch = char(ch - 'a' + 'A');
    if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F'))) return false;
  }
  return true;
}

bool LoraRxService::begin() {
  if (!queue_) queue_ = xQueueCreate(16, sizeof(Item*));
  if (!queue_) return false;
  if (!lora_.begin()) return false;

  lora_.onPacket([this](const std::string& p){
    std::string scanner, rfid;
    if (!parseAndValidate(p, scanner, rfid)) {
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
    lora_.pollOnce();

    // drain a few per loop
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

      // in-memory repo (debug/diagnostics pages still work)
      repo_.append(e);

      // Spool to SD: /spool/LOG.<rfid>.<scanner>
      SDfs.lock();
      do {
        if (!SDfs.isMounted()) { Serial.println("[LoRa] SD not mounted; skip spool"); break; }
        if (!SD.exists(kSpoolDir)) SD.mkdir(kSpoolDir);

        String fname = String(kSpoolDir) + "/LOG." + e.rfid.c_str() + "." + e.scanner_id.c_str();
        // avoid accidental collision: if exists, add numeric suffix .2, .3...
        if (SD.exists(fname)) {
          for (uint32_t n=2; n<1000; ++n) {
            String alt = fname + "." + String(n);
            if (!SD.exists(alt)) { fname = alt; break; }
          }
        }

        File f = SD.open(fname, FILE_WRITE);
        if (!f) { Serial.printf("[LoRa] Spool create failed: %s\n", fname.c_str()); break; }
        // store timestamp on first line for uploader
        f.println(e.ts_iso.c_str());
        f.flush(); f.close();

        Serial.printf("[LoRa] Spooled %s (ts=%s)\n", fname.c_str(), e.ts_iso.c_str());
      } while(false);
      SDfs.unlock();

      delete it;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
