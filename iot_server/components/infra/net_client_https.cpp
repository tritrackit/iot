#include "net_client.h"
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <HTTPClient.h>

class NetClientHttps : public NetClient {
public:
  bool postJson(const std::string& url, const std::string& json,
                int& code, std::string& resp,
                const std::string& apiKey = std::string()) override {
    HTTPClient http;
    WiFiClientSecure client;
    client.setInsecure(); // or setCACert(...)
    client.setTimeout(7000);       // read timeout
    http.setTimeout(7000);         // total transfer timeout
    String surl = url.c_str();
    surl.toLowerCase();
    bool isHttps = surl.startsWith("https://");
    bool okBegin = false;
    if (isHttps){
      WiFiClientSecure c; c.setInsecure();
      okBegin = http.begin(c, url.c_str());
    } else {
      // Plain HTTP
      okBegin = http.begin(url.c_str());
    }
    if (!okBegin) return false;
    http.setTimeout(8000);
    http.setReuse(false);            // disable keep-alive to avoid socket accumulation
    http.useHTTP10(true);            // force HTTP/1.0 so connection closes after response
    http.addHeader("Content-Type","application/json");
    http.addHeader("Accept","*/*");
    http.addHeader("Connection","close");
    if (!apiKey.empty()){
      http.addHeader("X-API-Key", apiKey.c_str());
    }
    code = http.POST((uint8_t*)json.data(), json.size());
    resp = http.getString().c_str();
    http.end();
    return (code > 0);
  }
};

NetClient* makeNetClientHttps(){ return new NetClientHttps(); }
