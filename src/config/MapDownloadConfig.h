#pragma once

// Configure later in platformio.ini, for example:
// -DMAP_DOWNLOAD_BASE_URL=\"https://raw.githubusercontent.com/usuario/repositorio/main/maps\"
#ifndef MAP_DOWNLOAD_BASE_URL
#define MAP_DOWNLOAD_BASE_URL ""
#endif

namespace MapDownloadConfig {

static constexpr const char* kBaseUrl = MAP_DOWNLOAD_BASE_URL;
static constexpr bool kAllowInsecureTls = true;
static constexpr unsigned long kWifiWaitTimeoutMs = 20000UL;
static constexpr unsigned long kHttpTimeoutMs = 20000UL;
static constexpr unsigned long kNoDataTimeoutMs = 12000UL;

}  // namespace MapDownloadConfig
