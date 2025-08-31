#include "wifi_manager.h"

static WifiManager* g_self = nullptr;

void WifiManager::begin(){
  g_self = this;
  WiFi.mode(WIFI_AP_STA);
  ensureApUp();
  WiFi.onEvent(onEventThunk);
}

void WifiManager::setAp(const String& ssid, const String& pass){
  ap_ssid_ = ssid; ap_pass_ = pass;
  ensureApUp();
}

void WifiManager::ensureApUp(){
  if (!WiFi.softAPgetStationNum() && WiFi.softAPSSID() != ap_ssid_) {
    WiFi.softAP(ap_ssid_.c_str(), ap_pass_.c_str());
  }
}

void WifiManager::connectSta(const String& ssid, const String& pass){
  // Spawn background task to avoid blocking AsyncWebServer
  struct Cfg { String s; String p; WifiManager* self; };
  auto* cfg = new Cfg{ ssid, pass, this };
  auto worker = +[](void* arg){
    std::unique_ptr<Cfg> c((Cfg*)arg);
    WifiManager* self = c->self;

    self->connecting_ = true;
    self->connected_ = false;
    self->last_attempt_ms_ = millis();
    self->last_result_ = -1;

    // WiFi stack hygiene
    WiFi.persistent(false);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);
    // Allow channels 1..13 in AP+STA
    wifi_country_t country = {"CN", 1, 13, 0, WIFI_COUNTRY_POLICY_MANUAL};
    esp_wifi_set_country(&country);

    // Manage AP during connect to avoid interference
    bool apWasRunning = WiFi.softAPSSID().length() > 0;
    if (apWasRunning && self->pause_ap_) {
      WiFi.softAPdisconnect(true);
    }

    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect(false);
    vTaskDelay(pdMS_TO_TICKS(100));
    Serial.printf("[WIFI] STA connect to '%s' (pwlen=%u)\n", c->s.c_str(), (unsigned)c->p.length());
    WiFi.begin(c->s.c_str(), c->p.c_str());
    wl_status_t res = (wl_status_t)WiFi.waitForConnectResult(30000);
    self->last_result_ = (int)res;
    self->status_ = WiFi.status();

    if (res == WL_CONNECTED){
      self->connected_ = true;
      WiFi.setAutoReconnect(true);
      // Keep AP off or bring it back on STA channel
      if (self->pause_ap_) {
        WiFi.softAP(self->ap_ssid_.c_str(), self->ap_pass_.c_str());
      }
    } else {
      self->connected_ = false;
      if (self->pause_ap_) {
        // Restore AP so UI is reachable
        WiFi.softAP(self->ap_ssid_.c_str(), self->ap_pass_.c_str());
      }
    }

    self->connecting_ = false;
    vTaskDelete(nullptr);
  };
  xTaskCreate(worker, "wifi_sta_connect", 4096, cfg, 1, nullptr);
}

WifiStatus WifiManager::get() const{
  WifiStatus s;
  s.connected = connected_;
  s.connecting = connecting_ || (!connected_ && (millis() - last_attempt_ms_) < 30000);
  s.status = (wl_status_t)status_;
  s.last_attempt_ms = last_attempt_ms_;
  s.last_result = last_result_;
  s.disc_reason = disc_reason_;
  return s;
}

int WifiManager::scanNetworks(bool includeHidden){
  return WiFi.scanNetworks(false, includeHidden);
}

void WifiManager::onEventThunk(WiFiEvent_t event, WiFiEventInfo_t info){ if (g_self) g_self->onEvent(event, info); }

void WifiManager::onEvent(WiFiEvent_t event, WiFiEventInfo_t info){
  switch(event){
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      connected_ = false; // wait for IP
      Serial.println("[WIFI] STA_CONNECTED");
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      connected_ = true;
      status_ = WL_CONNECTED;
      last_result_ = (int)WL_CONNECTED;
      Serial.printf("[WIFI] GOT_IP: %s\n", WiFi.localIP().toString().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      connected_ = false;
      disc_reason_ = info.wifi_sta_disconnected.reason;
      Serial.printf("[WIFI] DISCONNECTED reason=%u\n", (unsigned)disc_reason_);
      break;
    default: break;
  }
}

