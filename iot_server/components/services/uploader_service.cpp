#include "uploader_service.h"
#include <ArduinoJson.h>
#include <Arduino.h>
#include <WiFi.h>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// ─────────────────────────────────────────────────────────────────────────────
// FEATURE TOGGLES (safe defaults)
// ─────────────────────────────────────────────────────────────────────────────
// Enable this to physically trim logs.csv after each successful upload.
// Leave OFF while stabilizing to avoid heavy I/O right in the hot path.
// #define UP_ENABLE_TRIM_AFTER_UPLOAD 1

// If you need to test SD lock contention, you can temporarily skip SdFsImpl::lock()
// (NOT for production)
// #if 0
// #define UP_SD_LOCKLESS_DEBUG 1
// #endif

// Cursor file path on SD
static constexpr const char* kCursorPath = "/upload.cursor";

static void uploader_task_entry(void* arg){
  static_cast<UploaderService*>(arg)->taskLoop();
}

void UploaderService::armWarmup(uint32_t ms){
  warmup_deadline_ms_ = millis() + ms;
}

void UploaderService::ensureTask(){
  if (task_) return;
  const uint32_t stackWords = 6144; // ~24KB (TLS + JSON headroom)
  BaseType_t rc = xTaskCreatePinnedToCore(
    uploader_task_entry,
    "upl_task",
    stackWords,
    this,
    1,
    &task_,
    1 /* pin to APP CPU; avoid starving async_tcp on core 0 */);
  if (rc != pdPASS){
    task_ = nullptr;
    enabled_ = false;
    Serial.println("[UP] ERROR: failed to create upload task (out of memory)");
  }
}

// ───────────────────────── CSV helpers ─────────────────────────
// NOTE: use StaticJsonDocument (bounded stack, no heap fragmentation)
bool UploaderService::csvLoadCursor(uint64_t& off, uint32_t& line){
  off = 0; line = 0;
  if (!sdfs_) return true;

  String raw;
  if (!sdfs_->readAll(kCursorPath, raw)) {
    return true; // no cursor file yet -> start at 0
  }

  StaticJsonDocument<128> d;  // bounded; avoids runtime heap allocs
  auto err = deserializeJson(d, raw);
  if (err) {
    // malformed cursor -> ignore and start at 0
    return true;
  }
  off  = (uint64_t)(d["offset"] | 0);
  line = (uint32_t)(d["line"]   | 0);
  return true;
}

bool UploaderService::csvPersistCursor(uint64_t off, uint32_t line){
  if (!sdfs_) return true;

  StaticJsonDocument<128> d;
  d["offset"] = off;
  d["line"]   = line;
  String s;
  // serializeJson() on StaticJsonDocument never allocates on the heap
  serializeJson(d, s);
  return sdfs_->writeAll(kCursorPath, s);
}

void UploaderService::trimCrlf(String& s){
  while (s.length() && (s.endsWith("\r") || s.endsWith("\n"))) s.remove(s.length()-1);
}

bool UploaderService::parseCsvLine(const String& ln, String& scanner, String& rfid, String& ts, String& code, String& msg){
  int c1 = ln.indexOf(','); if (c1 < 0) return false;
  int c2 = ln.indexOf(',', c1+1); if (c2 < 0) return false;
  int c3 = ln.indexOf(',', c2+1); if (c3 < 0) return false;
  int c4 = ln.indexOf(',', c3+1); // may be -1
  scanner = ln.substring(0, c1);
  rfid    = ln.substring(c1+1, c2);
  ts      = ln.substring(c2+1, c3);
  code    = (c4 >= 0) ? ln.substring(c3+1, c4) : ln.substring(c3+1);
  msg     = (c4 >= 0) ? ln.substring(c4+1) : String("");
  return true;
}

