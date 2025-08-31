#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include <SPI.h>

#include "infra/sd_fs.h"
#include "infra/log_repo.h"
#include "infra/lora_port.h"
#include "infra/rtc_clock.h"
#include "infra/net_client.h"
#include "infra/config_store.h"          // << NEW

#include "services/lora_rx_service.h"
#include "services/uploader_service.h"
#include "api_http/http_api.h"

// factories
LoRaPort*   makeLoRaPortArduino(uint8_t ss, uint8_t rst, uint8_t dio0, SPIClass* spi, long freqHz);
RtcClock*   makeRtcDs3231();
NetClient*  makeNetClientHttps();
LogRepo*    makeCsvLogRepo(SdFs& fs);
LogRepo*    makeMemLogRepo();

SdFsImpl SDfs;
static DNSServer dnsServer;

void setup(){
  Serial.begin(115200);
  

  // Mount LittleFS for frontend
  bool lfs_ok = LittleFS.begin(true);
  // Load or create SD:/config.json
  String apSsid = "Device-Portal";
  String apPass = "12345678";
  String staSsid, staPass;
  String apiUrl; uint32_t intervalMs = 0;
  {
    StaticJsonDocument<512> d;
    const char* CFG_JSON = "/config.json";
    String raw;
    if (SDfs.readAll(CFG_JSON, raw)) { deserializeJson(d, raw); }
    if (!raw.length()) {
      d["auth_user"] = "admin";
      d["auth_password"] = "admin";
      d["wifi_ap_ssid"] = apSsid.c_str();
      d["wifi_ap_password"] = apPass.c_str();
      d["wifi_sta_ssid"] = "";
      d["wifi_sta_password"] = "";
      d["api_url"] = "";
      d["upload_interval"] = 0;
      String tmp; serializeJson(d, tmp); SDfs.writeAll(CFG_JSON, tmp);
    }
    apSsid = String((const char*)(d["wifi_ap_ssid"] | apSsid.c_str()));
    apPass = String((const char*)(d["wifi_ap_password"] | apPass.c_str()));
    staSsid = String((const char*)(d["wifi_sta_ssid"] | ""));
    staPass = String((const char*)(d["wifi_sta_password"] | ""));
    apiUrl = String((const char*)(d["api_url"] | ""));
    intervalMs = (uint32_t)(d["upload_interval"] | 0);
  }

  // --- AP+STA idle; STA connect later or now if configured
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.mode(WIFI_AP_STA);
  // Ensure default AP network is correct
  WiFi.softAPConfig(IPAddress(192,168,4,1), IPAddress(192,168,4,1), IPAddress(255,255,255,0));
  bool apok = WiFi.softAP(apSsid.c_str(), apPass.c_str());
  Serial.printf("[AP] %s started IP: %s\n", apok?"":"NOT ", WiFi.softAPIP().toString().c_str());
  dnsServer.start(53, "*", WiFi.softAPIP());

  // Bring up HTTP API ASAP for field diagnosis
  // Minimal infra first: in-memory repo and net client
  LogRepo* repo = makeMemLogRepo();
  repo->ensureReady();
  auto* https = makeNetClientHttps();
  static UploaderService up(*repo, *https);
  {
    UploadCfg cfg; cfg.api = ""; cfg.interval_ms = 15000; cfg.batch_size = 10; up.set(cfg);
  }
  static HttpApi api(*repo, up);
  api.begin();                              // serves UI + REST early

  // Proceed with the rest of peripherals after HTTP is up
  // I2C (RTC)
  Wire.begin(21,22);

  // SPI buses
  // VSPI for SD (original wiring)
  // Ensure CS lines are inactive to prevent bus contention during SD init
  pinMode(13, OUTPUT); digitalWrite(13, HIGH); // SD CS
  pinMode(27, OUTPUT); digitalWrite(27, HIGH); // LoRa CS (even if LoRa uses HSPI in code)
  SPI.begin(18,19,23);             // VSPI: SCK=18, MISO=19, MOSI=23
  SDfs.begin(/*cs*/13, SPI);       // SD CS=D13 on VSPI
  // LoRa shares the VSPI bus (SCK=18, MISO=19, MOSI=23) with its own CS
  const bool useSdRepo = false; // toggle CSV log repo vs in-memory repo
  // Infra
  auto* rtc   = makeRtcDs3231();   rtc->begin();
  auto* lora  = makeLoRaPortArduino(27,25,26,&SPI,433E6); // LoRa on shared VSPI
  static ConfigStore cfgStore;             // LittleFS-backed config for WiFi

  // Tasks
  // Start services (LoRa uses repo/net already constructed)
  static LoraRxService rx(*lora, *repo, *rtc);
  xTaskCreate([](void*){ rx.begin(); rx.taskLoop(); }, "lora_rx", 4096, nullptr, 1, nullptr);
  // Do not start uploader task here; it will be created on-demand via up.ensureTask() when /api/upload/start is called.

  // Optional: attempt STA connect at startup if credentials present
  if (staSsid.length() && staPass.length()){
    struct BootParams { String staSsid; String staPass; String apSsid; String apPass; };
    auto* boot = new BootParams{ staSsid, staPass, apSsid, apPass };
    xTaskCreate([](void* param){
      BootParams* b = (BootParams*)param;
      String ssid = b->staSsid; String pass = b->staPass; String apS = b->apSsid; String apP = b->apPass;
      delete b;
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      WiFi.disconnect(false);
      vTaskDelay(pdMS_TO_TICKS(100));
      Serial.printf("[BOOT] Auto connect STA '%s'\n", ssid.c_str());
      WiFi.begin(ssid.c_str(), pass.c_str());
      wl_status_t r = (wl_status_t)WiFi.waitForConnectResult(20000);
      Serial.printf("[BOOT] STA result=%d status=%d\n", (int)r, (int)WiFi.status());
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP(apS.c_str(), apP.c_str());
      vTaskDelete(nullptr);
    }, "sta_boot", 4096, boot, 1, nullptr);
  }
  // Start uploader if configured
  if (apiUrl.length() && intervalMs>0){ UploadCfg uc; uc.api=apiUrl.c_str(); uc.interval_ms=intervalMs; uc.batch_size=50; up.set(uc); }
}

void loop(){
  // Avoid UDP pressure when STA is connected; only serve DNS in AP-only mode
  if (WiFi.status() != WL_CONNECTED) {
    dnsServer.processNextRequest();
  }
  vTaskDelay(pdMS_TO_TICKS(10));
}
