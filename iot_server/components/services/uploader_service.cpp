// components/services/uploader_service.cpp
#include "uploader_service.h"
#include <ArduinoJson.h>
#include <Arduino.h>
#include <WiFi.h>

static void uploader_task_entry(void* arg){
  static_cast<UploaderService*>(arg)->taskLoop();
}

void UploaderService::armWarmup(uint32_t ms){
  warmup_deadline_ms_ = millis() + ms;
}

void UploaderService::ensureTask(){
  if (task_) return;
  // TLS + JSON can be stack heavy; allocate a reasonable stack (words)
  const uint32_t stackWords = 6144; // ~24KB on ESP32 FreeRTOS (TLS+JSON headroom)
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

void UploaderService::taskLoop(){
  uint32_t next_due = 0;
  wl_status_t prevSta = WL_DISCONNECTED;
  for(;;){
    // warmup window after enabling, avoid starting while HTTP handler runs
    if (warmup_deadline_ms_){
      int32_t t = (int32_t)(warmup_deadline_ms_ - millis());
      if (t > 0){ vTaskDelay(pdMS_TO_TICKS(t > 100 ? 100 : t)); continue; }
      warmup_deadline_ms_ = 0;
    }
    // honor global switch and config validity
    if (!enabled_ || cfg_.api.empty() || cfg_.interval_ms <= 1000){ vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    // Trigger immediate upload on STA connect transition
    wl_status_t st = WiFi.status();
    if (st==WL_CONNECTED && prevSta!=WL_CONNECTED) { next_due = 0; }
    prevSta = st;

    if (next_due == 0) next_due = millis();
    int32_t remain = (int32_t)(next_due - millis());
    if (remain > 0){ vTaskDelay(pdMS_TO_TICKS(remain > 50 ? 50 : remain)); continue; }

    if (cfg_.api.empty()) { continue; }               // disabled
    if (WiFi.status() != WL_CONNECTED) { continue; }  // wait for STA connection

    // Low-heap guard to avoid instability
    if (ESP.getFreeHeap() < 25000) {
      debug_.last_ms = millis();
      debug_.success = false;
      debug_.code = -1;
      debug_.error = "low_heap";
      next_due = millis() + 2000;
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Fetch a window of unsent logs and group by scanner_id (scanner code)
    auto window = repo_.listUnsent((size_t)500);
    if (window.empty()) { next_due = millis() + cfg_.interval_ms; continue; }

    // Choose first scanner group from the window
    std::string scanner = window.front().scanner_id;
    std::vector<domain::LogEntry> batch; batch.reserve(window.size());
    for (const auto& e : window){ if (e.scanner_id == scanner) batch.push_back(e); }
    // Optional cap per config batch_size; additionally degrade to single-item on recent failures
    size_t maxItems = (consec_fail_ > 0) ? 1 : cfg_.batch_size;
    if (maxItems == 0) maxItems = 1;
    if (batch.size() > maxItems) batch.resize(maxItems);

    // Build JSON object with `data` array per requirement
    std::string body; body.reserve(80 + 80*batch.size());
    body += "{\"data\":[";
    for (size_t i=0;i<batch.size();++i){
      const auto& e = batch[i];
      if (i) body += ',';
      body += "{\"rfid\":\""; body += e.rfid; body += "\",\"timestamp\":\""; body += e.ts_iso; body += "\"}";
    }
    body += "]}";

    // Retry logic for this specific batch only
    bool success=false; int code=0; std::string resp; std::string failMsg;
    debug_.url = cfg_.api; debug_.scanner = scanner; debug_.sent = body.size(); debug_.items = batch.size(); debug_.array_body = false;
    // API key from scanner code: use scanner group id (or 00000 if empty)
    // Temporary override: force fixed X-API-Key for all batches
    std::string apiKey = "SCANNER_STORAGE";
    for (uint8_t attempt=0; attempt<=cfg_.retry_count; ++attempt){
      bool ok = net_.postJson(cfg_.api, body, code, resp, apiKey);
      if (ok && code>=200 && code<300){ success=true; break; }
      // classify message
      if (!ok) failMsg = "NET_ERR"; else failMsg = std::string("HTTP_") + std::to_string(code);
      if (attempt < cfg_.retry_count) { vTaskDelay(pdMS_TO_TICKS(cfg_.retry_delay_ms)); }
    }

    debug_.last_ms = millis(); debug_.code = code; debug_.success = success; debug_.resp_size = resp.size(); debug_.error = success? std::string() : failMsg;
    next_due = millis() + cfg_.interval_ms;
    if (success){
      consec_fail_ = 0;
      repo_.markSent(batch);
      // Serial print uploaded logs
      Serial.println("[UP] Uploaded batch:");
      for (auto& e : batch){
        Serial.printf("  scanner=%s rfid=%s ts=%s\n", e.scanner_id.c_str(), e.rfid.c_str(), e.ts_iso.c_str());
      }
    } else {
      // No per-item fallback in degraded mode (already single item) — keep load minimal
      size_t sentCount = 0;
      Serial.printf("[UP] Upload failed: code=%d err=%s (sent %u of %u individually)\n", code, failMsg.c_str(), (unsigned)sentCount, (unsigned)batch.size());
      // Mark the whole batch failed for audit trail (already-sent entries are now out of the repo)
      repo_.markFailed(batch, failMsg);
      // Backoff/disable on persistent failures or auth errors
      consec_fail_++;
      if (code==401 || code==403 || consec_fail_ >= kMaxConsecFail){
        Serial.printf("[UP] Disabling uploader (code=%d, consec_fail=%u)\n", code, (unsigned)consec_fail_);
        enabled_ = false;
      }
    }
  }
}
