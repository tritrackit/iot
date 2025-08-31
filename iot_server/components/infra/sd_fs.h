#pragma once
#include <SD.h>
#include <SPI.h>
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "spi_lock.h"

class SdFs {
public:
  virtual ~SdFs() = default;
  virtual bool begin(uint8_t csPin, SPIClass& spi) = 0;
  virtual bool exists(const char* path) = 0;
  virtual File open(const char* path, const char* mode) = 0;
  virtual bool remove(const char* path) = 0;
  virtual bool rename(const char* from, const char* to) = 0;
  // Convenience helpers that fully read/write under internal locking
  virtual bool readAll(const char* path, String& out) = 0;
  virtual bool writeAll(const char* path, const String& data) = 0;
  virtual bool isDir(const char* path) = 0;
  // Expose coarse lock for multi-step operations (RAII recommended where possible)
  virtual void lock() = 0;
  virtual void unlock() = 0;
};

class SdFsImpl : public SdFs {
public:
  SdFsImpl(){ mtx_ = xSemaphoreCreateMutex(); }
  bool begin(uint8_t csPin, SPIClass& spi) override {
    // Try slower SPI for better signal integrity and explicit mount point
    csPin_ = csPin; spi_ = &spi;
    for (int i=0;i<3 && !mounted_; ++i){
      spi_lock();
      mounted_ = SD.begin(csPin, spi, (i==0? kSpiHz : 1000000 /*1MHz*/), kMountPoint);
      spi_unlock();
      if (!mounted_) delay(50);
    }
    fail_count_ = 0; reattempts_ = 0;
    return mounted_;
  }
  bool exists(const char* path) override {
    LockGuard g(*this);
    if (!ensureMounted()) return false;
    // A missing file is not a bus failure
    bool present = SD.exists(path);
    onOk();
    return present;
  }
  File open(const char* path, const char* mode) override {
    LockGuard g(*this);
    if (!ensureMounted()) return File();
    File f = SD.open(path, mode);
    if (!f){
      bool writeLike = (mode && (strchr(mode,'w') || strchr(mode,'a') || strchr(mode,'+')));
      if (writeLike){ onFail(); }
      else { onOk(); }
    } else {
      onOk();
    }
    return f;
  }
  bool remove(const char* path) override {
    LockGuard g(*this);
    if (!ensureMounted()) return false;
    bool present = SD.exists(path);
    bool ok = present ? SD.remove(path) : false;
    if (!ok) onFail(); else onOk();
    return ok;
  }
  bool rename(const char* from, const char* to) override {
    LockGuard g(*this);
    if (!ensureMounted()) return false;
    bool present = SD.exists(from);
    bool ok = present ? SD.rename(from, to) : false;
    if (!ok) onFail(); else onOk();
    return ok;
  }
  bool isMounted() const { return mounted_; }
  // Helpers
  bool readAll(const char* path, String& out) override {
    LockGuard g(*this);
    if (!ensureMounted()) return false;
    File f = SD.open(path, "r");
    if (!f) { onFail(); return false; }
    size_t sz = f.size(); if (sz>0 && sz<(256*1024)) out.reserve(sz+1);
    out = "";
    while (f.available()) { out += (char)f.read(); }
    f.close(); onOk(); return true;
  }
  bool writeAll(const char* path, const String& data) override {
    LockGuard g(*this);
    if (!ensureMounted()) return false;
    File f = SD.open(path, "w");
    if (!f) { onFail(); return false; }
    size_t n = f.print(data);
    f.flush(); f.close();
    if (n != data.length()) { onFail(); return false; }
    onOk(); return true;
  }
  bool isDir(const char* path) override {
    LockGuard g(*this);
    if (!ensureMounted()) return false;
    File f = SD.open(path, "r");
    if (!f) { onFail(); return false; }
    bool d = f.isDirectory(); f.close(); onOk(); return d;
  }
  void lock() override { if (mtx_) xSemaphoreTake(mtx_, portMAX_DELAY); spi_lock(); }
  void unlock() override { spi_unlock(); if (mtx_) xSemaphoreGive(mtx_); }
private:
  static constexpr uint32_t kSpiHz = 4000000; // 4 MHz
  static constexpr const char* kMountPoint = "/sd";
  bool mounted_ = false;
  int  fail_count_ = 0;
  int  reattempts_ = 0;
  uint8_t csPin_ = 0;
  SPIClass* spi_ = nullptr;
  SemaphoreHandle_t mtx_ = nullptr;
  struct LockGuard { SdFsImpl& s; LockGuard(SdFsImpl& s_):s(s_){ s.lock(); } ~LockGuard(){ s.unlock(); } };
  bool ensureMounted(){
    if (mounted_) return true;
    if (!spi_) return false;
    mounted_ = SD.begin(csPin_, *spi_, kSpiHz, kMountPoint);
    if (mounted_) fail_count_ = 0; // reset after successful remount
    return mounted_;
  }
  void onFail(){
    // Don't immediately disable; try bounded re-mounts
    if (!mounted_) return; // will be handled by ensureMounted
    if (++fail_count_ >= 3){
      SD.end();
      mounted_ = false;
      fail_count_ = 0;
      if (reattempts_ < 2){
        reattempts_++;
        mounted_ = SD.begin(csPin_, *spi_, kSpiHz, kMountPoint);
        if (mounted_) {
          Serial.println("[SD] re-mounted after transient failures");
          fail_count_ = 0;
        } else {
          Serial.println("[SD] mount failed; will require manual intervention");
        }
      } else {
        Serial.println("[SD] too many failures; disabling SD access");
      }
    }
  }
  void onOk(){ fail_count_ = 0; }
};
