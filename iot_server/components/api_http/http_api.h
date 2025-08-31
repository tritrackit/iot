#pragma once
#include "infra/log_repo.h"
#include "services/uploader_service.h"

class HttpApi {
  LogRepo&        repo_;
  UploaderService& up_;
public:
  HttpApi(LogRepo& r, UploaderService& u) : repo_(r), up_(u) {}
  bool begin();
};