// PRELOAD cursor *before* taking SD lock (readAll() uses its own lock)
bool UploaderService::csvReadNextBatch(size_t want, std::vector<String>& lines, uint64_t& newOffset){
  lines.clear();
  newOffset = csv_offset_;
  if (!sdfs_) {
    Serial.println("[UP] (csv) sdfs_ is null");
    return false;
  }

  if (csv_offset_ == 0 && csv_line_ == 0) {
    // This does NOT take the SdFsImpl user lock -> safe to call now
    csvLoadCursor(csv_offset_, csv_line_);
    newOffset = csv_offset_;
  }

  uint32_t t0 = millis();
#ifndef UP_SD_LOCKLESS_DEBUG
  sdfs_->lock();
#endif

  File f = SD.open(cfg_.csv_path.c_str(), FILE_READ);
  if (!f) {
#ifndef UP_SD_LOCKLESS_DEBUG
    sdfs_->unlock();
#endif
    return false;
  }

  // Handle truncation/rotation
  uint64_t sz = f.size();
  if (csv_offset_ > sz) { csv_offset_ = 0; csv_line_ = 0; }
  (void)f.seek(csv_offset_);

  // If at file start, optionally consume header
  if (f.position() == 0) {
    String first;
    while (f.available()){
      int ch = f.read();
      if (ch == '\r') continue;
      if (ch == '\n') break;
      first += (char)ch;
      if ((first.length() & 0x3FF) == 0) vTaskDelay(1); // periodic yield
    }
    trimCrlf(first);
    if (!first.startsWith("scanner,rfid,timestamp")) {
      if (first.length()) lines.push_back(first); // no header -> treat as data row
    }
  }

  // Read up to 'want' lines
  String ln;
  uint32_t iter = 0;
  while (f.available() && lines.size() < want) {
    int ch = f.read();
    if (ch == '\r') { if ((++iter & 0x3FF) == 0) vTaskDelay(1); continue; }
    if (ch == '\n') {
      trimCrlf(ln);
      if (ln.length()) { lines.push_back(ln); }
      ln = "";
      if ((++iter & 0x3FF) == 0) vTaskDelay(1);
      continue;
    }
    ln += (char)ch;
    if ((++iter & 0x3FF) == 0) vTaskDelay(1);
  }

  // capture tail if EOF without newline
  trimCrlf(ln);
  if (ln.length() && lines.size() < want) lines.push_back(ln);

  newOffset = f.position();
  f.close();
#ifndef UP_SD_LOCKLESS_DEBUG
  sdfs_->unlock();
#endif

  (void)t0; // keep for future profiling if needed
  return true;
}

// Compact CSV by keeping unsent tail only (thresholded, optional call)
bool UploaderService::compactCsvIfNeeded() {
  if (!sdfs_ || !cfg_.use_sd_csv) return false;

#ifndef UP_SD_LOCKLESS_DEBUG
  sdfs_->lock();
#endif
  File f = SD.open(cfg_.csv_path.c_str(), FILE_READ);
  if (!f) {
#ifndef UP_SD_LOCKLESS_DEBUG
    sdfs_->unlock();
#endif
    return false;
  }

  uint64_t size = f.size();
  uint64_t off  = csv_offset_;

  // Thresholds: compact when cursor >256KB and >60% of file
  static const uint32_t kMinAdvanceBytes = 256 * 1024;
  static const uint8_t  kMinAdvancePct   = 60;
  bool should = (off >= kMinAdvanceBytes) && (size > 0) && ((off * 100ull / size) >= kMinAdvancePct);
  if (!should) { f.close();
#ifndef UP_SD_LOCKLESS_DEBUG
    sdfs_->unlock();
#endif
    return false;
  }

  const char* tmpPath = "/logs.new";
  SD.remove(tmpPath);
  File nf = SD.open(tmpPath, FILE_WRITE);
  if (!nf) { f.close();
#ifndef UP_SD_LOCKLESS_DEBUG
    sdfs_->unlock();
#endif
    return false; }

  // Write header
  nf.print("scanner,rfid,timestamp,code,message\n");

  // Copy tail (from current cursor to EOF)
  f.seek(off);
  uint8_t buf[1024];
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    nf.write(buf, n);
    vTaskDelay(1); // yield
  }

  nf.flush(); nf.close();
  f.close();

  SD.remove(cfg_.csv_path.c_str());
  bool renamed = SD.rename(tmpPath, cfg_.csv_path.c_str());

  if (renamed) {
    csv_offset_ = 0;
    csv_line_   = 0;
    csvPersistCursor(0, 0);
    Serial.println("[UP] Compacted logs.csv (kept unsent tail only)");
  } else {
    SD.remove(tmpPath);
  }

#ifndef UP_SD_LOCKLESS_DEBUG
  sdfs_->unlock();
#endif

  return renamed;
}

