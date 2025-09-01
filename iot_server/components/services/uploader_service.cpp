// components/services/uploader_service.cpp
#include "uploader_service.h"

#include <Arduino.h>
#include <WiFi.h>
#include <SD.h>

#include <map>
#include <vector>
#include <algorithm>
#include <string>

// ─────────────────────────────────────────────────────────────
// Task trampoline
// ─────────────────────────────────────────────────────────────
static void uploader_task_entry(void* arg){
  static_cast<UploaderService*>(arg)->taskLoop();
}

// ─────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────
void UploaderService::armWarmup(uint32_t ms){
  warmup_deadline_ms_ = millis() + ms;
}

void UploaderService::ensureTask(){
  if (task_) return;
  const uint32_t stackWords = 6144; // ~24KB
  BaseType_t rc = xTaskCreatePinnedToCore(
    uploader_task_entry,
    "upl_task",
    stackWords,
    this,
    1,
    &task_,
    1 /* pin to APP CPU; keep core 0 lighter for wifi/async tcp */);
  if (rc != pdPASS){
    task_ = nullptr;
    enabled_ = false;
    Serial.println("[UP] ERROR: failed to create upload task (out of memory)");
  }
}

// ─────────────────────────────────────────────────────────────
// Spool helpers  (filename = LOG.<rfid>.<YYYYMMDDHHMMSS>.<scanner>[.N])
// ─────────────────────────────────────────────────────────────

// Convert 14-digit yyyymmddhhmmss -> "YYYY-MM-DD HH:MM:SS"
static String digits14ToIso(const String& ts14){
  if (ts14.length() != 14) return String("");
  const char* s = ts14.c_str();
  for (int i=0;i<14;i++) if (s[i]<'0' || s[i]>'9') return String("");
  char buf[20];
  snprintf(buf,sizeof(buf), "%c%c%c%c-%c%c-%c%c %c%c:%c%c:%c%c",
           s[0],s[1],s[2],s[3], s[4],s[5], s[6],s[7],
           s[8],s[9], s[10],s[11], s[12],s[13]);
  return String(buf);
}

// base must be just the filename (no directory)
static bool parseSpoolBaseNew(const String& base, String& rfid, String& tsIso, String& scanner){
  if (!base.startsWith("LOG.")) return false;

  // Expect: LOG.<rfid>.<YYYYMMDDHHMMSS>.<scanner>[.N]
  int p1 = base.indexOf('.', 4);        if (p1 < 0) return false;           // after rfid
  int p2 = base.indexOf('.', p1 + 1);   if (p2 < 0) return false;           // after ts14
  int p3 = base.indexOf('.', p2 + 1);   // optional suffix

  rfid = base.substring(4, p1);
  String ts14 = base.substring(p1 + 1, p2);
  scanner = (p3 >= 0) ? base.substring(p2 + 1, p3)
                      : base.substring(p2 + 1);

  if (rfid.length()==0 || scanner.length()==0) return false;

  tsIso = digits14ToIso(ts14);
  // We still accept empty tsIso (if malformed) to avoid dropping the item
  return true;
}

String UploaderService::baseName(const char* p){
  String s(p ? p : "");
  int k = s.lastIndexOf('/');
  return (k >= 0) ? s.substring(k+1) : s;
}

bool UploaderService::spoolListGrouped(size_t max_total,
                                       std::map<String, std::vector<SpoolItem>>& byScanner)
{
  byScanner.clear();
  if (!sdfs_) return false;
  if (max_total == 0) max_total = 50;

  sdfs_->lock();

  // Ensure dir exists (LoRa should create it, but make it robust)
  if (!SD.exists(cfg_.spool_dir.c_str())) {
    SD.mkdir(cfg_.spool_dir.c_str());
  }

  File dir = SD.open(cfg_.spool_dir.c_str());
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    sdfs_->unlock();
    return true; // treat as empty rather than error
  }

  size_t collected = 0;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    if (f.isDirectory()) { f.close(); continue; }

    String full = f.name();
    String base = baseName(full.c_str());
    f.close();

    if (!base.startsWith("LOG.")) continue;

    String rfid, tsIso, scanner;
    if (!parseSpoolBaseNew(base, rfid, tsIso, scanner)) continue;

    SpoolItem it;
    it.path    = String(cfg_.spool_dir.c_str()) + "/" + base;
    it.rfid    = rfid;
    it.scanner = scanner;
    it.ts      = tsIso; // extracted from filename

    byScanner[scanner].push_back(std::move(it));

    ++collected;
    if (collected >= max_total) break;
    if ((collected & 0x3F) == 0) vTaskDelay(1); // yield while scanning
  }

  dir.close();
  sdfs_->unlock();

  // Stable order inside each scanner group (by ts then RFID, fallback to RFID)
  for (auto& kv : byScanner) {
    auto& vec = kv.second;
    std::sort(vec.begin(), vec.end(),
      [](const SpoolItem& a, const SpoolItem& b){
        int c = a.ts.compareTo(b.ts);
        if (c == 0) c = a.rfid.compareTo(b.rfid);
        return c < 0;
      });
  }

  return true;
}

