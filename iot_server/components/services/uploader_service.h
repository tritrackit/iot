//components/services/uploader_service.h
#pragma once
#include <string>
#include <vector>
#include <Arduino.h>                 // for String
#include "infra/log_repo.h"
#include "infra/net_client.h"
#include "infra/sd_fs.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct UploadCfg {
  std::string api;
  uint32_t    interval_ms = 15000;
  size_t      batch_size  = 50;

  // Retry policy
  uint8_t     retry_count    = 0;     // additional attempts per batch
  uint32_t    retry_delay_ms = 2000;  // ms between retries

  // Source selection: SD CSV (default) or in-memory repo
  bool        use_sd_csv     = true;
  std::string csv_path       = "/logs.csv";
};

class UploaderService {
  LogRepo&     repo_;
  NetClient&   net_;
  UploadCfg    cfg_;
  TaskHandle_t task_ = nullptr;

  volatile bool     enabled_ = false;      // master switch
  uint16_t          consec_fail_ = 0;      // consecutive failure guard
  static constexpr uint16_t kMaxConsecFail = 5;
  volatile uint32_t warmup_deadline_ms_ = 0; // defer first try after (re)start

  // SD access + CSV cursor
  SdFsImpl* sdfs_       = nullptr;  // nullable; if null, fall back to repo mode
  uint64_t  csv_offset_ = 0;
  uint32_t  csv_line_   = 0;

  // NEW: tiny cross-thread flag to request cursor reset safely (handled in task)
  volatile bool cursor_reset_req_ = false;

  struct UploadDebug {
    uint32_t     last_ms    = 0;
    int          code       = 0;
    bool         success    = false;
    std::string  error;
    size_t       sent       = 0;
    size_t       resp_size  = 0;
    std::string  url;
    std::string  scanner;
    size_t       items      = 0;     // number of log items in last payload
    bool         array_body = true;  // true if body was a JSON array
  } debug_;

public:
  // Ctors
  UploaderService(LogRepo& r, NetClient& n) : repo_(r), net_(n) {}
  UploaderService(LogRepo& r, NetClient& n, SdFsImpl& sdfs) : repo_(r), net_(n), sdfs_(&sdfs) {}

  // Config & state
  void set(const UploadCfg& c) { cfg_ = c; }
  const UploadCfg& cfg() const { return cfg_; }
  bool isEnabled() const { return enabled_; }
  void setEnabled(bool on) { enabled_ = on; }
  bool canRun() const { return !cfg_.api.empty() && cfg_.interval_ms > 1000; }

  // Lifecycle
  void ensureTask();
  void taskLoop();
  void armWarmup(uint32_t ms);
  void disable() { enabled_ = false; }

  // Debug
  const UploadDebug& debug() const { return debug_; }

  // NEW: called from HTTP thread; actual SD work happens inside taskLoop()
  void requestCursorReset() { cursor_reset_req_ = true; }

private:
  // CSV helpers
  bool csvLoadCursor(uint64_t& off, uint32_t& line);
  bool csvPersistCursor(uint64_t off, uint32_t line);
  bool csvReadNextBatch(size_t want, std::vector<String>& lines, uint64_t& newOffset);
  static void trimCrlf(String& s);
  static bool parseCsvLine(const String& ln, String& scanner, String& rfid, String& ts, String& code, String& msg);
  bool compactCsvIfNeeded();  // copy unsent tail to new file, rotate, reset cursor
  bool trimCsvToCursor();
};
