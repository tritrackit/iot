#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <esp_wifi.h>

struct WifiStatus {
  bool connected = false;
  bool connecting = false;
  wl_status_t status = WL_DISCONNECTED;
  uint32_t last_attempt_ms = 0;
  int last_result = -1; // wl_status_t or -1
  uint8_t disc_reason = 0; // wifi_err_reason_t
};

class WifiManager {
public:
  void begin();
  void setAp(const String& ssid, const String& pass);
  void setPauseApDuringConnect(bool on) { pause_ap_ = on; }

  void connectSta(const String& ssid, const String& pass);
  WifiStatus get() const;

  // Utility: scan APs (sync)
  int scanNetworks(bool includeHidden);
private:
  static void onEventThunk(WiFiEvent_t event, WiFiEventInfo_t info);
  void onEvent(WiFiEvent_t event, WiFiEventInfo_t info);

  void ensureApUp();

  volatile bool connected_ = false;
  volatile bool connecting_ = false;
  volatile wl_status_t status_ = WL_DISCONNECTED;
  volatile uint32_t last_attempt_ms_ = 0;
  volatile int last_result_ = -1;
  volatile uint8_t disc_reason_ = 0;

  String ap_ssid_ = "Device-Setup";
  String ap_pass_ = "12345678";
  bool   pause_ap_ = true;
};

