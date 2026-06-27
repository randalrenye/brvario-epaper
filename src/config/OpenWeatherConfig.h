#pragma once

// Keep the key centralized. It can be defined in platformio.ini build_flags as:
//   -DOPENWEATHER_API_KEY=sua_chave_aqui
// or:
//   -DOPENWEATHER_API_KEY=\"sua_chave_aqui\"
// Leaving it undefined makes the weather page show a configuration warning.
#define OPENWEATHER_STRINGIFY_DETAIL(value) #value
#define OPENWEATHER_STRINGIFY(value) OPENWEATHER_STRINGIFY_DETAIL(value)

#ifndef OPENWEATHER_API_KEY
#define OPENWEATHER_API_KEY_TEXT ""
#else
#define OPENWEATHER_API_KEY_TEXT OPENWEATHER_STRINGIFY(OPENWEATHER_API_KEY)
#endif

namespace OpenWeatherConfig {

static constexpr const char* kApiKey = OPENWEATHER_API_KEY_TEXT;
static constexpr const char* kLanguage = "pt_br";
static constexpr const char* kUnits = "metric";
static constexpr uint32_t kWifiWaitTimeoutMs = 22000UL;
static constexpr uint32_t kHttpTimeoutMs = 6500UL;
static constexpr bool kPreferLocalPressure = true;

}  // namespace OpenWeatherConfig
