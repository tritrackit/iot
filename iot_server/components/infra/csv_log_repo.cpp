// components/infra/csv_log_repo.cpp
#include "log_repo.h"
#include "sd_fs.h"
#include <Arduino.h>
#include <unordered_set>
#include <string>

static const char* LOGS = "/sd/logs.csv";

class CsvLogRepo : public LogRepo {
  SdFs& fs_;
public:
  explicit CsvLogRepo(SdFs& fs): fs_(fs) {}

  bool ensureReady() override {
    if (fs_.exists(LOGS)) return true;
    File f = fs_.open(LOGS, "w"); if (!f) return false;
    f.printf("S-BOOT,INIT,%s,0,\n","1970-01-01 00:00:00");
    f.close();
    return true;
  }

  bool append(const domain::LogEntry& e) override {
    File f = fs_.open(LOGS, "a"); if(!f) return false;
    bool ok = f.printf("%s,%s,%s,%d,%s\n",
                       e.scanner_id.c_str(),
                       e.rfid.c_str(),
                       e.ts_iso.c_str(),
                       e.sent ? 1 : 0,
                       e.message.c_str()) > 0;
    f.close();
    return ok;
  }

  std::vector<domain::LogEntry> listAll(size_t maxN) override {
    std::vector<domain::LogEntry> out;
    File f = fs_.open(LOGS,"r"); if(!f) return out;
    while (f.available() && out.size() < maxN) {
      String line = f.readStringUntil('\n'); line.trim(); if (!line.length()) continue;
      int c1=line.indexOf(','), c2=line.indexOf(',',c1+1), c3=line.indexOf(',',c2+1), c4=line.indexOf(',',c3+1);
      if (c1<0 || c2<0) continue;
      domain::LogEntry e;
      e.scanner_id = line.substring(0,c1).c_str();
      e.rfid       = line.substring(c1+1,c2).c_str();
      if (c3<0) {
        // legacy 3 columns -> ts only
        e.ts_iso = line.substring(c2+1).c_str();
        e.sent = false;
        e.message.clear();
      } else if (c4<0) {
        // legacy 4 columns -> ts, sent
        e.ts_iso = line.substring(c2+1,c3).c_str();
        e.sent   = (line.substring(c3+1).toInt()!=0);
        e.message.clear();
      } else {
        // 5 columns -> ts, sent, message
        e.ts_iso  = line.substring(c2+1,c3).c_str();
        e.sent    = (line.substring(c3+1,c4).toInt()!=0);
        e.message = line.substring(c4+1).c_str();
      }
      out.push_back(e);
    }
    f.close();
    return out;
  }

  std::vector<domain::LogEntry> listUnsent(size_t limit) override {
    std::vector<domain::LogEntry> out;
    File f = fs_.open(LOGS,"r"); if(!f) return out;
    while (f.available() && out.size() < limit) {
      String line = f.readStringUntil('\n'); line.trim(); if (!line.length()) continue;
      int c1=line.indexOf(','), c2=line.indexOf(',',c1+1), c3=line.indexOf(',',c2+1), c4=line.indexOf(',',c3+1);
      if (c1<0 || c2<0) continue;
      bool sent;
      if (c3<0) sent = false;                              // legacy 3 columns
      else if (c4<0) sent = (line.substring(c3+1).toInt()!=0); // legacy 4 columns
      else sent = (line.substring(c3+1,c4).toInt()!=0);        // 5 columns
      if (sent) continue;
      domain::LogEntry e;
      e.scanner_id = line.substring(0,c1).c_str();
      e.rfid       = line.substring(c1+1,c2).c_str();
      if (c3<0) {
        e.ts_iso = line.substring(c2+1).c_str();
        e.message.clear();
      } else if (c4<0) {
        e.ts_iso = line.substring(c2+1,c3).c_str();
        e.message.clear();
      } else {
        e.ts_iso  = line.substring(c2+1,c3).c_str();
        e.message = line.substring(c4+1).c_str();
      }
      e.sent       = false;
      out.push_back(e);
    }
    f.close();
    return out;
  }