bool UploaderService::spoolDeleteFiles(const std::vector<SpoolItem>& items){
  if (!sdfs_) return false;
  bool all = true;
  sdfs_->lock();
  for (size_t i=0;i<items.size();++i){
    if (!SD.remove(items[i].path.c_str())) {
      all = false;
      Serial.printf("[UP] WARN: failed to delete %s\n", items[i].path.c_str());
    }
    if ((i & 0x1F) == 0) vTaskDelay(1);
  }
  sdfs_->unlock();
  return all;
}

// ─────────────────────────────────────────────────────────────
// Worker loop
// ─────────────────────────────────────────────────────────────
void UploaderService::taskLoop(){
  uint32_t    next_due = 0;
  wl_status_t prevSta  = WL_DISCONNECTED;

  for(;;){
    // optional warmup
    if (warmup_deadline_ms_){
      int32_t t = (int32_t)(warmup_deadline_ms_ - millis());
      if (t > 0){ vTaskDelay(pdMS_TO_TICKS(t > 100 ? 100 : t)); continue; }
      warmup_deadline_ms_ = 0;
    }

    if (!enabled_ || cfg_.api.empty() || cfg_.interval_ms <= 1000){
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }

    wl_status_t st = WiFi.status();
    if (st==WL_CONNECTED && prevSta!=WL_CONNECTED) { next_due = 0; }
    prevSta = st;

    if (next_due == 0) next_due = millis();
    int32_t remain = (int32_t)(next_due - millis());
    if (remain > 0){ vTaskDelay(pdMS_TO_TICKS(remain > 50 ? 50 : remain)); continue; }

    if (WiFi.status() != WL_CONNECTED) { next_due = millis() + cfg_.interval_ms; continue; }

    if (ESP.getFreeHeap() < 25000) {
      debug_.last_ms = millis();
      debug_.success = false;
      debug_.code = -1;
      debug_.error = "low_heap";
      next_due = millis() + 2000;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    Serial.println("[UP] Starting upload cycle");
    Serial.printf(" task=%p core=%d heap=%u\n",
                  xTaskGetCurrentTaskHandle(), xPortGetCoreID(), (unsigned)ESP.getFreeHeap());
    Serial.printf(" API: %s\n", cfg_.api.c_str());
    Serial.printf(" Source: %s\n", cfg_.use_sd_spool ? "spool" : "repo");

    // ======== SPOOL MODE ========
    if (cfg_.use_sd_spool && sdfs_) {
      Serial.printf(" Spool dir: %s\n", cfg_.spool_dir.c_str());
      const size_t want = (cfg_.batch_size ? cfg_.batch_size : 50);

      // scan a bit more to form groups
      std::map<String, std::vector<SpoolItem>> groups;
      bool okList = spoolListGrouped(want * 8, groups);
      if (!okList) {
        debug_.last_ms = millis(); debug_.success = false; debug_.code = -10; debug_.error = "spool_list_failed";
        next_due = millis() + cfg_.interval_ms;
        continue;
      }

      if (groups.empty()) {
        debug_.last_ms = millis(); debug_.success = true; debug_.code = 204; debug_.error.clear();
        next_due = millis() + cfg_.interval_ms;
        continue;
      }

      // Pick first non-empty scanner group
      auto it = groups.begin();
      while (it != groups.end() && it->second.empty()) ++it;
      if (it == groups.end()) {
        debug_.last_ms = millis(); debug_.success = true; debug_.code = 204; debug_.error.clear();
        next_due = millis() + cfg_.interval_ms;
        continue;
      }

      const String scanner = it->first;
      auto items = it->second;
      if (items.size() > want) items.resize(want);

      Serial.printf("[UP] Spool: scanner=%s items=%u\n", scanner.c_str(), (unsigned)items.size());
      for (auto& si : items) {
        Serial.printf("  file=%s rfid=%s ts=%s\n", si.path.c_str(), si.rfid.c_str(), si.ts.c_str());
      }

      // Build JSON: {"data":[{"rfid":"..","timestamp":".."}, ...]}
      std::string body; body.reserve(96 + 64*items.size());
      body += "{\"data\":[";
      for (size_t i=0;i<items.size();++i){
        if (i) body += ',';
        body += "{\"rfid\":\""; body += items[i].rfid.c_str();
        body += "\",\"timestamp\":\""; body += items[i].ts.length()? items[i].ts.c_str() : "";
        body += "\"}";
      }
      body += "]}";

      bool success=false; int code=0; std::string resp; std::string failMsg;
      debug_.url = cfg_.api; debug_.scanner = scanner.c_str(); debug_.sent = body.size();
      debug_.items = items.size(); debug_.array_body = false;

      const std::string apiKey = scanner.length() ? std::string(scanner.c_str())
                                                  : std::string("SCANNER_UNKNOWN");

      delay(0);

      for (uint8_t attempt=0; attempt<=cfg_.retry_count; ++attempt){
        bool ok = net_.postJson(cfg_.api, body, code, resp, apiKey);
        if (ok && code>=200 && code<300){ success=true; break; }
        failMsg = ok ? (std::string("HTTP_") + std::to_string(code)) : std::string("NET_ERR");
        if (attempt < cfg_.retry_count) vTaskDelay(pdMS_TO_TICKS(cfg_.retry_delay_ms));
      }

      debug_.last_ms = millis(); debug_.code = code; debug_.success = success;
      debug_.resp_size = resp.size(); debug_.error = success? std::string() : failMsg;

      next_due = millis() + cfg_.interval_ms;

      if (success){
        consec_fail_ = 0;
        if (spoolDeleteFiles(items)) {
          Serial.printf("[UP] Sent & deleted %u files for scanner=%s\n", (unsigned)items.size(), scanner.c_str());
        } else {
          Serial.printf("[UP] Sent %u files but some deletes failed (scanner=%s)\n", (unsigned)items.size(), scanner.c_str());
        }
      } else {
        Serial.printf("[UP] Spool upload failed: code=%d err=%s (scanner=%s)\n", code, failMsg.c_str(), scanner.c_str());
        consec_fail_++;
        if (code==401 || code==403 || consec_fail_ >= kMaxConsecFail){
          Serial.printf("[UP] Disabling uploader (code=%d, consec_fail=%u)\n", code, (unsigned)consec_fail_);
          enabled_ = false;
        }
      }
      continue;
    }

    // ======== REPO MODE (fallback) ========
    auto window = repo_.listUnsent((size_t)500);
    if (window.empty()) { next_due = millis() + cfg_.interval_ms; continue; }

    std::string scanner = window.front().scanner_id;
    Serial.printf("[UP] Uploading batch for scanner=%s (items=%u)\n", scanner.c_str(), (unsigned)window.size());
    std::vector<domain::LogEntry> batch; batch.reserve(window.size());
    for (const auto& e : window){ if (e.scanner_id == scanner) batch.push_back(e); }
    size_t maxItems = (consec_fail_ > 0) ? 1 : cfg_.batch_size;
    if (maxItems == 0) maxItems = 1;
    if (batch.size() > maxItems) batch.resize(maxItems);

    std::string body; body.reserve(80 + 80*batch.size());
    body += "{\"data\":[";
    for (size_t i=0;i<batch.size();++i){
      const auto& e = batch[i];
      if (i) body += ',';
      body += "{\"rfid\":\""; body += e.rfid;
      body += "\",\"timestamp\":\""; body += e.ts_iso; body += "\"}";
      Serial.printf("  scanner=%s rfid=%s ts=%s\n", e.scanner_id.c_str(), e.rfid.c_str(), e.ts_iso.c_str());
    }
    body += "]}";

    bool success=false; int code=0; std::string resp; std::string failMsg;

    debug_.url = cfg_.api; debug_.scanner = scanner; debug_.sent = body.size(); debug_.items = batch.size(); debug_.array_body = false;

    const std::string apiKey = scanner.empty() ? std::string("SCANNER_UNKNOWN") : scanner;

    delay(0);

    for (uint8_t attempt=0; attempt<=cfg_.retry_count; ++attempt){
      bool ok = net_.postJson(cfg_.api, body, code, resp, apiKey);
      if (ok && code>=200 && code<300){ success=true; break; }
      failMsg = ok ? (std::string("HTTP_") + std::to_string(code)) : std::string("NET_ERR");
      if (attempt < cfg_.retry_count) vTaskDelay(pdMS_TO_TICKS(cfg_.retry_delay_ms));
    }

    debug_.last_ms = millis(); debug_.code = code; debug_.success = success; debug_.resp_size = resp.size(); debug_.error = success? std::string() : failMsg;
    next_due = millis() + cfg_.interval_ms;

    if (success){
      consec_fail_ = 0;
      repo_.markSent(batch);
      Serial.println("[UP] Uploaded batch:");
      for (auto& e : batch){
        Serial.printf("  scanner=%s rfid=%s ts=%s\n", e.scanner_id.c_str(), e.rfid.c_str(), e.ts_iso.c_str());
      }
    } else {
      size_t sentCount = 0;
      Serial.printf("[UP] Upload failed: code=%d err=%s (sent %u of %u individually)\n", code, failMsg.c_str(), (unsigned)sentCount, (unsigned)batch.size());
      repo_.markFailed(batch, failMsg);
      consec_fail_++;
      if (code==401 || code==403 || consec_fail_ >= kMaxConsecFail){
        Serial.printf("[UP] Disabling uploader (code=%d, consec_fail=%u)\n", code, (unsigned)consec_fail_);
        enabled_ = false;
      }
    }
  }
}
