#pragma once
#include <string>
#include <LittleFS.h>

struct WifiCfg {
  std::string ssid;
  std::string pass;
  bool        autoconnect = false;
};

class ConfigStore {
public:
  ConfigStore() = default;
  bool saveWifi(const WifiCfg& c);
  bool loadWifi(WifiCfg& c);
};