// Trim CSV physically to current cursor (disabled by default; see toggle above)
bool UploaderService::trimCsvToCursor() {
#ifndef UP_ENABLE_TRIM_AFTER_UPLOAD
  return false; // feature disabled by default to avoid heavy I/O in hot path
#else
  if (!sdfs_ || !cfg_.use_sd_csv) return false;
  if (csv_offset_ == 0) return true; // nothing to trim

  sdfs_->lock();
  File f = SD.open(cfg_.csv_path.c_str(), FILE_READ);
  if (!f) { sdfs_->unlock(); return false; }

  const char* tmpPath = "/logs.new";
  SD.remove(tmpPath);
  File nf = SD.open(tmpPath, FILE_WRITE);
  if (!nf) { f.close(); sdfs_->unlock(); return false; }

  // Always rewrite header
  nf.print("scanner,rfid,timestamp,code,message\n");

  // Copy only the unsent tail (from current cursor to EOF)
  f.seek(csv_offset_);
  uint8_t buf[1024];
  size_t n;
  while ((n = f.read(buf, sizeof(buf))) > 0) {
    nf.write(buf, n);
    vTaskDelay(1); // yield for WDT
  }
  nf.flush(); nf.close(); f.close();

  // Replace original
  SD.remove(cfg_.csv_path.c_str());
  bool renamed = SD.rename(tmpPath, cfg_.csv_path.c_str());
  if (renamed) {
    // After trimming, we've kept only the unsent tail; cursor returns to the top
    csv_offset_ = 0;
    csv_line_   = 0;
    csvPersistCursor(0, 0);
    Serial.println("[UP] Trimmed logs.csv to unsent tail");
  } else {
    SD.remove(tmpPath);
  }
  sdfs_->unlock();
  return renamed;
#endif
}

