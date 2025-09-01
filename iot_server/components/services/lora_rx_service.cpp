#include "lora_rx_service.h"
#include "domain/log_entry.h"
#include <Arduino.h>
#include <SD.h>
#include "infra/sd_fs.h"
#include <cctype>
#include <time.h>

extern SdFsImpl SDfs; // provided by main.cpp
static constexpr const char* kSpoolDir = "/spool";

// --- payload validation ---
static bool parseAndValidate(const std::string& in, std::string& scanner, std::string& rfid) {
  if (in.size() < 6 || in.size() > 64) return false;
  auto k = in.find(',');
  if (k == std::string::npos) return false;

  scanner = in.substr(0, k);
  rfid    = in.substr(k + 1);

  if (scanner.empty() || scanner.size() > 32) return false;
  for (unsigned char ch : scanner) if (!(std::isalnum(ch) || ch == '_' || ch == '-')) return false;

  if (rfid.size() < 8 || rfid.size() > 32) return false;
  for (char& ch : rfid) {
    if (ch >= 'a' && ch <= 'f') ch = char(ch - 'a' + 'A');
    if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F'))) return false;
  }
  return true;
}

// --- Make ISO + 14-digit timestamp from best clock (RTC > SNTP > millis) ---
static void makeTimestamps(RtcClock& rtc, String& iso, String& ts14, const char** src){
  // 1) RTC
  std::string i = rtc.nowIso();
  if (i.size() == 19 && i.rfind("1970-01-01", 0) != 0) {
    iso = i.c_str();
    ts14 = iso.substring(0,4) + iso.substring(5,7) + iso.substring(8,10)
         + iso.substring(11,13) + iso.substring(14,16) + iso.substring(17,19);
    if (src) *src = "RTC";
    return;
  }
  // 2) SNTP/system time
  struct tm tm{}; 
  if (getLocalTime(&tm, 0)) {
    char bufIso[20];
    snprintf(bufIso, sizeof(bufIso), "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    iso = bufIso;
    char buf14[15];
    strftime(buf14, sizeof(buf14), "%Y%m%d%H%M%S", &tm);
    ts14 = buf14;
    if (src) *src = "SNTP";
    return;
  }
  // 3) millis fallback
  unsigned long sec = millis()/1000UL;
  unsigned long hh = (sec/3600UL)%24UL, mm=(sec/60UL)%60UL, ss=sec%60UL;
  char isoBuf[20], tsBuf[15];
  snprintf(isoBuf, sizeof(isoBuf), "1970-01-01 %02lu:%02lu:%02lu", hh, mm, ss);
  snprintf(tsBuf,  sizeof(tsBuf),  "19700101%02lu%02lu%02lu",      hh, mm, ss);
  iso  = isoBuf; ts14 = tsBuf; if (src) *src = "MILLIS";
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
      delete it; Serial.println("[LoRa] queue full; dropping packet");
    }
  });
  return true;
}

void LoraRxService::taskLoop() {
  for(;;) {
    lora_.pollOnce();

    for (int i = 0; i < 4; i++) {
      Item* it = nullptr;
      if (xQueueReceive(queue_, &it, 0) != pdPASS) break;
      if (!it) continue;

      String iso, ts14; const char* src = "?";
      makeTimestamps(rtc_, iso, ts14, &src);

      domain::LogEntry e;
      e.scanner_id = it->scanner;
      e.rfid       = it->rfid;
      e.ts_iso     = iso.c_str();
      e.sent       = false;

      Serial.printf("[LoRa] RX scanner=%s rfid=%s ts=%s (src=%s)\n",
        e.scanner_id.c_str(), e.rfid.c_str(), e.ts_iso.c_str(), src);

      repo_.append(e);

      SDfs.lock();
      do {
        if (!SDfs.isMounted()) { Serial.println("[LoRa] SD not mounted; skip spool"); break; }
        if (!SD.exists(kSpoolDir)) SD.mkdir(kSpoolDir);

        String fname = String(kSpoolDir) + "/LOG." + e.rfid.c_str() + "." + ts14 + "." + e.scanner_id.c_str();
        if (SD.exists(fname)) {               // very rare same-second collision
          for (uint32_t n=2; n<1000; ++n) {
            String alt = fname + "." + String(n);
            if (!SD.exists(alt)) { fname = alt; break; }
          }
        }
        File f = SD.open(fname, FILE_WRITE);
        if (!f) { Serial.printf("[LoRa] Spool create failed: %s\n", fname.c_str()); break; }
        f.close();
        Serial.printf("[LoRa] Spooled %s\n", fname.c_str());
      } while(false);
      SDfs.unlock();

      delete it;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
