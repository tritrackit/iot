#pragma once
#include <string>
#include "infra/log_repo.h"
#include "infra/net_client.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

struct UploadCfg {
  std::string api;
  uint32_t    interval_ms = 15000;
  size_t      batch_size  = 50;
  // New: retry policy
  uint8_t     retry_count    = 0;     // additional attempts per batch (safer default)
  uint32_t    retry_delay_ms = 2000;  // delay between retries
};

class UploaderService {
  LogRepo&   repo_;
  NetClient& net_;
  UploadCfg  cfg_;
  TaskHandle_t task_ = nullptr;
  volatile bool enabled_ = false;  // global switch
  uint16_t consec_fail_ = 0;       // consecutive failures guard
  static constexpr uint16_t kMaxConsecFail = 5;
  volatile uint32_t warmup_deadline_ms_ = 0; // defer first try after start
  struct UploadDebug {
    uint32_t     last_ms    = 0;
    int          code       = 0;
    bool         success    = false;
    std::string  error;
    size_t       sent       = 0;
    size_t       resp_size  = 0;
    std::string  url;
    std::string  scanner;
    size_t       items      = 0;   // number of log items in last payload
    bool         array_body = true; // true if body was a JSON array
  } debug_;
public:
  UploaderService(LogRepo& r, NetClient& n) : repo_(r), net_(n) {}
  void set(const UploadCfg& c) { cfg_ = c; }
  const UploadCfg& cfg() const { return cfg_; }
  bool isEnabled() const { return enabled_; }
  void setEnabled(bool on) { enabled_ = on; }
  bool canRun() const { return !cfg_.api.empty() && cfg_.interval_ms > 1000; }
  void ensureTask();
  void taskLoop();
  const UploadDebug& debug() const { return debug_; }
  void disable() { enabled_ = false; }
  void armWarmup(uint32_t ms);
};
