#pragma once
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <Arduino.h>
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
  uint8_t     retry_count    = 0;
  uint32_t    retry_delay_ms = 2000;

  // NEW spool mode
  bool        use_sd_spool   = true;
  std::string spool_dir      = "/spool";

  // Legacy CSV knobs kept for config compatibility (unused by uploader now)
  bool        use_sd_csv     = false;
  std::string csv_path       = "/logs.csv";
};

class UploaderService {
  LogRepo&     repo_;
  NetClient&   net_;
  UploadCfg    cfg_;
  TaskHandle_t task_ = nullptr;

  volatile bool     enabled_ = false;
  uint16_t          consec_fail_ = 0;
  static constexpr uint16_t kMaxConsecFail = 5;
  volatile uint32_t warmup_deadline_ms_ = 0;

  SdFsImpl* sdfs_ = nullptr;

  struct UploadDebug {
    uint32_t     last_ms    = 0;
    int          code       = 0;
    bool         success    = false;
    std::string  error;
    size_t       sent       = 0;
    size_t       resp_size  = 0;
    std::string  url;
    std::string  scanner;
    size_t       items      = 0;
    bool         array_body = true;
  } debug_;

  // One-file-per-log spool item
  struct SpoolItem {
    String rfid;
    String scanner;
    String path;
    String ts;     // first-line timestamp (optional)
  };

  // (kept for API compatibility; no longer used)
  volatile bool cursor_reset_req_ = false;

public:
  UploaderService(LogRepo& r, NetClient& n) : repo_(r), net_(n) {}
  UploaderService(LogRepo& r, NetClient& n, SdFsImpl& sdfs) : repo_(r), net_(n), sdfs_(&sdfs) {}

  void set(const UploadCfg& c) { cfg_ = c; }
  const UploadCfg& cfg() const { return cfg_; }
  bool isEnabled() const { return enabled_; }
  void setEnabled(bool on) { enabled_ = on; }
  bool canRun() const { return !cfg_.api.empty() && cfg_.interval_ms > 1000; }

  void ensureTask();
  void taskLoop();
  void armWarmup(uint32_t ms);
  void disable() { enabled_ = false; }

  const UploadDebug& debug() const { return debug_; }

  // Kept so other code compiling against this symbol still builds:
  void requestCursorReset() { cursor_reset_req_ = false; }

private:
  // Spool helpers
  static bool parseSpoolBase(const String& base, String& rfid, String& scanner);
  static String baseName(const char* p);
  static bool readSmallTextFile(const String& path, String& out);

  bool spoolListGrouped(size_t max_total,
                        std::map<String, std::vector<SpoolItem>>& byScanner);
  bool spoolDeleteFiles(const std::vector<SpoolItem>& items);
};
