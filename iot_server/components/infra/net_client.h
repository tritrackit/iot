#pragma once
#include <string>
class NetClient {
public:
  virtual ~NetClient() = default;
  virtual bool postJson(const std::string& url,
                        const std::string& json,
                        int& code,
                        std::string& resp,
                        const std::string& apiKey = std::string()) = 0;
};
NetClient* makeNetClientHttps();
