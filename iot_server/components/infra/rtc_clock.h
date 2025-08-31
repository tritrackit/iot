#pragma once
#include <string>
class RtcClock {
public:
  virtual ~RtcClock() = default;
  virtual bool begin() = 0;
  virtual std::string nowIso() = 0;
};
RtcClock* makeRtcDs3231();