// ───────────────────────── worker loop ─────────────────────────
void UploaderService::taskLoop(){
  uint32_t    next_due = 0;
  wl_status_t prevSta  = WL_DISCONNECTED;

  for(;;){
    if (warmup_deadline_ms_){
      int32_t t = (int32_t)(warmup_deadline_ms_ - millis());
      if (t > 0){ vTaskDelay(pdMS_TO_TICKS(t > 100 ? 100 : t)); continue; }
      warmup_deadline_ms_ = 0;
    }

    if (cursor_reset_req_) {
      cursor_reset_req_ = false;
      if (sdfs_) {
#ifndef UP_SD_LOCKLESS_DEBUG
        sdfs_->lock();
#endif
        (void)SD.remove(kCursorPath);
#ifndef UP_SD_LOCKLESS_DEBUG
        sdfs_->unlock();
#endif
      }
      vTaskDelay(pdMS_TO_TICKS(1));
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

    if (cfg_.api.empty()) { continue; }
    if (WiFi.status() != WL_CONNECTED) { continue; }

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
    Serial.printf(" Source: %s\n", cfg_.use_sd_csv ? "sd_csv" : "repo");

    // ===== CSV MODE =====
    if (cfg_.use_sd_csv && sdfs_) {
      Serial.printf(" CSV: %s (offset=%llu line=%lu)\n",
                    cfg_.csv_path.c_str(),
                    (unsigned long long)csv_offset_,
                    (unsigned long)csv_line_);

      std::vector<String> lines;
      uint64_t newOff = csv_offset_;
      bool okRead = csvReadNextBatch((size_t)(cfg_.batch_size ? cfg_.batch_size : 50), lines, newOff);
      Serial.printf(okRead ? "[UP] Read CSV OK\n" : "[UP] Read CSV FAIL\n");
      Serial.printf("  read %u lines (newOff=%llu)\n", (unsigned)lines.size(), (unsigned long long)newOff);

      if (!okRead) {
        debug_.last_ms = millis();
        debug_.success = false;
        debug_.code = -2;
        debug_.error = "csv_read_failed";
        next_due = millis() + cfg_.interval_ms;
        continue;
      }
      if (lines.empty()){
        debug_.last_ms = millis();
        debug_.success = true;
        debug_.code = 204;
        debug_.error.clear();
        Serial.println("[UP] CSV has no unsent rows (204)");
        next_due = millis() + cfg_.interval_ms;
        continue;
      }

      // Build JSON object {"data":[...]}
      std::string body; body.reserve(80 + 80*lines.size());
      Serial.printf("[UP] Uploading batch from CSV (items=%u)\n", (unsigned)lines.size());
      body += "{\"data\":[";
      String firstScanner;
      size_t items = 0;
      for (size_t i=0;i<lines.size();++i){
        String scanner, rfid, ts, code, msg;
        if (!parseCsvLine(lines[i], scanner, rfid, ts, code, msg)) continue;
        if (!firstScanner.length()) firstScanner = scanner;
        if (items++) body += ',';
        body += "{\"rfid\":\""; body += rfid.c_str();
        body += "\",\"timestamp\":\""; body += ts.c_str();
        body += "\"}";
        Serial.printf("  scanner=%s rfid=%s ts=%s\n", scanner.c_str(), rfid.c_str(), ts.c_str());
      }
      body += "]}";

      // Post
      bool success=false; int code=0; std::string resp; std::string failMsg;
      debug_.url = cfg_.api; debug_.scanner = firstScanner.c_str(); debug_.sent = body.size(); debug_.items = items; debug_.array_body = false;

      std::string apiKey = firstScanner.length()
        ? std::string(firstScanner.c_str())
        : std::string("SCANNER_UNKNOWN");

      delay(0); // Give LWIP/async_tcp a breath

      for (uint8_t attempt=0; attempt<=cfg_.retry_count; ++attempt){
        bool ok = net_.postJson(cfg_.api, body, code, resp, apiKey);
        if (ok && code>=200 && code<300){ success=true; break; }
        if (!ok) failMsg = "NET_ERR"; else failMsg = std::string("HTTP_") + std::to_string(code);
        if (attempt < cfg_.retry_count) { vTaskDelay(pdMS_TO_TICKS(cfg_.retry_delay_ms)); }
      }

      debug_.last_ms = millis(); debug_.code = code; debug_.success = success; debug_.resp_size = resp.size(); debug_.error = success? std::string() : failMsg;
      next_due = millis() + cfg_.interval_ms;

      if (success){
        consec_fail_ = 0;
        if (csvPersistCursor(newOff, csv_line_ + items)) {
          csv_offset_ = newOff;
          csv_line_  += items;
        }
        Serial.printf("[UP] Uploaded %u CSV rows (offset=%llu line=%lu)\n",
                      (unsigned)items, (unsigned long long)csv_offset_, (unsigned long)csv_line_);

        // Keep compaction disabled unless you need it; it's conservative on I/O.
        // compactCsvIfNeeded();

        // Physical trimming is OFF by default. Enable via #define if desired.
        trimCsvToCursor();
      } else {
        Serial.printf("[UP] CSV upload failed: code=%d err=%s\n", code, failMsg.c_str());
        consec_fail_++;
        if (code==401 || code==403 || consec_fail_ >= kMaxConsecFail){
          Serial.printf("[UP] Disabling uploader (code=%d, consec_fail=%u)\n", code, (unsigned)consec_fail_);
          enabled_ = false;
        }
      }
      continue; // skip repo path
    }
    // ===== END CSV MODE =====

    // --------- REPO MODE (unchanged) ----------
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
      body += "{\"rfid\":\""; body += e.rfid; body += "\",\"timestamp\":\""; body += e.ts_iso; body += "\"}";
      Serial.printf("  scanner=%s rfid=%s ts=%s\n", e.scanner_id.c_str(), e.rfid.c_str(), e.ts_iso.c_str());
    }
    body += "]}";

    bool success=false; int code=0; std::string resp; std::string failMsg;

    debug_.url = cfg_.api; debug_.scanner = scanner; debug_.sent = body.size(); debug_.items = batch.size(); debug_.array_body = false;

    std::string apiKey = scanner.empty() ? std::string("SCANNER_UNKNOWN") : scanner;

    delay(0);

    for (uint8_t attempt=0; attempt<=cfg_.retry_count; ++attempt){
      bool ok = net_.postJson(cfg_.api, body, code, resp, apiKey);
      if (ok && code>=200 && code<300){ success=true; break; }
      if (!ok) failMsg = "NET_ERR"; else failMsg = std::string("HTTP_") + std::to_string(code);
      if (attempt < cfg_.retry_count) { vTaskDelay(pdMS_TO_TICKS(cfg_.retry_delay_ms)); }
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
    // --------- END REPO MODE ----------
  }
}
