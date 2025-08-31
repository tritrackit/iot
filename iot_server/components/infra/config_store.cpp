#include "config_store.h"
#include <ArduinoJson.h>

static const char* WIFI_JSON = "/wifi.json"; // stored on LittleFS

bool ConfigStore::saveWifi(const WifiCfg& c){
  File f = LittleFS.open(WIFI_JSON, "w");
  if(!f) return false;
  StaticJsonDocument<256> doc;
  doc["ssid"] = c.ssid;
  doc["pass"] = c.pass;
  doc["autoconnect"] = c.autoconnect;
  serializeJson(doc, f);
  f.close(); return true;
}

bool ConfigStore::loadWifi(WifiCfg& c){
  File f = LittleFS.open(WIFI_JSON, "r");
  if(!f) return false;
  StaticJsonDocument<256> doc;
  auto err = deserializeJson(doc, f);
  f.close();
  if(err) return false;
  c.ssid = doc["ssid"] | "";
  c.pass = doc["pass"] | "";
  c.autoconnect = doc["autoconnect"] | false;
  return true;
}