  bool markSent(const std::vector<domain::LogEntry>& sent) override {
    // use std::string keys
    std::unordered_set<std::string> keys;
    keys.reserve(sent.size());
    for (const auto& e : sent) {
      keys.insert(e.scanner_id + "|" + e.rfid + "|" + e.ts_iso);
    }

    File in = fs_.open(LOGS,"r"); if(!in) return false;
    File out = fs_.open("/logs.tmp","w"); if(!out){ in.close(); return false; }

    while (in.available()) {
      String raw = in.readStringUntil('\n');
      String line = raw; line.trim();
      if (!line.length()) { out.print(raw); continue; }

      int c1=line.indexOf(','), c2=line.indexOf(',',c1+1), c3=line.indexOf(',',c2+1), c4=line.indexOf(',',c3+1);
      if (c1<0 || c2<0) { out.print(raw); continue; }

      String s=line.substring(0,c1);
      String r=line.substring(c1+1,c2);
      String t=(c3<0? line.substring(c2+1): line.substring(c2+1,c3));

      std::string key = std::string(s.c_str()) + "|" + r.c_str() + "|" + t.c_str();
      bool isTarget = keys.find(key) != keys.end();
      int sentFlag;
      String msg;
      if (isTarget) {
        sentFlag = 1;
        msg = ""; // clear message on success
      } else {
        if (c3<0) { sentFlag = 0; msg = ""; }
        else if (c4<0) { sentFlag = line.substring(c3+1).toInt(); msg = ""; }
        else { sentFlag = line.substring(c3+1,c4).toInt(); msg = line.substring(c4+1); }
      }

      out.printf("%s,%s,%s,%d,%s\n", s.c_str(), r.c_str(), t.c_str(), sentFlag, msg.c_str());
    }

    in.close(); out.close();
    fs_.remove(LOGS);
    return fs_.rename("/logs.tmp", LOGS);
  }

  bool markFailed(const std::vector<domain::LogEntry>& failed, const std::string& message) override {
    std::unordered_set<std::string> keys;
    keys.reserve(failed.size());
    for (const auto& e : failed) {
      keys.insert(e.scanner_id + std::string("|") + e.rfid + std::string("|") + e.ts_iso);
    }

    File in = fs_.open(LOGS,"r"); if(!in) return false;
    File out = fs_.open("/logs.tmp","w"); if(!out){ in.close(); return false; }

    while (in.available()) {
      String raw = in.readStringUntil('\n');
      String line = raw; line.trim();
      if (!line.length()) { out.print(raw); continue; }

      int c1=line.indexOf(','), c2=line.indexOf(',',c1+1), c3=line.indexOf(',',c2+1), c4=line.indexOf(',',c3+1);
      if (c1<0 || c2<0) { out.print(raw); continue; }

      String s=line.substring(0,c1);
      String r=line.substring(c1+1,c2);
      String t=(c3<0? line.substring(c2+1): line.substring(c2+1,c3));

      std::string key = std::string(s.c_str()) + "|" + r.c_str() + "|" + t.c_str();
      bool isTarget = keys.find(key) != keys.end();

      int sentFlag;
      String msg;
      if (isTarget) {
        sentFlag = 0; // keep unsent on failure
        msg = message.c_str();
      } else {
        if (c3<0) { sentFlag = 0; msg = ""; }
        else if (c4<0) { sentFlag = line.substring(c3+1).toInt(); msg = ""; }
        else { sentFlag = line.substring(c3+1,c4).toInt(); msg = line.substring(c4+1); }
      }

      out.printf("%s,%s,%s,%d,%s\n", s.c_str(), r.c_str(), t.c_str(), sentFlag, msg.c_str());
    }

    in.close(); out.close();
    fs_.remove(LOGS);
    return fs_.rename("/logs.tmp", LOGS);
  }
};

// factory
LogRepo* makeCsvLogRepo(SdFs& fs) { return new CsvLogRepo(fs); }
