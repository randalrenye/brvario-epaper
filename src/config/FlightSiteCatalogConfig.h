#pragma once

#ifndef FLIGHT_SITE_CATALOG_URL
#define FLIGHT_SITE_CATALOG_URL ""
#endif

namespace FlightSiteCatalogConfig {

static constexpr const char* kCatalogUrl = FLIGHT_SITE_CATALOG_URL;
static constexpr bool kAllowInsecureTls = true;
static constexpr unsigned long kWifiWaitTimeoutMs = 22000UL;
static constexpr unsigned long kHttpTimeoutMs = 20000UL;
static constexpr unsigned long kNoDataTimeoutMs = 12000UL;
static constexpr uint32_t kMaximumCatalogBytes = 512UL * 1024UL;

}  // namespace FlightSiteCatalogConfig
