// components/services/uploader_service.h
#pragma once

#include <Arduino.h>                 // String
#include <map>
#include <vector>
#include <string>

#include "infra/log_repo.h"
#include "infra/net_client.h"
#include "infra/sd_fs.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct UploadCfg {
  // REST endpoint
  std::string api;

  // cadence + batching
  uint32_t    interval_ms = 15000;
  size_t      batch_size  = 50;

  // retry policy
  uint8_t     retry_count    = 0;     // additional attempts per batch
  uint32_t    retry_delay_ms = 2000;  // ms between retries

  // SPOOL mode (one file per log) — default ON
  bool        use_sd_spool   = true;
  String      spool_dir      = "/spool";
};

class UploaderService {
  // deps
  LogRepo&     repo_;
  NetClient&   net_;
  SdFsImpl*    sdfs_ = nullptr; // nullable (repo-only mode if null)

  // task + state
  TaskHandle_t task_ = nullptr;
  volatile bool enabled_ = false;
  uint16_t     consec_fail_ = 0;
  static constexpr uint16_t kMaxConsecFail = 5;
  volatile uint32_t warmup_deadline_ms_ = 0;

public:
  struct UploadDebug {
    uint32_t     last_ms    = 0;
    int          code       = 0;
    bool         success    = false;
    std::string  error;
    size_t       sent       = 0;         // bytes of request body
    size_t       resp_size  = 0;
    std::string  url;
    std::string  scanner;
    size_t       items      = 0;         // #records in last payload
    bool         array_body = true;      // kept for UI compatibility
  };

  // A single spooled record (filename encodes scanner+rfid; file body may hold ts)
  struct SpoolItem {
    String path;     // /spool/LOG.<rfid>.<scanner>[.n]
    String rfid;
    String scanner;
    String ts;       // first line of file (best-effort)
  };

  // ctors
  UploaderService(LogRepo& r, NetClient& n) : repo_(r), net_(n) {}
  UploaderService(LogRepo& r, NetClient& n, SdFsImpl& sdfs) : repo_(r), net_(n), sdfs_(&sdfs) {}

  // config/state
  void set(const UploadCfg& c) { cfg_ = c; }
  const UploadCfg& cfg() const { return cfg_; }

  bool isEnabled() const { return enabled_; }
  void setEnabled(bool on) { enabled_ = on; }
  void disable() { enabled_ = false; }

  bool canRun() const { return !cfg_.api.empty() && cfg_.interval_ms >= 1000; }

  // lifecycle
  void ensureTask();
  void taskLoop();
  void armWarmup(uint32_t ms);

  // debug
  const UploadDebug& debug() const { return debug_; }

private:
  // spool helpers
  static String baseName(const char* p);
  static bool parseSpoolBase(const String& base, String& rfid, String& scanner);
  static bool readSmallTextFile(const String& path, String& out);

  bool spoolListGrouped(size_t max_total,
                        std::map<String, std::vector<SpoolItem>>& byScanner);
  bool spoolDeleteFiles(const std::vector<SpoolItem>& items);

private:
  UploadCfg   cfg_;
  UploadDebug debug_;
};
