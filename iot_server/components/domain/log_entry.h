#pragma once
#include <string>
namespace domain {
struct LogEntry {
  std::string scanner_id;
  std::string rfid;
  std::string ts_iso;
  bool        sent = false;
  // New: error/status message for upload attempts (e.g., NET_ERR, HTTP_500, TIMEOUT)
  std::string message;
};
}
