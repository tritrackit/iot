// components/infra/mem_log_repo.cpp
#include "log_repo.h"
#include <algorithm>

class MemLogRepo : public LogRepo {
  std::vector<domain::LogEntry> items_;
public:
  bool ensureReady() override { return true; }
  bool append(const domain::LogEntry& e) override {
    items_.push_back(e); return true;
  }
  std::vector<domain::LogEntry> listAll(size_t maxN) override {
    std::vector<domain::LogEntry> out;
    out.reserve(std::min(maxN, items_.size()));
    for (size_t i=0; i<items_.size() && out.size()<maxN; ++i) out.push_back(items_[i]);
    return out;
  }
  std::vector<domain::LogEntry> listUnsent(size_t limit) override {
    std::vector<domain::LogEntry> out; out.reserve(limit);
    for (const auto& e : items_) { if (!e.sent) { out.push_back(e); if (out.size()>=limit) break; } }
    return out;
  }
  bool markSent(const std::vector<domain::LogEntry>& sent) override {
    for (auto& it : items_) {
      for (const auto& s : sent) {
        if (it.scanner_id==s.scanner_id && it.rfid==s.rfid && it.ts_iso==s.ts_iso) {
          it.sent = true; it.message.clear();
        }
      }
    }
    return true;
  }
  bool markFailed(const std::vector<domain::LogEntry>& failed, const std::string& message) override {
    for (auto& it : items_) {
      for (const auto& f : failed) {
        if (it.scanner_id==f.scanner_id && it.rfid==f.rfid && it.ts_iso==f.ts_iso) {
          it.sent = false; it.message = message;
        }
      }
    }
    return true;
  }
};

LogRepo* makeMemLogRepo(){ return new MemLogRepo(); }

