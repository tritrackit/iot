//src/main.cpp
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>
#include <time.h>
#include <sys/time.h>

#include "infra/sd_fs.h"
#include "infra/log_repo.h"
#include "infra/lora_port.h"
#include "infra/rtc_clock.h"
#include "infra/net_client.h"

#include "services/lora_rx_service.h"
#include "services/uploader_service.h"
#include "api_http/http_api.h"

LoRaPort*   makeLoRaPortArduino(uint8_t ss, uint8_t rst, uint8_t dio0, SPIClass* spi, long freqHz);
RtcClock*   makeRtcDs3231();
NetClient*  makeNetClientHttps();
LogRepo*    makeMemLogRepo();

SdFsImpl SDfs;
static DNSServer dnsServer;

static void setTZ_AsiaManila(){
  setenv("TZ", "PST-8", 1); // UTC+8, no DST
  tzset();
}
static void configSNTP(){
  // offsets 0, rely on TZ for local time
  configTime(0, 0, "pool.ntp.org", "time.google.com", "time.nist.gov");
}
static bool waitForSNTP(uint32_t ms){
  uint32_t start = millis(); struct tm tm{};
  while (millis()-start < ms){ if (getLocalTime(&tm, 0)) return true; delay(200); }
  return false;
}
static bool parseIso(const String& iso, struct tm& out){
  if (iso.length() < 19) return false;
  memset(&out, 0, sizeof(out));
  out.tm_year = iso.substring(0,4).toInt() - 1900;
  out.tm_mon  = iso.substring(5,7).toInt() - 1;
  out.tm_mday = iso.substring(8,10).toInt();
  out.tm_hour = iso.substring(11,13).toInt();
  out.tm_min  = iso.substring(14,16).toInt();
  out.tm_sec  = iso.substring(17,19).toInt();
  return (out.tm_year >= 120 && out.tm_year <= 199);
}
static void primeSystemClockFromRTC(RtcClock& rtc){
  String iso = rtc.nowIso().c_str();
  struct tm tm{}; 
  if (!parseIso(iso, tm) || iso.startsWith("1970-01-01")) {
    Serial.printf("[RTC] Not priming system time (iso=%s)\n", iso.c_str());
    return;
  }
  time_t t = mktime(&tm); if (t <= 0) return;
  struct timeval tv{ .tv_sec = t, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  Serial.printf("[RTC] System time primed from RTC: %s\n", iso.c_str());
}

void setup(){
  Serial.begin(115200);
  (void)LittleFS.begin(true);

  // Simple config defaults (trimmed for brevity)
  String apSsid="Device-Portal", apPass="12345678";
  WiFi.persistent(false); WiFi.setAutoReconnect(true); WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.printf("[HTTP] server started on port 80. AP=%s STA=%s\n",
    WiFi.softAPIP().toString().c_str(), WiFi.localIP().toString().c_str());

  LogRepo* repo = makeMemLogRepo(); repo->ensureReady();
  NetClient* https = makeNetClientHttps();

  static UploaderService up(*repo, *https, SDfs);
  { UploadCfg c; c.api=""; c.interval_ms=15000; c.batch_size=50; c.use_sd_spool=true; c.spool_dir="/spool"; up.set(c); }
  static HttpApi api(*repo, up); api.begin();

  // Buses
  Wire.begin(21,22); Wire.setClock(400000);
  pinMode(13,OUTPUT); digitalWrite(13,HIGH); pinMode(27,OUTPUT); digitalWrite(27,HIGH);
  SPI.begin(18,19,23);
  SDfs.begin(/*cs*/13, SPI);

  // RTC -> system time (at boot)
  RtcClock* rtc = makeRtcDs3231();
  bool rtcOk = rtc->begin(&Wire);        // **NOTE**: rtc_.begin(*wire) fixed inside
  if (rtcOk) primeSystemClockFromRTC(*rtc);

  // SNTP setup (safe even if offline now)
  setTZ_AsiaManila();
  configSNTP();

  // LoRa
  LoRaPort* lora = makeLoRaPortArduino(27,25,26,&SPI,433E6);
  static LoraRxService rx(*lora, *repo, *rtc);
  xTaskCreate([](void*){ rx.begin(); rx.taskLoop(); }, "lora_rx", 4096, nullptr, 1, nullptr);

  // Try connect STA quickly if you want (optional). After Wi-Fi is up, sync NTP back to RTC.
  xTaskCreate([](void* arg){
    RtcClock* rtc = (RtcClock*)arg;
    if (waitForSNTP(20000)) {
      struct tm tm{}; if (getLocalTime(&tm, 0)) {
        rtc->adjustYMDHMS(tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
        Serial.printf("[RTC] SNTP->RTC write: %04d-%02d-%02d %02d:%02d:%02d\n",
          tm.tm_year+1900, tm.tm_mon+1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
      }
    } else {
      Serial.println("[RTC] SNTP not ready; staying with RTC time");
    }
    vTaskDelete(nullptr);
  }, "time_sync", 4096, rtc, 1, nullptr);
}

void loop(){
  if (WiFi.status() != WL_CONNECTED) dnsServer.processNextRequest();
  vTaskDelay(pdMS_TO_TICKS(10));
}
