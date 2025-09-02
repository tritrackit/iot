// src/main.cpp
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
#include <esp_wifi.h>

#include "infra/sd_fs.h"
#include "infra/log_repo.h"
#include "infra/lora_port.h"
#include "infra/rtc_clock.h"
#include "infra/net_client.h"
#include "services/lora_rx_service.h"
#include "services/uploader_service.h"
#include "api_http/http_api.h"

// Factories
LoRaPort*   makeLoRaPortArduino(uint8_t ss, uint8_t rst, uint8_t dio0, SPIClass* spi, long freqHz);
RtcClock*   makeRtcDs3231();
NetClient*  makeNetClientHttps();
LogRepo*    makeMemLogRepo();

// Globals
SdFsImpl SDfs;
static DNSServer dnsServer;
static bool dnsStarted = false;

// === Time helpers ===
static void setTZ_AsiaManila(){ setenv("TZ", "PST-8", 1); tzset(); }
static void configSNTP(){ configTime(0, 0, "pool.ntp.org", "time.google.com", "time.nist.gov"); }
static bool waitForSNTP(uint32_t ms){
  uint32_t start = millis(); struct tm tm{};
  while (millis() - start < ms){ if (getLocalTime(&tm, 0)) return true; delay(200); }
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

  // ===== BUSES FIRST =====
  Wire.begin(21,22);           // RTC: SDA=21, SCL=22
  Wire.setClock(400000);

  // SPI bus
  SPI.begin(18,19,23);         // SD: SCK=18, MISO=19, MOSI=23

  // ---- CRITICAL: De-select all SPI slaves BEFORE touching SD ----
  const int SD_CS   = 13;      // Your Mini Data Logger CS
  const int LORA_CS = 27;      // LoRa RA-02 NSS (from your earlier wiring)
  pinMode(SD_CS, OUTPUT);   digitalWrite(SD_CS, HIGH);   // deselect SD
  pinMode(LORA_CS, OUTPUT); digitalWrite(LORA_CS, HIGH); // deselect LoRa
  delay(10); // let rails/level shifters settle

  // ===== SD MOUNT BEFORE reading /config.json =====
  bool sd_ok = SDfs.begin(SD_CS, SPI /*, optionalHz*/);
  if (!sd_ok) {
    Serial.println("[SD] First mount failed; retry @ lower SPI clock...");
    // If your SdFsImpl supports a frequency param, use ~10 MHz on retry:
    // sd_ok = SDfs.begin(SD_CS, SPI, 10000000);
    // If not, add a short delay and try again:
    delay(100);
    sd_ok = SDfs.begin(SD_CS, SPI);
  }
  Serial.println(sd_ok ? "[SD] Mounted OK (CS=13)" : "[SD] Mount FAILED (CS=13)");

  // ===== CONFIG LOAD (prefer SD, fallback LittleFS) =====
  String apSsid="Device-Portal", apPass="12345678";
  String staSsid="", staPass="", apiUrl="";
  uint32_t uploadIntervalMs = 15000;
  {
    auto mergeAndNorm = [&](JsonDocument& src){
      JsonDocument out;
      out["wifi_ap_ssid"]      = src["wifi_ap_ssid"]      | "Device-Portal";
      out["wifi_ap_password"]  = src["wifi_ap_password"]  | "12345678";
      out["wifi_sta_ssid"]     = src["wifi_sta_ssid"]     | (src["ssid"]       | "");
      out["wifi_sta_password"] = src["wifi_sta_password"] | (src["password"]   | "");
      out["api_url"]           = src["api_url"]           | (src["apiUrl"]     | "");
      out["upload_interval"]   = src["upload_interval"]   | (src["intervalMs"] | 15000);
      return out;
    };
    const char* CFG_JSON = "/config.json";
    bool loaded = false;
    String raw;

    if (SDfs.isMounted() && SDfs.readAll(CFG_JSON, raw)) {
      JsonDocument d; auto err = deserializeJson(d, raw);
      if (!err) {
        JsonDocument n = mergeAndNorm(d);
        apSsid           = String((const char*)(n["wifi_ap_ssid"]));
        apPass           = String((const char*)(n["wifi_ap_password"]));
        staSsid          = String((const char*)(n["wifi_sta_ssid"]));
        staPass          = String((const char*)(n["wifi_sta_password"]));
        apiUrl           = String((const char*)(n["api_url"]));
        uploadIntervalMs = (uint32_t)n["upload_interval"];
        String tmp; serializeJson(n, tmp); SDfs.writeAll(CFG_JSON, tmp);
        loaded = true;
        Serial.println("[CFG] Loaded from SD:/config.json");
      } else {
        Serial.printf("[CFG] SD:/config.json parse error: %s\n", err.c_str());
      }
    } else {
      Serial.println("[CFG] SD not mounted or /config.json missing");
    }

    if (!loaded) {
      File f = LittleFS.open("/config.json", "r");
      if (f) {
        String lraw; lraw.reserve(f.size()+16);
        while (f.available()) lraw += (char)f.read();
        f.close();
        JsonDocument d; auto err = deserializeJson(d, lraw);
        if (!err) {
          JsonDocument n = mergeAndNorm(d);
          apSsid           = String((const char*)(n["wifi_ap_ssid"]));
          apPass           = String((const char*)(n["wifi_ap_password"]));
          staSsid          = String((const char*)(n["wifi_sta_ssid"]));
          staPass          = String((const char*)(n["wifi_sta_password"]));
          apiUrl           = String((const char*)(n["api_url"]));
          uploadIntervalMs = (uint32_t)n["upload_interval"];
          loaded = true;
          Serial.println("[CFG] Loaded from LittleFS:/config.json (fallback)");
          if (SDfs.isMounted()) { String tmp; serializeJson(n, tmp); SDfs.writeAll(CFG_JSON, tmp); Serial.println("[CFG] Migrated LittleFS -> SD:/config.json"); }
        } else {
          Serial.printf("[CFG] LittleFS:/config.json parse error: %s\n", err.c_str());
        }
      } else {
        Serial.println("[CFG] LittleFS:/config.json not found");
      }
    }

    if (!loaded && SDfs.isMounted()) {
      JsonDocument n;
      n["auth_user"]        = "admin";
      n["auth_password"]    = "admin";
      n["wifi_ap_ssid"]      = apSsid.c_str();
      n["wifi_ap_password"]  = apPass.c_str();
      n["wifi_sta_ssid"]     = "";
      n["wifi_sta_password"] = "";
      n["api_url"]           = "";
      n["upload_interval"]   = uploadIntervalMs;
      String tmp; serializeJson(n, tmp); SDfs.writeAll(CFG_JSON, tmp);
      Serial.println("[CFG] Created default SD:/config.json");
    }

    Serial.printf("[CFG] AP SSID='%s' PASS='%s'\n", apSsid.c_str(), apPass.c_str());
    Serial.printf("[CFG] STA SSID='%s' PASS='%s'\n", staSsid.c_str(), staPass.c_str());
    Serial.printf("[CFG] API URL='%s' INTERVAL=%u\n", apiUrl.c_str(), uploadIntervalMs);
  }

  // ===== Wi-Fi + DNS =====
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  WiFi.softAP(apSsid.c_str(), apPass.c_str());

  dnsStarted = dnsServer.start(53, "*", WiFi.softAPIP());
  Serial.printf("[HTTP] AP=%s STA=%s\n", WiFi.softAPIP().toString().c_str(), WiFi.localIP().toString().c_str());
  Serial.printf("[DNS] start=%s ip=%s\n", dnsStarted ? "ok" : "fail", WiFi.softAPIP().toString().c_str());

  // ===== Core services =====
  LogRepo* repo = makeMemLogRepo(); repo->ensureReady();
  NetClient* https = makeNetClientHttps();

  static UploaderService up(*repo, *https, SDfs);
  {
    UploadCfg c;
    c.api = apiUrl.c_str();
    c.interval_ms = uploadIntervalMs;
    c.batch_size = 50;
    c.use_sd_spool = true;
    c.spool_dir = "/spool";
    up.set(c);
    up.setEnabled(true);
    up.armWarmup(1500);
    up.ensureTask();
  }
  static HttpApi api(*repo, up); api.begin();
  
  
  // ===== RTC and time =====
  RtcClock* rtc = makeRtcDs3231();
  bool rtcOk = rtc->begin(&Wire);
  if (rtcOk) primeSystemClockFromRTC(*rtc);

  setTZ_AsiaManila();
  configSNTP();

  // ===== LoRa (after SD is settled) =====
  // LoRa SS=27, RST=25, DIO0=26 — keep CS pins unique and HIGH by default
  LoRaPort* lora = makeLoRaPortArduino(LORA_CS, 25, 26, &SPI, 433E6);
  static LoraRxService rx(*lora, *repo, *rtc);
  xTaskCreate([](void*){ rx.begin(); rx.taskLoop(); }, "lora_rx", 4096, nullptr, 1, nullptr);

  // ===== Sync NTP -> RTC later =====
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

  
  // ===== STA connect (if creds present) =====
  if (staSsid.length() && staPass.length()) {
    struct Cfg { String ssid; String pass; };
    auto* cfgp = new Cfg{ staSsid, staPass };
    auto worker = +[](void* arg){
      std::unique_ptr<Cfg> c((Cfg*)arg);
      WiFi.mode(WIFI_AP_STA);
      WiFi.setSleep(false);
      esp_wifi_set_ps(WIFI_PS_NONE);
      Serial.printf("[WIFI] Connect STA '%s' (pwlen=%u)\n", c->ssid.c_str(), (unsigned)c->pass.length());
      WiFi.begin(c->ssid.c_str(), c->pass.c_str());
      wl_status_t res = (wl_status_t)WiFi.waitForConnectResult(20000);
      Serial.printf("[WIFI] Result=%d status=%d\n", (int)res, (int)WiFi.status());
      vTaskDelete(nullptr);
    };
    xTaskCreate(worker, "sta_connect", 4096, cfgp, 1, nullptr);
  }

}

void loop(){
  if (WiFi.status() != WL_CONNECTED) dnsServer.processNextRequest();
  vTaskDelay(pdMS_TO_TICKS(10));
}
