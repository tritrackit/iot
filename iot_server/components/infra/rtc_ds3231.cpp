#include "rtc_clock.h"
#include <RTClib.h>
#include <Wire.h>

class RtcDs3231 : public RtcClock {
  RTC_DS3231 rtc_;
public:
  bool begin() override {
    if (!rtc_.begin()) return false;
    if (rtc_.lostPower()) rtc_.adjust(DateTime(F(__DATE__), F(__TIME__)));
    return true;
  }
  std::string nowIso() override {
    DateTime n = rtc_.now();
    int y = n.year(), mo = n.month(), d = n.day(), h = n.hour(), mi = n.minute(), s = n.second();
    bool valid = (y >= 2020 && y <= 2099) && (mo >= 1 && mo <= 12) && (d >= 1 && d <= 31) && (h >= 0 && h < 24) && (mi >= 0 && mi < 60) && (s >= 0 && s < 60);
    char buf[20];
    if (!valid){
      unsigned long sec = millis() / 1000UL;
      unsigned long hh = (sec / 3600UL) % 24UL;
      unsigned long mm = (sec / 60UL) % 60UL;
      unsigned long ss = sec % 60UL;
      snprintf(buf, sizeof(buf), "1970-01-01 %02lu:%02lu:%02lu", hh, mm, ss);
    } else {
      snprintf(buf,sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
    }
    return std::string(buf);
  }
};

RtcClock* makeRtcDs3231(){ return new RtcDs3231(); }
