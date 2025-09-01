// components/infra/rtc_ds3231.cpp
#include "rtc_clock.h"
#include <Arduino.h>
#include <Wire.h>
#include <RTClib.h>

static inline bool looksValid(const DateTime& n){
  return (n.year()   >= 2020 && n.year()   <= 2099) &&
         (n.month()  >= 1    && n.month()  <= 12)   &&
         (n.day()    >= 1    && n.day()    <= 31)   &&
         (n.hour()   >= 0    && n.hour()   <  24)   &&
         (n.minute() >= 0    && n.minute() <  60)   &&
         (n.second() >= 0    && n.second() <  60);
}

class RtcDs3231 : public RtcClock {
  RTC_DS3231 rtc_;
  bool ready_ = false;

  static void toIso(const DateTime& t, char* out, size_t n){
    snprintf(out, n, "%04d-%02d-%02d %02d:%02d:%02d",
             t.year(), t.month(), t.day(), t.hour(), t.minute(), t.second());
  }

public:
  bool begin(TwoWire* wire) override {
    // Choose I2C bus pointer (RTClib wants a pointer)
    TwoWire* bus = wire ? wire : &Wire;

    // Faster I2C can help on longer leads
    bus->setClock(400000);

    // Optional quick probe @0x68 (helps with diagnostics)
    bus->beginTransmission(0x68);
    uint8_t rc = bus->endTransmission();
    if (rc != 0) {
      Serial.printf("[RTC] DS3231 not found at 0x68 (I2C rc=%u)\n", rc);
      // Continue; some boards NACK this probe but still work.
    }

    // ✅ Correct: pass a POINTER
    bool ok = rtc_.begin(bus);
    if (!ok) {
      Serial.println("[RTC] rtc_.begin() failed");
      ready_ = false;
      return false;
    }

    // If lost power or invalid, seed with compile time
    if (rtc_.lostPower() || !looksValid(rtc_.now())) {
      Serial.println("[RTC] lostPower/invalid; seeding from compile time");
      rtc_.adjust(DateTime(F(__DATE__), F(__TIME__)));
    }

    ready_ = looksValid(rtc_.now());
    char iso[24]; toIso(rtc_.now(), iso, sizeof(iso));
    Serial.printf("[RTC] Ready=%d Current=%s\n", (int)ready_, iso);
    return ready_;
  }

  std::string nowIso() override {
    if (ready_) {
      DateTime n = rtc_.now();
      if (looksValid(n)) {
        char buf[24]; toIso(n, buf, sizeof(buf));
        return std::string(buf);
      }
    }
    // Fallback (ticks so you know it's alive)
    unsigned long sec = millis()/1000UL;
    unsigned long hh = (sec/3600UL) % 24UL;
    unsigned long mm = (sec/60UL)   % 60UL;
    unsigned long ss =  sec         % 60UL;
    char buf[24];
    snprintf(buf, sizeof(buf), "1970-01-01 %02lu:%02lu:%02lu", hh, mm, ss);
    return std::string(buf);
  }

  void adjustYMDHMS(int y,int mo,int d,int h,int mi,int s) override {
    rtc_.adjust(DateTime(y, mo, d, h, mi, s));
    ready_ = looksValid(rtc_.now());
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", y, mo, d, h, mi, s);
    Serial.printf("[RTC] adjustYMDHMS -> %s (ready=%d)\n", buf, (int)ready_);
  }
};

RtcClock* makeRtcDs3231(){ return new RtcDs3231(); }
