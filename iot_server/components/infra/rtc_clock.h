#pragma once
#include <string>

class TwoWire;

class RtcClock {
public:
  virtual ~RtcClock() = default;
  virtual bool begin(TwoWire* wire) = 0;             // init using I2C bus
  virtual std::string nowIso() = 0;                  // "YYYY-MM-DD HH:MM:SS"
  virtual void adjustYMDHMS(int y,int mo,int d,int h,int mi,int s) = 0;
};

RtcClock* makeRtcDs3231();
