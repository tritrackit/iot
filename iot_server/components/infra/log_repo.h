#pragma once
#include <vector>
#include "domain/log_entry.h"

class LogRepo {
public:
  virtual ~LogRepo() = default;
  virtual bool ensureReady() = 0;
  virtual bool append(const domain::LogEntry&) = 0;
  virtual std::vector<domain::LogEntry> listAll(size_t maxN) = 0;
  virtual std::vector<domain::LogEntry> listUnsent(size_t limit) = 0;
  virtual bool markSent(const std::vector<domain::LogEntry>&) = 0;
  // New: set a message for the provided entries (keeps sent=false)
  virtual bool markFailed(const std::vector<domain::LogEntry>&, const std::string& message) = 0;
};
