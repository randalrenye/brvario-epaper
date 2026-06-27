#include "MainScreen.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <LittleFS.h>
#include <Preferences.h>

#include "audio/VarioBuzzer.h"
#include "assets/BrvarioBootLogo.h"
#include "assets/BrvarioParagliderIcon.h"
#include "config/AppConfig.h"
#include "config/PilotProfileConfig.h"
#include "ble/TracklogBleService.h"
#include "map/OfflineMapPackage.h"
#include "network/FirmwareUpdater.h"
#include "network/FlightSiteCatalogUpdater.h"
#include "network/MapDownloadManager.h"
#include "network/OpenWeatherClient.h"
#include "network/WifiManager.h"
#include "storage/StorageManager.h"
#include "tracklog/FlightRecorder.h"
#include "weather/FlightSiteCatalog.h"
#include "weather/WeatherLocationManager.h"

namespace {

// Disabled during bench/programming tests. When re-enabling, keep the popup centered
// so it does not cover the main controls while GPS is acquiring fix.
static constexpr bool kGpsNoFixPopupEnabled = false;
static constexpr bool kTracklogBleTransferEnabled = true;
static constexpr uint8_t kWeatherInfoPageCount = 4;
static constexpr uint8_t kThermalInfoPageCount = 3;
static constexpr uint8_t kThermalCycleInfoPageCount = 5;
static constexpr uint8_t kManualPageCount = 5;
static constexpr uint8_t kManualVisibleLineCount = 8;
static constexpr uint8_t kManualScrollStepLines = 3;
static constexpr int32_t kCenterMetricReservedHeight = 58;
static constexpr uint32_t kStartupMapGpsWaitMs = 2500UL;
static constexpr uint32_t kStartupOverlayRefreshMs = 500UL;
static constexpr uint32_t kKeyboardSpaceRepeatGuardMs = 280UL;
static constexpr int32_t kPageDragMinDistancePx = 34;
static constexpr uint32_t kIdlePageFullRefreshMs = 15UL * 60UL * 1000UL;
static constexpr uint32_t kThermalCyclePageFullRefreshMs = 10UL * 60UL * 1000UL;
static constexpr uint32_t kThermalCyclePagePartialRefreshMs = 5000UL;
static constexpr uint32_t kFooterSoftCleanIntervalMs = 10UL * 60UL * 1000UL;
static constexpr uint8_t kFooterInk = AppConfig::kMid;
static constexpr uint8_t kFooterRuleInk = AppConfig::kMid;
static constexpr int32_t kSettingsButtonW = 390;
static constexpr int32_t kSettingsButtonH = 40;
static constexpr int32_t kSettingsButtonTopY = 104;
static constexpr int32_t kSettingsButtonStepY = 48;
static constexpr int32_t kSettingsButtonPadX = 10;
static constexpr int32_t kSettingsButtonPadY = 4;
static constexpr char kAdvancedIgcSdDir[] = "/brvario/igc";
static constexpr uint8_t kWeatherCatalogRowsPerPage = 4;
static constexpr const char* kBrazilStates[] = {
    "",   "AC", "AL", "AP", "AM", "BA", "CE",
    "DF", "ES", "GO", "MA", "MT", "MS", "MG",
    "PA", "PB", "PR", "PE", "PI", "RJ", "RN",
    "RS", "RO", "RR", "SC", "SP", "SE", "TO",
};
static constexpr uint8_t kBrazilStateCount = sizeof(kBrazilStates) / sizeof(kBrazilStates[0]);

char foldedPortugueseChar(const char*& text) {
  const uint8_t first = static_cast<uint8_t>(*text++);
  if (first < 0x80) {
    return first >= 'a' && first <= 'z' ? static_cast<char>(first - ('a' - 'A')) : static_cast<char>(first);
  }
  if (first != 0xC3 || *text == '\0') {
    return '\0';
  }
  const uint8_t second = static_cast<uint8_t>(*text++);
  switch (second) {
    case 0x80:
    case 0x81:
    case 0x82:
    case 0x83:
    case 0x84:
    case 0x85:
    case 0xA0:
    case 0xA1:
    case 0xA2:
    case 0xA3:
    case 0xA4:
    case 0xA5:
      return 'A';
    case 0x87:
    case 0xA7:
      return 'C';
    case 0x88:
    case 0x89:
    case 0x8A:
    case 0x8B:
    case 0xA8:
    case 0xA9:
    case 0xAA:
    case 0xAB:
      return 'E';
    case 0x8C:
    case 0x8D:
    case 0x8E:
    case 0x8F:
    case 0xAC:
    case 0xAD:
    case 0xAE:
    case 0xAF:
      return 'I';
    case 0x92:
    case 0x93:
    case 0x94:
    case 0x95:
    case 0x96:
    case 0xB2:
    case 0xB3:
    case 0xB4:
    case 0xB5:
    case 0xB6:
      return 'O';
    case 0x99:
    case 0x9A:
    case 0x9B:
    case 0x9C:
    case 0xB9:
    case 0xBA:
    case 0xBB:
    case 0xBC:
      return 'U';
    default:
      return '\0';
  }
}

void foldPortugueseText(const char* input, char* output, size_t outputSize) {
  if (!output || outputSize == 0) {
    return;
  }
  output[0] = '\0';
  if (!input) {
    return;
  }
  size_t used = 0;
  const char* cursor = input;
  while (*cursor != '\0' && used + 1 < outputSize) {
    const char folded = foldedPortugueseChar(cursor);
    if (folded != '\0') {
      output[used++] = folded;
    }
  }
  output[used] = '\0';
}

bool containsFoldedText(const char* text, const char* query) {
  if (!query || query[0] == '\0') {
    return true;
  }
  char folded[64];
  foldPortugueseText(text, folded, sizeof(folded));
  return strstr(folded, query) != nullptr;
}

void copyUtf8Prefix(const char* input, char* output, size_t outputSize, size_t maxCharacters) {
  if (!output || outputSize == 0) {
    return;
  }
  output[0] = '\0';
  if (!input) {
    return;
  }
  size_t written = 0;
  size_t characters = 0;
  const uint8_t* cursor = reinterpret_cast<const uint8_t*>(input);
  while (*cursor != 0 && characters < maxCharacters) {
    size_t bytes = 1;
    if ((*cursor & 0xE0) == 0xC0) {
      bytes = 2;
    } else if ((*cursor & 0xF0) == 0xE0) {
      bytes = 3;
    } else if ((*cursor & 0xF8) == 0xF0) {
      bytes = 4;
    }
    if (written + bytes + 1 > outputSize) {
      break;
    }
    bool complete = true;
    for (size_t i = 1; i < bytes; ++i) {
      if (cursor[i] == 0 || (cursor[i] & 0xC0) != 0x80) {
        complete = false;
        break;
      }
    }
    if (!complete) {
      break;
    }
    memcpy(output + written, cursor, bytes);
    written += bytes;
    cursor += bytes;
    ++characters;
  }
  output[written] = '\0';
}

uint8_t weatherWindSector(int degrees) {
  int normalized = degrees % 360;
  if (normalized < 0) {
    normalized += 360;
  }
  return static_cast<uint8_t>(((normalized + 22) / 45) % 8);
}

const char* weatherWindCompatibility(uint16_t acceptedQuadrants, int forecastDegrees) {
  if (acceptedQuadrants == 0) {
    return "NAO INFORMADA";
  }

  const uint8_t sector = weatherWindSector(forecastDegrees);
  const uint16_t forecastBit = static_cast<uint16_t>(1U << sector);
  if ((acceptedQuadrants & forecastBit) != 0) {
    return "FAVORAVEL";
  }

  const uint8_t previousSector = static_cast<uint8_t>((sector + 7) % 8);
  const uint8_t nextSector = static_cast<uint8_t>((sector + 1) % 8);
  const uint16_t adjacentBits =
      static_cast<uint16_t>((1U << previousSector) | (1U << nextSector));
  if ((acceptedQuadrants & adjacentBits) != 0) {
    return "MARGINAL";
  }

  return "DESFAVORAVEL";
}

bool clearPrefsNamespace(const char* ns) {
  Preferences prefs;
  if (!ns || !prefs.begin(ns, false)) {
    return false;
  }
  const bool ok = prefs.clear();
  prefs.end();
  return ok;
}

const char* advancedActionLabel(TouchAction action) {
  switch (action) {
    case TouchAction::AdvancedMoveIgcToSd:
      return "MOVER IGC PARA SD";
    case TouchAction::AdvancedClearWifi:
      return "LIMPAR WIFI";
    case TouchAction::AdvancedResetSettings:
      return "PADRAO CONFIG";
    case TouchAction::AdvancedClearWeatherCache:
      return "LIMPAR METEO";
    case TouchAction::AdvancedFormatSystem:
      return "FORMATAR SISTEMA";
    default:
      return "";
  }
}

struct ManualTextPage {
  const char* tab;
  const char* title;
  const char* const* lines;
  uint8_t lineCount;
};

const char* const kManualLogicGeneral[] = {
    "BRVARIO e um computador de voo para voo livre.",
    "A tela principal combina vario, altitude, vento,",
    "assistente termica, mapa, bateria, GPS e audio.",
    "O layout pode ser personalizado sem alterar sensores.",
    "Dashboard e mapa mantem audio e tracklog em voo.",
    "Paginas de ajuste pausam rotinas visuais pesadas.",
    "Ao detectar decolagem, o sistema volta ao modo voo.",
};

const char* const kManualLogicSensors[] = {
    "GPS fornece hora UTC, posiçao, rumo e velocidade solo.",
    "Barometro mede pressao e alimenta altitude/vario.",
    "O vario usa filtro para suavizar pressao e ruido.",
    "A tela atualiza em blocos; o audio usa dado filtrado.",
    "ALT GPS vem do receptor; ALT AGL usa referencia local.",
    "Bateria usa ADC e curva de tensao configuravel.",
    "Falhas de GPS e barometro aparecem no Status do Sistema.",
};

const char* const kManualLogicAudio[] = {
    "O som e o canal principal quando o piloto nao olha a tela.",
    "Subida gera beeps com cadencia e tom crescentes.",
    "Descida usa tom de alerta conforme o perfil escolhido.",
    "Editor de audio ajusta resposta, tom geral e volume.",
    "O icone de som desliga apenas audio de vario.",
    "Beep de toque e sons de sistema continuam ativos.",
};

const char* const kManualLogicWind[] = {
    "Vento local e estimado pelo GPS em setores de rumo.",
    "O algoritmo compara maior e menor velocidade por setor.",
    "Dados meteo podem preencher vento ate o calculo local.",
    "O calculo local substitui a previsao quando fica valido.",
    "Assistente termica marca onde voce encontrou subida.",
    "Ele começa a aprender quando voce gira subindo.",
    "O BRVARIO acompanha a termica empurrada pelo vento.",
    "Se a subida muda de lado, ele tenta recentrar sozinho.",
    "Historico mostra termicas recentes no mapa.",
    "NUCLEO % indica o quanto o centro parece confiavel.",
};

const char* const kManualLogicLogs[] = {
    "Tracklog IGC inicia automaticamente apos decolagem.",
    "A gravaçao inclui trecho anterior ao inicio confirmado.",
    "Pouso encerra o voo por velocidade, AGL e deslocamento.",
    "Voos ficam na memoria interna ou SD e exportam por BLE.",
    "Mapa offline usa cache e tiles no microSD.",
    "WiFi salva ate 10 redes e escolhe a melhor disponivel.",
    "Estaçao meteo combina API, GPS e sensor local.",
};

const char* const kManualUseStart[] = {
    "Ao ligar, aguarde GPS, barometro e cache do mapa.",
    "A tela principal mostra vario, altitude, vento e bateria.",
    "Rodape: logo volta ao inicio; som liga/desliga vario.",
    "Botao mapa abre navegaçao offline com widgets de voo.",
    "Botao tracklog abre voos IGC e sincronizaçao BLE.",
    "Botao configuraçao abre ajustes de piloto, audio e sistema.",
    "Se decolar em outra tela, o BRVARIO volta ao modo voo.",
    "Na pagina mapa, audio do vario e tracklog continuam ativos.",
};

const char* const kManualUseFlight[] = {
    "Assistente Termica mostra pontos de subida ao redor.",
    "Bolinhas maiores/escuras indicam melhor ascendencia.",
    "A seta aponta tendencia para o nucleo mais forte.",
    "NUCLEO % alto indica centro mais confiavel.",
    "Historico no mapa ajuda voltar a uma subida perdida.",
    "AG no historico indica a termica ativa/agora.",
    "O BRVARIO acompanha o deslocamento da termica sozinho.",
    "Voce nao precisa escolher modelo durante o voo.",
    "Use o circulo para ajustar o giro sem olhar numeros.",
    "RUMO/DIR VENTO mostra sua proa e vento estimado.",
    "O vento local melhora depois de curvas ou aproximaçao.",
    "Se nao houver dado confiavel, o vento fica oculto.",
    "GANHO mostra altura acumulada na termica atual.",
    "PLANEIO usa deslocamento GPS e perda de altitude.",
    "DURAÇAO VOO conta somente durante voo detectado.",
};

const char* const kManualUseLayout[] = {
    "Configuraçao > Personalizar Tela muda o widget central.",
    "Toque nos blocos para escolher esquerda, centro e direita.",
    "Centro e a area principal: vario, termica ou rumo.",
    "Altitudes e velocidades ficam nas laterais para leitura.",
    "Botao PADRAO restaura a distribuiçao recomendada.",
    "Configuraçao > Assistente Termica muda o modo visual.",
    "PILOTO NO CENTRO mostra o rastro ao redor do piloto.",
    "TERMICA NO CENTRO mostra deslocamento em relaçao ao nucleo.",
    "Historico e acompanhamento da termica sao automaticos.",
    "As escolhas ficam salvas apos reiniciar.",
};

const char* const kManualUseAudio[] = {
    "Configuraçao > Editor de audio ajusta o som do vario.",
    "RESPOSTA altera sensibilidade e cadencia dos beeps.",
    "TOM GERAL deixa o som mais grave ou mais agudo.",
    "Volume beep controla apenas subida/descida do vario.",
    "VOZ GPS liga/desliga aviso falado de GPS conectado.",
    "O icone de som no rodape silencia somente o vario.",
    "Beep de toque e sons de sistema continuam ativos.",
    "Toque em grave/agudo/volume para ouvir feedback.",
    "Teste em solo antes de voar para evitar som cansativo.",
};

const char* const kManualUseSystem[] = {
    "Tracklog IGC inicia automaticamente na decolagem.",
    "Voos menores que 1 minuto nao sao gravados.",
    "Pagina Tracklog lista arquivos LOCAL e SD.",
    "Exportar envia o voo selecionado por BLE.",
    "Sincronizar com app envia arquivos que faltam no app.",
    "WiFi salva ate 10 redes e escolhe a melhor visivel.",
    "Estaçao Meteo usa GPS, API e barometro local.",
    "Mapa usa microSD para tiles offline quando disponivel.",
    "Avançado recupera tela, limpa caches e move IGC.",
};

uint8_t lineCount(const char* const* lines, size_t bytes) {
  (void)lines;
  return static_cast<uint8_t>(bytes / sizeof(const char*));
}

ManualTextPage manualLogicText(uint8_t page) {
  switch (page) {
    case 0:
      return {"GERAL", "VISAO GERAL DO INSTRUMENTO", kManualLogicGeneral, lineCount(kManualLogicGeneral, sizeof(kManualLogicGeneral))};
    case 1:
      return {"SENSORES", "SENSORES, ALTITUDE E VARIO", kManualLogicSensors, lineCount(kManualLogicSensors, sizeof(kManualLogicSensors))};
    case 2:
      return {"AUDIO", "LOGICA DO AUDIO DO VARIO", kManualLogicAudio, lineCount(kManualLogicAudio, sizeof(kManualLogicAudio))};
    case 3:
      return {"VENTO", "VENTO E ASSISTENTE TERMICA", kManualLogicWind, lineCount(kManualLogicWind, sizeof(kManualLogicWind))};
    default:
      return {"LOGS", "TRACKLOG, MAPA, WIFI E METEO", kManualLogicLogs, lineCount(kManualLogicLogs, sizeof(kManualLogicLogs))};
  }
}

ManualTextPage manualUserText(uint8_t page) {
  switch (page) {
    case 0:
      return {"INICIO", "USO DA TELA PRINCIPAL", kManualUseStart, lineCount(kManualUseStart, sizeof(kManualUseStart))};
    case 1:
      return {"VOO", "ASSISTENTE TERMICA E VOO", kManualUseFlight, lineCount(kManualUseFlight, sizeof(kManualUseFlight))};
    case 2:
      return {"LAYOUT", "PERSONALIZAR A TELA", kManualUseLayout, lineCount(kManualUseLayout, sizeof(kManualUseLayout))};
    case 3:
      return {"AUDIO", "EDITOR DE AUDIO", kManualUseAudio, lineCount(kManualUseAudio, sizeof(kManualUseAudio))};
    default:
      return {"SISTEMA", "TRACKLOG, WIFI, MAPA E METEO", kManualUseSystem, lineCount(kManualUseSystem, sizeof(kManualUseSystem))};
  }
}

void pauseWifiBeforeBle(WifiManager* wifi) {
  if (!wifi || (!wifi->isEnabled() && !wifi->isConnected())) {
    return;
  }
  Serial.println("WiFi: pausado temporariamente para exportacao BLE.");
  wifi->end();
  delay(80);
}

void pauseBleBeforeWifi(TracklogBleService* ble) {
  if (!ble || (!ble->enabled() && !ble->connected() && !ble->activeTransfer())) {
    return;
  }
  Serial.println("BLE Tracklog: pausado para priorizar WiFi.");
  ble->end();
  delay(80);
}

void drawThickLine(EpdDisplay& display, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t color) {
  display.drawLine(x0, y0, x1, y1, color);
  display.drawLine(x0 + 1, y0, x1 + 1, y1, color);
  display.drawLine(x0, y0 + 1, x1, y1 + 1, color);
}

void drawIconStrokeLine(EpdDisplay& display, int32_t x0, int32_t y0, int32_t x1, int32_t y1, int32_t radius, uint8_t color) {
  const int32_t radiusSq = radius * radius;
  for (int32_t dx = -radius; dx <= radius; ++dx) {
    for (int32_t dy = -radius; dy <= radius; ++dy) {
      if (dx * dx + dy * dy <= radiusSq + radius) {
        display.drawLine(x0 + dx, y0 + dy, x1 + dx, y1 + dy, color);
      }
    }
  }
  display.fillCircle(x0, y0, radius, color);
  display.fillCircle(x1, y1, radius, color);
}

void drawIconArc(EpdDisplay& display,
                 int32_t cx,
                 int32_t cy,
                 int32_t radius,
                 float startDeg,
                 float endDeg,
                 int32_t strokeRadius,
                 uint8_t color) {
  static constexpr float kRadPerDeg = 0.01745329252F;
  const uint8_t steps = 12;
  int32_t prevX = cx + static_cast<int32_t>(cosf(startDeg * kRadPerDeg) * static_cast<float>(radius) + 0.5F);
  int32_t prevY = cy + static_cast<int32_t>(sinf(startDeg * kRadPerDeg) * static_cast<float>(radius) + 0.5F);
  for (uint8_t i = 1; i <= steps; ++i) {
    const float angle = startDeg + (endDeg - startDeg) * static_cast<float>(i) / static_cast<float>(steps);
    const int32_t x = cx + static_cast<int32_t>(cosf(angle * kRadPerDeg) * static_cast<float>(radius) + 0.5F);
    const int32_t y = cy + static_cast<int32_t>(sinf(angle * kRadPerDeg) * static_cast<float>(radius) + 0.5F);
    drawIconStrokeLine(display, prevX, prevY, x, y, strokeRadius, color);
    prevX = x;
    prevY = y;
  }
}

bool showThermalCoreMetric(const VarioData& data) {
  return data.thermalCoreConfidencePercent >= 25 && data.varioMs > 0.10F;
}

Rect_t settingsColumnButtonBounds(const Rect_t& screen, bool rightColumn, uint8_t row) {
  const int32_t x = rightColumn ? screen.x + screen.width - kSettingsButtonW - 58 : screen.x + 58;
  return {x, screen.y + kSettingsButtonTopY + kSettingsButtonStepY * static_cast<int32_t>(row), kSettingsButtonW, kSettingsButtonH};
}

Rect_t settingsButtonTouchBounds(const Rect_t& button) {
  return {button.x - kSettingsButtonPadX,
          button.y - kSettingsButtonPadY,
          button.width + kSettingsButtonPadX * 2,
          button.height + kSettingsButtonPadY * 2};
}

void drawPackedIcon4bpp(EpdDisplay& display, const uint8_t* pixels, int32_t width, int32_t height, int32_t x, int32_t y, uint8_t tint = 0xFF) {
  uint8_t* fb = display.framebuffer();
  if (!fb || !pixels) return;

  const int32_t stride = (width + 1) / 2;
  for (int32_t py = 0; py < height; ++py) {
    const int32_t screenY = y + py;
    if (screenY < 0 || screenY >= EPD_HEIGHT) continue;

    for (int32_t px = 0; px < width; ++px) {
      const int32_t screenX = x + px;
      if (screenX < 0 || screenX >= EPD_WIDTH) continue;

      const uint8_t packed = pixels[static_cast<size_t>(py) * stride + static_cast<size_t>(px / 2)];
      const uint8_t value = (px & 1) ? ((packed >> 4) & 0x0F) : (packed & 0x0F);
      if (value == 0x0F) continue;
      const uint8_t drawValue = tint == 0xFF ? value : tint;

      uint8_t& dst = fb[static_cast<size_t>(screenY) * EPD_WIDTH / 2 + screenX / 2];
      if (screenX & 1) {
        dst = (dst & 0x0F) | (drawValue << 4);
      } else {
        dst = (dst & 0xF0) | drawValue;
      }
    }
  }
}

void drawPackedIcon4bppScaled(EpdDisplay& display,
                              const uint8_t* pixels,
                              int32_t width,
                              int32_t height,
                              int32_t x,
                              int32_t y,
                              int32_t drawWidth,
                              int32_t drawHeight) {
  uint8_t* fb = display.framebuffer();
  if (!fb || !pixels || width <= 0 || height <= 0 || drawWidth <= 0 || drawHeight <= 0) return;

  const int32_t stride = (width + 1) / 2;
  for (int32_t dy = 0; dy < drawHeight; ++dy) {
    const int32_t screenY = y + dy;
    if (screenY < 0 || screenY >= EPD_HEIGHT) continue;
    const int32_t sy = (dy * height) / drawHeight;

    for (int32_t dx = 0; dx < drawWidth; ++dx) {
      const int32_t screenX = x + dx;
      if (screenX < 0 || screenX >= EPD_WIDTH) continue;
      const int32_t sx = (dx * width) / drawWidth;
      const uint8_t packed = pixels[static_cast<size_t>(sy) * stride + static_cast<size_t>(sx / 2)];
      const uint8_t value = (sx & 1) ? ((packed >> 4) & 0x0F) : (packed & 0x0F);
      if (value == 0x0F) continue;

      uint8_t& dst = fb[static_cast<size_t>(screenY) * EPD_WIDTH / 2 + screenX / 2];
      if (screenX & 1) {
        dst = (dst & 0x0F) | (value << 4);
      } else {
        dst = (dst & 0xF0) | value;
      }
    }
  }
}

void drawSettingsIcon(EpdDisplay& display, int32_t cx, int32_t cy, uint8_t color = AppConfig::kBlack) {
  const int32_t left = cx - 24;
  const int32_t right = cx + 24;
  const int32_t topY = cy - 15;
  const int32_t midY = cy;
  const int32_t botY = cy + 15;

  drawIconStrokeLine(display, left, topY, right, topY, 1, color);
  drawIconStrokeLine(display, left, midY, right, midY, 1, color);
  drawIconStrokeLine(display, left, botY, right, botY, 1, color);

  display.fillCircle(cx - 9, topY, 5, AppConfig::kWhite);
  display.drawCircle(cx - 9, topY, 5, color);
  display.drawCircle(cx - 9, topY, 4, color);
  display.fillCircle(cx + 12, midY, 5, AppConfig::kWhite);
  display.drawCircle(cx + 12, midY, 5, color);
  display.drawCircle(cx + 12, midY, 4, color);
  display.fillCircle(cx - 2, botY, 5, AppConfig::kWhite);
  display.drawCircle(cx - 2, botY, 5, color);
  display.drawCircle(cx - 2, botY, 4, color);
}

void drawPageIcon(EpdDisplay& display, int32_t cx, int32_t cy) {
  display.drawRect({cx - 18, cy - 18, 30, 34}, AppConfig::kBlack);
  display.drawRect({cx - 17, cy - 17, 28, 32}, AppConfig::kBlack);
  display.fillTriangle(cx + 2, cy - 18, cx + 12, cy - 8, cx + 2, cy - 8, AppConfig::kBlack);
  drawThickLine(display, cx - 11, cy + 1, cx + 6, cy + 1, AppConfig::kBlack);
  drawThickLine(display, cx - 11, cy + 9, cx + 6, cy + 9, AppConfig::kBlack);
}

void drawMapIcon(EpdDisplay& display, int32_t cx, int32_t cy, uint8_t color = AppConfig::kBlack) {
  const int32_t left = cx - 29;
  const int32_t top = cy - 18;
  const int32_t foldW = 19;
  const int32_t mapH = 36;

  display.drawLine(left, top + 5, left + foldW, top, color);
  display.drawLine(left + foldW, top, left + foldW * 2, top + 5, color);
  display.drawLine(left + foldW * 2, top + 5, left + foldW * 3, top, color);
  display.drawLine(left, top + 5, left, top + mapH, color);
  display.drawLine(left + foldW, top, left + foldW, top + mapH - 5, color);
  display.drawLine(left + foldW * 2, top + 5, left + foldW * 2, top + mapH, color);
  display.drawLine(left + foldW * 3, top, left + foldW * 3, top + mapH - 5, color);
  display.drawLine(left, top + mapH, left + foldW, top + mapH - 5, color);
  display.drawLine(left + foldW, top + mapH - 5, left + foldW * 2, top + mapH, color);
  display.drawLine(left + foldW * 2, top + mapH, left + foldW * 3, top + mapH - 5, color);

  drawThickLine(display, left + 5, top + 26, left + 16, top + 17, color);
  drawThickLine(display, left + 16, top + 17, left + 30, top + 24, color);
  drawThickLine(display, left + 30, top + 24, left + 45, top + 12, color);

  const int32_t pinX = cx + 13;
  const int32_t pinY = cy - 5;
  display.drawCircle(pinX, pinY, 10, color);
  display.drawCircle(pinX, pinY, 9, color);
  display.fillCircle(pinX, pinY, 3, color);
  display.fillTriangle(pinX - 8, pinY + 6, pinX + 8, pinY + 6, pinX, pinY + 21, color);
}

void drawBackIcon(EpdDisplay& display, int32_t cx, int32_t cy, uint8_t color = AppConfig::kBlack) {
  drawIconStrokeLine(display, cx + 12, cy - 18, cx - 11, cy, 1, color);
  drawIconStrokeLine(display, cx - 11, cy, cx + 12, cy + 18, 1, color);
}

void drawAudioIcon(EpdDisplay& display, int32_t cx, int32_t cy, bool enabled, uint8_t color = AppConfig::kBlack) {
  const int32_t leftX = cx - 28;
  const int32_t throatX = cx - 18;
  const int32_t coneX = cx + 1;
  drawThickLine(display, leftX, cy - 10, throatX, cy - 10, color);
  drawThickLine(display, leftX, cy - 10, leftX, cy + 10, color);
  drawThickLine(display, leftX, cy + 10, throatX, cy + 10, color);
  drawThickLine(display, throatX, cy - 10, coneX, cy - 20, color);
  drawThickLine(display, coneX, cy - 20, coneX, cy + 20, color);
  drawThickLine(display, coneX, cy + 20, throatX, cy + 10, color);
  if (enabled) {
    drawIconArc(display, cx + 2, cy, 11, -42.0F, 42.0F, 0, color);
    drawIconArc(display, cx + 2, cy, 19, -42.0F, 42.0F, 0, color);
    drawIconArc(display, cx + 2, cy, 27, -42.0F, 42.0F, 0, color);
  } else {
    drawThickLine(display, cx + 11, cy - 14, cx + 29, cy + 14, color);
    drawThickLine(display, cx + 29, cy - 14, cx + 11, cy + 14, color);
  }
}

void drawTracklogIcon(EpdDisplay& display, int32_t cx, int32_t cy, bool active, uint8_t color = AppConfig::kBlack) {
  drawThickLine(display, cx - 23, cy + 14, cx - 13, cy + 3, color);
  drawThickLine(display, cx - 13, cy + 3, cx - 1, cy + 8, color);
  drawThickLine(display, cx - 1, cy + 8, cx + 12, cy - 9, color);
  drawThickLine(display, cx + 12, cy - 9, cx + 23, cy - 15, color);
  display.fillCircle(cx - 23, cy + 14, 5, color);
  display.fillCircle(cx - 13, cy + 3, 4, active ? color : AppConfig::kWhite);
  display.drawCircle(cx - 13, cy + 3, 5, color);
  display.fillCircle(cx - 1, cy + 8, 4, active ? color : AppConfig::kWhite);
  display.drawCircle(cx - 1, cy + 8, 5, color);
  display.fillTriangle(cx + 23, cy - 15, cx + 12, cy - 10, cx + 18, cy - 2, color);
}

Rect_t footerButtonBounds(const Rect_t& footer, uint8_t slot) {
  static constexpr int32_t kHorizontalInset = 3;
  static constexpr int32_t kVerticalInset = 5;
  const int32_t buttonW = footer.width / 5;
  const int32_t x = footer.x + static_cast<int32_t>(slot) * buttonW;
  const int32_t w = slot == 4 ? footer.width - buttonW * 4 : buttonW;
  return {x + kHorizontalInset, footer.y + kVerticalInset, w - kHorizontalInset * 2, footer.height - kVerticalInset * 2};
}

void drawFooterButtonFrames(EpdDisplay& display, const Rect_t& footer, uint8_t color = AppConfig::kBlack) {
  for (uint8_t i = 0; i < 5; ++i) {
    display.drawRoundRect(footerButtonBounds(footer, i), 9, color);
  }
}

void drawPowerIcon(EpdDisplay& display, int32_t cx, int32_t cy, uint8_t color = AppConfig::kBlack) {
  display.drawCircle(cx, cy + 3, 22, color);
  display.drawCircle(cx, cy + 3, 21, color);
  display.fillRect({cx - 5, cy - 24, 10, 25}, AppConfig::kWhite);
  drawThickLine(display, cx, cy - 24, cx, cy - 4, color);
  drawThickLine(display, cx - 13, cy - 10, cx - 20, cy + 2, color);
  drawThickLine(display, cx + 13, cy - 10, cx + 20, cy + 2, color);
}

void drawHomeIcon(EpdDisplay& display, int32_t cx, int32_t cy, uint8_t color = AppConfig::kBlack) {
  drawThickLine(display, cx - 25, cy - 1, cx, cy - 24, color);
  drawThickLine(display, cx, cy - 24, cx + 25, cy - 1, color);
  display.drawRect({cx - 18, cy - 1, 36, 27}, color);
  display.drawRect({cx - 17, cy, 34, 25}, color);
  display.fillRect({cx - 6, cy + 10, 12, 16}, color);
}

void drawBrvarioHomeIcon(EpdDisplay& display, int32_t cx, int32_t cy, uint8_t color = AppConfig::kBlack) {
  drawPackedIcon4bpp(display,
                     BrvarioParagliderIcon::kPixels,
                     BrvarioParagliderIcon::kWidth,
                     BrvarioParagliderIcon::kHeight,
                     cx - BrvarioParagliderIcon::kWidth / 2,
                     cy - BrvarioParagliderIcon::kHeight / 2,
                     color);
}

void drawWeatherIcon(EpdDisplay& display, int32_t cx, int32_t cy, uint8_t type) {
  switch (type) {
    case 0:  // temperature
      display.drawCircle(cx, cy + 14, 8, AppConfig::kBlack);
      display.fillCircle(cx, cy + 14, 5, AppConfig::kBlack);
      drawThickLine(display, cx, cy - 20, cx, cy + 8, AppConfig::kBlack);
      display.drawRect({cx - 5, cy - 21, 10, 29}, AppConfig::kBlack);
      break;
    case 1:  // wind
      drawThickLine(display, cx - 20, cy - 9, cx + 18, cy - 9, AppConfig::kBlack);
      drawThickLine(display, cx - 14, cy + 1, cx + 24, cy + 1, AppConfig::kBlack);
      drawThickLine(display, cx - 8, cy + 11, cx + 12, cy + 11, AppConfig::kBlack);
      display.fillTriangle(cx + 22, cy - 9, cx + 12, cy - 15, cx + 12, cy - 3, AppConfig::kBlack);
      break;
    case 2:  // cloud
      display.drawCircle(cx - 12, cy + 4, 10, AppConfig::kBlack);
      display.drawCircle(cx + 2, cy - 5, 15, AppConfig::kBlack);
      display.drawCircle(cx + 18, cy + 4, 11, AppConfig::kBlack);
      display.fillRect({cx - 25, cy + 1, 54, 22}, AppConfig::kWhite);
      drawThickLine(display, cx - 23, cy + 18, cx + 29, cy + 18, AppConfig::kBlack);
      drawThickLine(display, cx - 23, cy + 18, cx - 29, cy + 11, AppConfig::kBlack);
      drawThickLine(display, cx + 29, cy + 18, cx + 35, cy + 10, AppConfig::kBlack);
      drawThickLine(display, cx - 21, cy + 2, cx - 14, cy - 6, AppConfig::kBlack);
      drawThickLine(display, cx - 8, cy - 18, cx + 12, cy - 18, AppConfig::kBlack);
      drawThickLine(display, cx + 14, cy - 8, cx + 25, cy - 1, AppConfig::kBlack);
      break;
    case 3:  // rain
      display.drawCircle(cx - 8, cy - 9, 10, AppConfig::kBlack);
      display.drawCircle(cx + 7, cy - 13, 12, AppConfig::kBlack);
      display.drawRect({cx - 20, cy - 8, 42, 12}, AppConfig::kBlack);
      drawThickLine(display, cx - 12, cy + 10, cx - 17, cy + 22, AppConfig::kBlack);
      drawThickLine(display, cx + 1, cy + 10, cx - 4, cy + 22, AppConfig::kBlack);
      drawThickLine(display, cx + 14, cy + 10, cx + 9, cy + 22, AppConfig::kBlack);
      break;
    case 4:  // sun / UV
      display.drawCircle(cx, cy, 13, AppConfig::kBlack);
      display.fillCircle(cx, cy, 8, AppConfig::kBlack);
      drawThickLine(display, cx, cy - 25, cx, cy - 17, AppConfig::kBlack);
      drawThickLine(display, cx, cy + 17, cx, cy + 25, AppConfig::kBlack);
      drawThickLine(display, cx - 25, cy, cx - 17, cy, AppConfig::kBlack);
      drawThickLine(display, cx + 17, cy, cx + 25, cy, AppConfig::kBlack);
      break;
    default:  // compass
      display.drawCircle(cx, cy, 20, AppConfig::kBlack);
      display.fillTriangle(cx, cy - 18, cx - 7, cy + 8, cx, cy + 4, AppConfig::kBlack);
      display.fillTriangle(cx, cy + 18, cx + 7, cy - 8, cx, cy - 4, AppConfig::kBlack);
      break;
  }
}

int32_t roundToInt(float value) {
  return static_cast<int32_t>(value + (value >= 0.0F ? 0.5F : -0.5F));
}

int16_t roundToTenths(float value) {
  return static_cast<int16_t>(roundToInt(value * 10.0F));
}

int16_t normalizeRoundedDeg(float value) {
  int32_t rounded = roundToInt(value);
  while (rounded < 0) rounded += 360;
  while (rounded >= 360) rounded -= 360;
  return static_cast<int16_t>(rounded);
}

uint32_t mixHash(uint32_t hash, int32_t value) {
  hash ^= static_cast<uint32_t>(value) + 0x9E3779B9UL + (hash << 6) + (hash >> 2);
  return hash;
}

const char* gpsStatusText(GpsSensorStatus status) {
  switch (status) {
    case GpsSensorStatus::Off:
      return "UART OFF";
    case GpsSensorStatus::NoData:
      return "ERRO: SEM DADOS NMEA";
    case GpsSensorStatus::StaleData:
      return "ERRO: DADOS ANTIGOS";
    case GpsSensorStatus::NoFix:
      return "SEM FIX";
    case GpsSensorStatus::Fix:
      return "FIX OK";
  }
  return "GPS DESCONHECIDO";
}

const char* compactGpsStatusText(GpsSensorStatus status) {
  switch (status) {
    case GpsSensorStatus::Off:
      return "OFF";
    case GpsSensorStatus::NoData:
      return "SEM NMEA";
    case GpsSensorStatus::StaleData:
      return "PARADO";
    case GpsSensorStatus::NoFix:
      return "SEM FIX";
    case GpsSensorStatus::Fix:
      return "FIX OK";
  }
  return "---";
}

const char* barometerStatusText(BarometerSensorStatus status) {
  switch (status) {
    case BarometerSensorStatus::Off:
      return "OFF";
    case BarometerSensorStatus::NotFound:
      return "ERRO: NAO ENCONTRADO";
    case BarometerSensorStatus::CalibrationError:
      return "ERRO: CALIBRAÇAO";
    case BarometerSensorStatus::ConfigError:
      return "ERRO: CONFIGURAÇAO";
    case BarometerSensorStatus::NoSample:
      return "ERRO: SEM AMOSTRA";
    case BarometerSensorStatus::StaleSample:
      return "ERRO: AMOSTRA ANTIGA";
    case BarometerSensorStatus::ReadError:
      return "ERRO: FALHA LEITURA";
    case BarometerSensorStatus::Ok:
      return "BMP280 OK";
  }
  return "BARO DESCONHECIDO";
}

const char* compactBarometerStatusText(BarometerSensorStatus status) {
  switch (status) {
    case BarometerSensorStatus::Off:
      return "OFF";
    case BarometerSensorStatus::NotFound:
      return "NAO ACHOU";
    case BarometerSensorStatus::CalibrationError:
      return "CALIB ERRO";
    case BarometerSensorStatus::ConfigError:
      return "CFG ERRO";
    case BarometerSensorStatus::NoSample:
      return "SEM AMOSTRA";
    case BarometerSensorStatus::StaleSample:
      return "AMOSTRA ANT";
    case BarometerSensorStatus::ReadError:
      return "ERRO LEIT";
    case BarometerSensorStatus::Ok:
      return "BMP280 OK";
  }
  return "---";
}

void formatAge(char* text, size_t size, uint32_t ageMs) {
  if (ageMs == UINT32_MAX) {
    snprintf(text, size, "NUNCA");
    return;
  }
  if (ageMs < 10000UL) {
    snprintf(text, size, "%lums", static_cast<unsigned long>(ageMs));
    return;
  }
  snprintf(text, size, "%lus", static_cast<unsigned long>(ageMs / 1000UL));
}

const char* responseLevelLabel(int8_t level) {
  if (level <= -4) return "MUITO SUAVE";
  if (level < 0) return "SUAVE";
  if (level == 0) return "NORMAL";
  if (level >= 4) return "MUITO SENSIVEL";
  return "SENSIVEL";
}

const char* pitchLevelLabel(int8_t level) {
  if (level <= -4) return "MUITO GRAVE";
  if (level < 0) return "GRAVE";
  if (level == 0) return "NORMAL";
  if (level >= 4) return "MUITO AGUDO";
  return "AGUDO";
}

void formatLevelText(char* text, size_t size, const char* label, int8_t level) {
  snprintf(text, size, "%s %+d", label, static_cast<int>(level));
}

void formatFileSize(uint32_t bytes, char* out, size_t outSize) {
  if (bytes >= 1024UL * 1024UL) {
    const uint32_t tenths = (bytes * 10UL) / (1024UL * 1024UL);
    snprintf(out, outSize, "%lu.%lu MB", static_cast<unsigned long>(tenths / 10UL), static_cast<unsigned long>(tenths % 10UL));
    return;
  }
  const uint32_t kb = (bytes + 1023UL) / 1024UL;
  snprintf(out, outSize, "%lu KB", static_cast<unsigned long>(kb));
}

void formatStorageSize(uint64_t bytes, char* out, size_t outSize) {
  static constexpr uint64_t kKb = 1024ULL;
  static constexpr uint64_t kMb = 1024ULL * 1024ULL;
  static constexpr uint64_t kGb = 1024ULL * 1024ULL * 1024ULL;
  if (bytes >= kGb) {
    const uint64_t tenths = (bytes * 10ULL) / kGb;
    snprintf(out, outSize, "%llu.%llu GB", tenths / 10ULL, tenths % 10ULL);
    return;
  }
  if (bytes >= kMb) {
    const uint64_t tenths = (bytes * 10ULL) / kMb;
    snprintf(out, outSize, "%llu.%llu MB", tenths / 10ULL, tenths % 10ULL);
    return;
  }
  snprintf(out, outSize, "%llu KB", (bytes + kKb - 1ULL) / kKb);
}

const char* fileNameFromPath(const char* path) {
  const char* name = path ? path : "";
  for (const char* cursor = name; *cursor != '\0'; ++cursor) {
    if (*cursor == '/' || *cursor == '\\') {
      name = cursor + 1;
    }
  }
  return name;
}

void formatDurationText(uint32_t seconds, char* out, size_t outSize) {
  const uint32_t hours = seconds / 3600UL;
  const uint32_t minutes = (seconds / 60UL) % 60UL;
  const uint32_t secs = seconds % 60UL;
  snprintf(out, outSize, "%02lu:%02lu:%02lu", static_cast<unsigned long>(hours), static_cast<unsigned long>(minutes),
           static_cast<unsigned long>(secs));
}

void formatTimeOfDayText(uint32_t seconds, char* out, size_t outSize) {
  seconds %= 86400UL;
  snprintf(out,
           outSize,
           "%02lu:%02lu",
           static_cast<unsigned long>(seconds / 3600UL),
           static_cast<unsigned long>((seconds / 60UL) % 60UL));
}

void formatLocalTimeOfDayText(uint32_t utcSeconds, char* out, size_t outSize) {
  int32_t localSeconds = static_cast<int32_t>(utcSeconds % 86400UL) - 3L * 3600L;
  while (localSeconds < 0) {
    localSeconds += 86400L;
  }
  while (localSeconds >= 86400L) {
    localSeconds -= 86400L;
  }
  formatTimeOfDayText(static_cast<uint32_t>(localSeconds), out, outSize);
}

char keyboardKeyAt(uint8_t mode, uint8_t row, uint8_t col) {
  static const char row0[] = "1234567890";
  static const char row1Lower[] = "qwertyuiop";
  static const char row1Upper[] = "QWERTYUIOP";
  static const char row2Lower[] = "asdfghjkl";
  static const char row2Upper[] = "ASDFGHJKL";
  static const char row3Lower[] = "zxcvbnm._-";
  static const char row3Upper[] = "ZXCVBNM._-";
  static const char row0Symbols[] = "!@#$%^&*()";
  static const char row1Symbols[] = "-_=+[]{}\\|";
  static const char row2Symbols[] = ";:'\",<>?/";
  static const char row3Symbols[] = ".+-*/%~?@#";

  const char* text = row0;
  uint8_t maxCol = 10;
  if (mode == 2) {
    if (row == 0) {
      text = row0Symbols;
    } else if (row == 1) {
      text = row1Symbols;
    } else if (row == 2) {
      text = row2Symbols;
      maxCol = 9;
    } else if (row == 3) {
      text = row3Symbols;
    }
  } else if (row == 1) {
    text = mode == 1 ? row1Upper : row1Lower;
  } else if (row == 2) {
    text = mode == 1 ? row2Upper : row2Lower;
    maxCol = 9;
  } else if (row == 3) {
    text = mode == 1 ? row3Upper : row3Lower;
  }
  return col < maxCol ? text[col] : '\0';
}

const char* dashboardSlotLabel(DashboardSlot slot) {
  switch (slot) {
    case DashboardSlot::Left:
      return "ESQUERDA";
    case DashboardSlot::Center:
      return "CENTRO";
    case DashboardSlot::Right:
      return "DIREITA";
    default:
      return "";
  }
}

float degToRad(float deg) {
  return deg * 0.01745329252F;
}

int32_t pointOnCircleX(int32_t cx, int32_t radius, float angleDeg) {
  return cx + roundToInt(cosf(degToRad(angleDeg)) * static_cast<float>(radius));
}

int32_t pointOnCircleY(int32_t cy, int32_t radius, float angleDeg) {
  return cy + roundToInt(sinf(degToRad(angleDeg)) * static_cast<float>(radius));
}

void drawPreviewArc(EpdDisplay& display, int32_t cx, int32_t cy, int32_t radius, float startDeg, float endDeg, uint8_t color) {
  int32_t prevX = pointOnCircleX(cx, radius, startDeg);
  int32_t prevY = pointOnCircleY(cy, radius, startDeg);
  for (float angle = startDeg + 4.0F; angle <= endDeg + 0.1F; angle += 4.0F) {
    const int32_t x = pointOnCircleX(cx, radius, angle);
    const int32_t y = pointOnCircleY(cy, radius, angle);
    display.drawLine(prevX, prevY, x, y, color);
    prevX = x;
    prevY = y;
  }
}

void drawPreviewNeedle(EpdDisplay& display, int32_t cx, int32_t cy, float angleDeg, int32_t length, int32_t baseWidth) {
  const float angle = degToRad(angleDeg);
  const float normal = angle + 1.5707963F;
  const int32_t tipX = cx + roundToInt(cosf(angle) * static_cast<float>(length));
  const int32_t tipY = cy + roundToInt(sinf(angle) * static_cast<float>(length));
  const int32_t leftX = cx + roundToInt(cosf(normal) * static_cast<float>(baseWidth));
  const int32_t leftY = cy + roundToInt(sinf(normal) * static_cast<float>(baseWidth));
  const int32_t rightX = cx - roundToInt(cosf(normal) * static_cast<float>(baseWidth));
  const int32_t rightY = cy - roundToInt(sinf(normal) * static_cast<float>(baseWidth));
  display.fillTriangle(tipX, tipY, leftX, leftY, rightX, rightY, AppConfig::kBlack);
  display.fillCircle(cx, cy, baseWidth / 2, AppConfig::kWhite);
  display.drawCircle(cx, cy, baseWidth / 2 + 1, AppConfig::kBlack);
}

void drawMiniVarioPreview(EpdDisplay& display, const Rect_t& box) {
  const int32_t cx = box.x + box.width / 2;
  const int32_t cy = box.y + 164;
  const int32_t radius = 76;
  drawPreviewArc(display, cx, cy, radius, -210.0F, 30.0F, AppConfig::kBlack);
  drawPreviewArc(display, cx, cy, radius - 1, -210.0F, 30.0F, AppConfig::kBlack);

  for (int8_t value = -4; value <= 4; ++value) {
    const float angle = -210.0F + static_cast<float>(value + 4) * 30.0F;
    const int32_t outerX = pointOnCircleX(cx, radius, angle);
    const int32_t outerY = pointOnCircleY(cy, radius, angle);
    const int32_t innerX = pointOnCircleX(cx, radius - 13, angle);
    const int32_t innerY = pointOnCircleY(cy, radius - 13, angle);
    drawThickLine(display, innerX, innerY, outerX, outerY, AppConfig::kBlack);

    char text[5];
    snprintf(text, sizeof(text), "%d", static_cast<int>(value));
    display.drawSmallTextBoldAligned(text, pointOnCircleX(cx, radius + 18, angle) - 3, pointOnCircleY(cy, radius + 18, angle) - 5, 1,
                                      AppConfig::kBlack, EpdDisplay::Align::Center);
  }

  display.drawCircle(cx, cy, 52, AppConfig::kBlack);
  display.drawCircle(cx, cy, 51, AppConfig::kBlack);
  drawPreviewNeedle(display, cx, cy, -42.0F, 60, 10);
  display.drawSmallTextBoldAligned("+1.2", cx, cy + 40, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("m/s", cx + 62, cy + 48, 1, AppConfig::kBlack, EpdDisplay::Align::Center);
}

void drawMiniCompassPreview(EpdDisplay& display, const Rect_t& box) {
  const int32_t cx = box.x + box.width / 2;
  const int32_t cy = box.y + 164;
  const int32_t radius = 74;
  display.drawCircle(cx, cy, radius, AppConfig::kBlack);
  display.drawCircle(cx, cy, radius - 1, AppConfig::kBlack);
  display.drawLine(cx, cy - radius + 18, cx, cy + radius - 18, AppConfig::kBlack);
  display.drawLine(cx - radius + 18, cy, cx + radius - 18, cy, AppConfig::kBlack);
  display.drawSmallTextBoldAligned("N", cx, cy - radius + 4, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("S", cx, cy + radius - 20, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("O", cx - radius + 10, cy - 8, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("L", cx + radius - 10, cy - 8, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  display.fillTriangle(cx, cy - 48, cx - 13, cy + 18, cx + 13, cy + 18, AppConfig::kBlack);
  display.fillCircle(cx, cy + 2, 9, AppConfig::kWhite);
  display.drawCircle(cx, cy + 2, 10, AppConfig::kBlack);
  drawThickLine(display, cx - 42, cy + 35, cx + 42, cy - 30, AppConfig::kBlack);
  display.fillTriangle(cx + 42, cy - 30, cx + 25, cy - 27, cx + 35, cy - 13, AppConfig::kBlack);
}

void drawMiniThermalPreview(EpdDisplay& display, const Rect_t& box) {
  const int32_t cx = box.x + box.width / 2;
  const int32_t cy = box.y + 164;
  const int32_t radius = 74;
  display.drawCircle(cx, cy, radius, AppConfig::kBlack);
  display.drawCircle(cx, cy, radius - 1, AppConfig::kBlack);
  display.drawCircle(cx, cy, radius / 2, AppConfig::kBlack);
  display.drawCircle(cx, cy, radius / 4, AppConfig::kBlack);
  display.drawLine(cx, cy - radius + 22, cx, cy + radius - 22, AppConfig::kBlack);
  display.drawLine(cx - radius + 22, cy, cx + radius - 22, cy, AppConfig::kBlack);
  display.drawSmallTextBoldAligned("N", cx, cy - radius + 6, 1, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("S", cx, cy + radius - 16, 1, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("O", cx - radius + 10, cy - 5, 1, AppConfig::kBlack, EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("L", cx + radius - 10, cy - 5, 1, AppConfig::kBlack, EpdDisplay::Align::Center);

  display.fillTriangle(cx, cy - 16, cx - 10, cy + 14, cx + 10, cy + 14, AppConfig::kBlack);
  display.fillCircle(cx + 34, cy - 30, 7, AppConfig::kBlack);
  display.fillCircle(cx + 48, cy - 12, 5, AppConfig::kBlack);
  display.drawCircle(cx + 16, cy - 40, 5, AppConfig::kBlack);
  display.drawCircle(cx - 34, cy + 20, 4, AppConfig::kBlack);
  display.drawCircle(cx - 18, cy - 22, 4, AppConfig::kBlack);
  drawThickLine(display, cx - 38, cy + 45, cx - 8, cy + 18, AppConfig::kBlack);
  display.fillTriangle(cx - 8, cy + 18, cx - 24, cy + 23, cx - 16, cy + 34, AppConfig::kBlack);
}

void drawDashboardWidgetPreview(EpdDisplay& display, const Rect_t& box, DashboardWidgetKind widget) {
  switch (widget) {
    case DashboardWidgetKind::Thermal:
      drawMiniThermalPreview(display, box);
      break;
    case DashboardWidgetKind::Vario:
      drawMiniVarioPreview(display, box);
      break;
    case DashboardWidgetKind::Compass:
      drawMiniCompassPreview(display, box);
      break;
    default:
      break;
  }
}

}  // namespace

MainScreen::MainScreen(EpdDisplay& display)
    : display_(display),
      layout_(Layout::horizontal()),
      header_(layout_.header),
      vario_(layout_.vario),
      info_(layout_.info),
      speed_(layout_.speed) {}

void MainScreen::attachAudioEditor(VarioBuzzer* buzzer) {
  audioBuzzer_ = buzzer;
}

void MainScreen::attachFirmwareUpdater(FirmwareUpdater* updater) {
  firmwareUpdater_ = updater;
}

void MainScreen::attachWifiManager(WifiManager* wifi) {
  wifiManager_ = wifi;
}

void MainScreen::attachMapDownloadManager(MapDownloadManager* maps) {
  mapDownloadManager_ = maps;
}

void MainScreen::attachOpenWeatherClient(OpenWeatherClient* weather) {
  weatherClient_ = weather;
}

void MainScreen::attachWeatherLocationManager(WeatherLocationManager* location) {
  weatherLocationManager_ = location;
}

void MainScreen::attachFlightSiteCatalogUpdater(FlightSiteCatalogUpdater* updater) {
  flightSiteCatalogUpdater_ = updater;
}

void MainScreen::attachPilotProfile(PilotProfileConfig* profile) {
  pilotProfile_ = profile;
}

void MainScreen::attachFlightRecorder(FlightRecorder* recorder) {
  flightRecorder_ = recorder;
}

void MainScreen::attachTracklogBleService(TracklogBleService* ble) {
  tracklogBle_ = ble;
}

void MainScreen::attachThermalAssistConfig(ThermalAssistConfig* config) {
  thermalAssistConfig_ = config;
}

void MainScreen::attachStorageManager(StorageManager* storage) {
  storageManager_ = storage;
  mapPage_.attachStorageManager(storage);
  if (flightRecorder_) {
    flightRecorder_->attachArchiveStorage(storageManager_ ? storageManager_->filesystem() : nullptr, kAdvancedIgcSdDir);
  }
}

MainScreen::PageDebug MainScreen::currentPageDebug() const {
  PageDebug debug;
  debug.page = static_cast<uint8_t>(activePage_);
  debug.zoneCount = touchZoneCount_;
  return debug;
}

void MainScreen::begin(const VarioData& data) {
  dashboardLayout_.begin();
  mapPage_.begin(display_);
  configureDashboardWidgetBounds();
  lastData_ = data;
  if (weatherLocationManager_) {
    weatherLocationManager_->updateGpsLocation(data.latitudeDeg, data.longitudeDeg, data.gpsFix);
  }
  visualState_ = buildVisualState(data);
  visualStateValid_ = true;
  activePage_ = Page::Dashboard;
  dashboardStartupLock_ = true;
  dashboardStartupProgress_ = 6;
  dashboardStartupStartedMs_ = millis();
  dashboardStartupLastRefreshMs_ = 0;
  renderStatic(data);
  display_.saveBaseBuffer();
  renderDynamic(data);
  renderLayoutFrame();
  renderDashboardOverlays(data);
  display_.fullRefresh();
  markFooterDisplayed(Page::Dashboard);
  partialCycleCount_ = 0;
  pageAutoRefreshMs_ = millis();
  pageFullRefreshMs_ = pageAutoRefreshMs_;
}

void MainScreen::showDashboard(const VarioData& data) {
  lastData_ = data;
  openDashboard();
}

void MainScreen::showWeatherStationSleep(const VarioData& data) {
  weatherStationSleepMode_ = true;
  lastData_ = data;
  openPage(Page::WeatherStation);
}

void MainScreen::setWeatherStationSleepMode(bool enabled) {
  weatherStationSleepMode_ = enabled;
}

void MainScreen::update(const VarioData& data) {
  const VisualState nextState = buildVisualState(data);
  lastData_ = data;
  if (weatherLocationManager_) {
    weatherLocationManager_->updateGpsLocation(data.latitudeDeg, data.longitudeDeg, data.gpsFix);
  }
  char completedPath[64];
  if (flightRecorder_ && flightRecorder_->consumeCompletedFlight(completedPath, sizeof(completedPath))) {
    openTracklogDetails(completedPath);
    return;
  }
  if (serviceFooterMaintenance(millis())) {
    return;
  }
  if (activePage_ == Page::WifiSettings && wifiManager_ && static_cast<uint8_t>(wifiManager_->state()) != wifiLastState_ &&
      millis() - wifiLastRefreshMs_ > 700UL) {
    wifiLastState_ = static_cast<uint8_t>(wifiManager_->state());
    refreshWifiStatusArea();
    wifiLastRefreshMs_ = millis();
    return;
  }
  if (activePage_ == Page::ThermalCycleBeta) {
    const uint32_t now = millis();
    const bool processedSample = thermalCycleBeta_.ingestPressureHpa(data.pressureHpa, data.sensorDataValid, now);
    if (refreshIdlePageFullIfDue(now)) {
      return;
    }
    if (processedSample && now - pageAutoRefreshMs_ >= kThermalCyclePagePartialRefreshMs) {
      pageAutoRefreshMs_ = now;
      refreshActivePage();
    }
    return;
  }
  if (activePage_ == Page::WeatherStation || activePage_ == Page::SystemStatus || activePage_ == Page::Map) {
    const uint32_t now = millis();
    if (activePage_ == Page::Map) {
      syncMapPageData(data);
      const bool forceFullMapRefresh = partialCycleCount_ >= AppConfig::kFullRefreshEveryPartialCycles;
      const bool refreshBase = mapPage_.needsBaseRedraw() || forceFullMapRefresh;
      if (forceFullMapRefresh || refreshBase || (mapPage_.needsDynamicRedraw() && now - mapDynamicRefreshMs_ >= 900UL)) {
        refreshMapPage(refreshBase, forceFullMapRefresh);
        if (forceFullMapRefresh) {
          partialCycleCount_ = 0;
        } else {
          ++partialCycleCount_;
        }
        mapDynamicRefreshMs_ = now;
        if (refreshBase) {
          registerPageTouchZones();
        }
      }
      return;
    }
    const uint32_t intervalMs = 2000UL;
    if (refreshIdlePageFullIfDue(now)) {
      return;
    }
    if (now - pageAutoRefreshMs_ >= intervalMs) {
      pageAutoRefreshMs_ = now;
      refreshActivePage();
    }
    return;
  }
  if (activePage_ == Page::WeatherLocation && weatherLocationView_ == WeatherLocationView::CatalogLoading) {
    const bool completed = processWeatherCatalogMatchChunk(12);
    if (completed) {
      refreshActivePage();
    } else if (weatherCatalogLoadingProgress_ >= static_cast<uint8_t>(weatherCatalogRenderedProgress_ + 10)) {
      weatherCatalogRenderedProgress_ = weatherCatalogLoadingProgress_;
      const Rect_t& s = layout_.screen;
      const Rect_t loadingArea = {s.x + 100, s.y + 148, s.width - 200, 190};
      refreshActivePageArea(loadingArea);
    }
    return;
  }
  if (activePage_ == Page::WeatherLocation && weatherLocationView_ == WeatherLocationView::Catalog) {
    const uint32_t now = millis();
    const uint8_t progress = flightSiteCatalogUpdater_ ? flightSiteCatalogUpdater_->progressPercent() : 0;
    const uint8_t state = flightSiteCatalogUpdater_ ? static_cast<uint8_t>(flightSiteCatalogUpdater_->state()) : 0;
    if (progress != weatherCatalogLastProgress_ || state != weatherCatalogLastState_ ||
        (flightSiteCatalogUpdater_ && flightSiteCatalogUpdater_->busy() && now - pageAutoRefreshMs_ >= 700UL)) {
      if (state != weatherCatalogLastState_) {
        weatherCatalogFilterDirty_ = true;
      }
      weatherCatalogLastProgress_ = progress;
      weatherCatalogLastState_ = state;
      pageAutoRefreshMs_ = now;
      if (weatherCatalogFilterDirty_ && (!flightSiteCatalogUpdater_ || !flightSiteCatalogUpdater_->busy())) {
        beginWeatherCatalogMatchRebuild();
      }
      refreshActivePage();
    }
    return;
  }
  if (activePage_ == Page::Tracklog || activePage_ == Page::TracklogDetails) {
    const bool bleVisible = tracklogBle_ && tracklogBle_->visibleStatus();
    const uint8_t progress = bleVisible ? tracklogBle_->progressPercent() : 0;
    const uint32_t now = millis();
    if (refreshIdlePageFullIfDue(now)) {
      return;
    }
    if (bleVisible != tracklogBleStatusVisible_ || progress != tracklogBleLastProgress_ || (bleVisible && now - pageAutoRefreshMs_ >= 1000UL)) {
      tracklogBleStatusVisible_ = bleVisible;
      tracklogBleLastProgress_ = progress;
      pageAutoRefreshMs_ = now;
      refreshActivePage();
    }
    return;
  }
  if (activePage_ == Page::MapDownload) {
    const uint8_t progress = mapDownloadManager_ ? mapDownloadManager_->progressPercent() : 0;
    const uint8_t state = mapDownloadManager_ ? static_cast<uint8_t>(mapDownloadManager_->state()) : 0;
    const uint32_t now = millis();
    if (refreshIdlePageFullIfDue(now)) {
      return;
    }
    if (progress != mapDownloadLastProgress_ || state != mapDownloadLastState_ ||
        (mapDownloadManager_ && mapDownloadManager_->busy() && now - pageAutoRefreshMs_ >= 700UL)) {
      if (state != mapDownloadLastState_ && mapDownloadManager_ && mapDownloadManager_->state() == MapDownloadState::Success) {
        OfflineMapPackage::invalidateCache();
        mapPage_.invalidatePreparedBaseCache();
      }
      mapDownloadLastProgress_ = progress;
      mapDownloadLastState_ = state;
      pageAutoRefreshMs_ = now;
      refreshActivePage();
    }
    return;
  }
  if (activePage_ != Page::Dashboard) {
    refreshIdlePageFullIfDue(millis());
    return;
  }

  if (dashboardStartupLock_) {
    const uint32_t now = millis();
    const bool gpsReady = data.gpsFix || (data.latitudeDeg != 0.0F && data.longitudeDeg != 0.0F);
    const bool waitedEnough = dashboardStartupStartedMs_ == 0 || now - dashboardStartupStartedMs_ >= kStartupMapGpsWaitMs;
    uint8_t nextProgress = 12;
    if (dashboardStartupStartedMs_ != 0) {
      const uint32_t elapsed = now - dashboardStartupStartedMs_;
      const uint32_t scaled = 12UL + (elapsed * 58UL) / kStartupMapGpsWaitMs;
      nextProgress = static_cast<uint8_t>(scaled > 70UL ? 70UL : scaled);
    }
    if (gpsReady && nextProgress < 70) {
      nextProgress = 70;
    }
    if (nextProgress > dashboardStartupProgress_) {
      dashboardStartupProgress_ = nextProgress;
    }
    if (gpsReady || waitedEnough) {
      if (dashboardStartupProgress_ < 82) {
        dashboardStartupProgress_ = 82;
        refreshDashboardOverlayArea(startupLoadingPopupBounds());
      }
      prepareMapPageBaseCache(true);
      dashboardStartupProgress_ = 100;
      dashboardStartupLock_ = false;
      dashboardStartupLastRefreshMs_ = 0;
      display_.restoreBaseBuffer();
      renderDynamic(lastData_);
      renderLayoutFrame();
      renderDashboardOverlays(lastData_);
      Rect_t clearAreas[2] = {startupLoadingPopupBounds(), header_.dynamicBounds()};
      display_.updateAreas(clearAreas, 2);
      registerPageTouchZones();
    } else if (dashboardStartupLastRefreshMs_ == 0 || now - dashboardStartupLastRefreshMs_ >= kStartupOverlayRefreshMs) {
      refreshDashboardOverlayArea(startupLoadingPopupBounds());
      dashboardStartupLastRefreshMs_ = now;
    }
    visualState_ = nextState;
    visualStateValid_ = true;
    return;
  }

  configureDashboardWidgetBounds();
  display_.restoreBaseBuffer();
  renderDynamic(data);
  renderLayoutFrame();
  renderDashboardOverlays(data);

  if (partialCycleCount_ >= AppConfig::kFullRefreshEveryPartialCycles) {
    display_.fullRefresh();
    markFooterDisplayed(Page::Dashboard);
    partialCycleCount_ = 0;
    visualState_ = nextState;
    visualStateValid_ = true;
    prepareMapPageBaseCache(false);
    return;
  }

  updateDynamicAreas(nextState);
  visualState_ = nextState;
  visualStateValid_ = true;
  ++partialCycleCount_;
  prepareMapPageBaseCache(false);
}

void MainScreen::renderStatic(const VarioData& data) {
  (void)data;
  configureDashboardWidgetBounds();
  powerConfirmVisible_ = false;
  display_.clearBuffer(AppConfig::kWhite);
  header_.renderStatic(display_);
  vario_.renderStatic(display_);
  info_.renderStatic(display_);
  speed_.renderStatic(display_);
  renderLayoutFrame();
  registerTouchZones();
}

void MainScreen::renderDynamic(const VarioData& data) {
  configureDashboardWidgetBounds();
  header_.renderDynamic(display_, data);
  vario_.renderDynamic(display_, data);
  info_.renderDynamic(display_, data);
  speed_.renderDynamic(display_, data);
  renderCenterMetrics(data);
  renderFooterDynamic(data);
}

void MainScreen::updateDynamicAreas(const VisualState& nextState) {
  Rect_t dirtyAreas[18];
  size_t count = 0;
  bool lowerBandChanged = false;

  const bool allDirty = !visualStateValid_;
  const DashboardWidgetKind centerWidget = dashboardLayout_.widgetForSlot(DashboardSlot::Center);
  const bool thermalCenter = centerWidget == DashboardWidgetKind::Thermal;
  const bool compassCenter = centerWidget == DashboardWidgetKind::Compass;
  const bool centerMetricsActive = centerWidget != DashboardWidgetKind::Vario;

  if (allDirty || nextState.clockMinute != visualState_.clockMinute || nextState.batteryPercent != visualState_.batteryPercent ||
      nextState.batteryCharging != visualState_.batteryCharging ||
      nextState.gpsFix != visualState_.gpsFix || nextState.satellites != visualState_.satellites ||
      nextState.wifiEnabled != visualState_.wifiEnabled || nextState.bluetoothActive != visualState_.bluetoothActive ||
      nextState.bluetoothConnected != visualState_.bluetoothConnected) {
    dirtyAreas[count++] = header_.dynamicBounds();
  }

  const bool altGpsChanged = allDirty || nextState.altGpsM != visualState_.altGpsM;
  const bool altAglChanged = allDirty || nextState.altAglM != visualState_.altAglM;
  const bool groundSpeedChanged = allDirty || nextState.groundSpeedKmh != visualState_.groundSpeedKmh;
  const bool windSpeedChanged = allDirty || nextState.windSpeedKmh != visualState_.windSpeedKmh || nextState.windQuality != visualState_.windQuality;

  const bool varioChanged = allDirty || nextState.varioTenths != visualState_.varioTenths || nextState.ganhoM != visualState_.ganhoM ||
                            nextState.glideTenths != visualState_.glideTenths || nextState.elapsedSeconds != visualState_.elapsedSeconds ||
                            nextState.thermalCoreConfidencePercent != visualState_.thermalCoreConfidencePercent ||
                            nextState.thermalCoreMetricVisible != visualState_.thermalCoreMetricVisible ||
                            (thermalCenter && (altGpsChanged || altAglChanged)) || (compassCenter && (groundSpeedChanged || windSpeedChanged));
  if (varioChanged) {
    dirtyAreas[count++] = vario_.dynamicBounds();
  }
  if (centerMetricsActive &&
      (allDirty || nextState.ganhoM != visualState_.ganhoM || nextState.glideTenths != visualState_.glideTenths ||
       nextState.thermalCoreConfidencePercent != visualState_.thermalCoreConfidencePercent ||
       nextState.thermalCoreMetricVisible != visualState_.thermalCoreMetricVisible ||
       nextState.elapsedSeconds != visualState_.elapsedSeconds)) {
    dirtyAreas[count++] = centerMetricBounds();
  }

  const bool thermalChanged = allDirty || nextState.thermalHash != visualState_.thermalHash;
  if (thermalChanged) {
    dirtyAreas[count++] = info_.dynamicBounds();
  }
  if (!thermalCenter && altGpsChanged) {
    dirtyAreas[count++] = info_.cellValueBounds(0);
  }
  if (!thermalCenter && altAglChanged) {
    dirtyAreas[count++] = info_.cellValueBounds(2);
  }
  if (altAglChanged) {
    lowerBandChanged = true;
  }

  const bool compassChanged = allDirty || nextState.courseDeg != visualState_.courseDeg ||
                              nextState.windDirectionDeg != visualState_.windDirectionDeg ||
                              nextState.windQuality != visualState_.windQuality;
  if (compassChanged) {
    dirtyAreas[count++] = speed_.compassBounds();
  }
  if (!compassCenter && groundSpeedChanged) {
    dirtyAreas[count++] = speed_.groundSpeedValueBounds();
  }
  if (!compassCenter && windSpeedChanged) {
    dirtyAreas[count++] = speed_.windSpeedValueBounds();
  }
  if (windSpeedChanged) {
    lowerBandChanged = true;
  }
  const bool footerChanged = allDirty || nextState.audioEnabled != visualState_.audioEnabled ||
                             nextState.trackingEnabled != visualState_.trackingEnabled;
  if (footerChanged) {
    dirtyAreas[count++] = footerDynamicBounds();
    lowerBandChanged = true;
  }

  if (powerConfirmVisible_) {
    dirtyAreas[count++] = powerConfirmPopupBounds();
  }
  if (kGpsNoFixPopupEnabled &&
      (!nextState.gpsFix || allDirty || nextState.gpsFix != visualState_.gpsFix || nextState.satellites != visualState_.satellites)) {
    dirtyAreas[count++] = gpsNoFixPopupBounds();
  }

  Rect_t reinforceAreas[3];
  size_t reinforceCount = 0;
  if (count > 0 && (partialCycleCount_ % AppConfig::kGlobalContrastEveryCycles) == 0) {
    reinforceAreas[reinforceCount++] = {layout_.screen.x, layout_.screen.y, layout_.screen.width, layout_.trend.y - layout_.screen.y};
  } else if (AppConfig::kLowerBandContrastEveryCycles > 0 && lowerBandChanged &&
             (partialCycleCount_ % AppConfig::kLowerBandContrastEveryCycles) == 0) {
    reinforceAreas[reinforceCount++] = lowerBandBounds();
  }

  display_.updateAreasAndReinforce(dirtyAreas, count, reinforceAreas, reinforceCount);
  if (footerChanged) {
    markFooterDisplayed(Page::Dashboard);
  }
}

Rect_t MainScreen::lowerBandBounds() const {
  const int32_t y = layout_.info.y + layout_.info.height - 124;
  const int32_t bottom = layout_.screen.y + layout_.screen.height;
  return {layout_.screen.x, y, layout_.screen.width, bottom - y};
}

Rect_t MainScreen::footerContrastBounds() const {
  const int32_t y = layout_.trend.y;
  const int32_t bottom = layout_.screen.y + layout_.screen.height;
  return {layout_.footerButtons.x, y, layout_.footerButtons.width, bottom - y};
}

uint32_t MainScreen::footerSignature(Page page) const {
  uint32_t hash = 2166136261UL;
  const bool firstButtonSettings = page == Page::Dashboard || pageUsesFooterSettings(page);
  const bool lastButtonPower = page == Page::Dashboard;
  const bool tracklogActive = page == Page::Dashboard ? lastData_.trackingEnabled : page == Page::Tracklog;
  const uint8_t values[] = {
      static_cast<uint8_t>(page),
      static_cast<uint8_t>(firstButtonSettings ? 1 : 0),
      static_cast<uint8_t>(lastButtonPower ? 1 : 0),
      static_cast<uint8_t>(lastData_.audioEnabled ? 1 : 0),
      static_cast<uint8_t>(tracklogActive ? 1 : 0),
  };
  for (uint8_t value : values) {
    hash ^= value;
    hash *= 16777619UL;
  }
  return hash;
}

bool MainScreen::footerNeedsPanelUpdate(Page page) const {
  return !footerDisplayedSignatureValid_ || footerDisplayedSignature_ != footerSignature(page);
}

void MainScreen::markFooterDisplayed(Page page) {
  footerDisplayedSignature_ = footerSignature(page);
  footerDisplayedSignatureValid_ = true;
  footerLastSoftCleanMs_ = millis();
}

void MainScreen::renderActiveFooter() {
  renderPageFooter(activePage_);
}

bool MainScreen::serviceFooterMaintenance(uint32_t now) {
  if (dashboardStartupLock_ || powerConfirmVisible_ || !display_.isReady()) {
    return false;
  }
  if (tracklogBle_ && tracklogBle_->activeTransfer()) {
    return false;
  }
  if (mapDownloadManager_ && mapDownloadManager_->busy()) {
    return false;
  }
  if (footerLastSoftCleanMs_ != 0 && now - footerLastSoftCleanMs_ < kFooterSoftCleanIntervalMs) {
    return false;
  }

  const Rect_t area = layout_.footerButtons;
  display_.fillRect(area, AppConfig::kWhite);
  display_.updateAreas(&area, 1);
  renderActiveFooter();
  display_.updateAreas(&area, 1);
  footerLastSoftCleanMs_ = millis();
  markFooterDisplayed(activePage_);
  return true;
}

Rect_t MainScreen::centerMetricBounds() const {
  return {layout_.vario.x + 20, layout_.vario.y + layout_.vario.height - kCenterMetricReservedHeight, layout_.vario.width - 40, 56};
}

void MainScreen::renderCenterMetrics(const VarioData& data) {
  if (dashboardLayout_.widgetForSlot(DashboardSlot::Center) == DashboardWidgetKind::Vario) {
    return;
  }

  const Rect_t area = centerMetricBounds();
  const int32_t cx = area.x + area.width / 2;
  display_.fillRect(area, AppConfig::kWhite);
  display_.drawLine(area.x + 8, area.y, area.x + area.width - 8, area.y, AppConfig::kBlack);
  display_.drawLine(area.x + 8, area.y + 31, area.x + area.width - 8, area.y + 31, AppConfig::kBlack);
  const bool thermalCoreMetric = showThermalCoreMetric(data);

  char text[14];
  if (thermalCoreMetric) {
    display_.drawLine(cx, area.y + 6, cx, area.y + 28, AppConfig::kBlack);
    display_.drawSmallTextBold("NUCLEO", area.x + 16, area.y + 9, 2, AppConfig::kBlack);
    snprintf(text, sizeof(text), "%u%%", static_cast<unsigned>(data.thermalCoreConfidencePercent));
    display_.drawSmallTextBoldAligned(text, cx - 18, area.y + 9, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
    display_.drawSmallTextBold("GANHO", cx + 18, area.y + 9, 2, AppConfig::kBlack);
    snprintf(text, sizeof(text), "%.0f m", data.ganhoTermicaM);
    display_.drawSmallTextBoldAligned(text, area.x + area.width - 16, area.y + 9, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
  } else {
    display_.drawSmallTextBold("PLANEIO", cx - 100, area.y + 8, 2, AppConfig::kBlack);
    snprintf(text, sizeof(text), "%.1f", data.glideRatio);
    display_.drawSmallTextBoldAligned(text, cx + 100, area.y + 8, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
  }

  char duration[16];
  formatDurationText(data.elapsedSeconds, duration, sizeof(duration));
  char durationText[32];
  snprintf(durationText, sizeof(durationText), "DURAÇAO VOO %s", duration);
  display_.drawSmallTextBoldAligned(durationText, cx, area.y + 34, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
}

Rect_t MainScreen::dashboardSlotBounds(DashboardSlot slot) const {
  switch (slot) {
    case DashboardSlot::Left:
      return layout_.info;
    case DashboardSlot::Center:
      return layout_.vario;
    case DashboardSlot::Right:
      return layout_.speed;
    default:
      return layout_.vario;
  }
}

void MainScreen::configureDashboardWidgetBounds() {
  const DashboardWidgetKind centerWidget = dashboardLayout_.widgetForSlot(DashboardSlot::Center);

  Rect_t thermalBounds = dashboardSlotBounds(dashboardLayout_.slotForWidget(DashboardWidgetKind::Thermal));
  Rect_t speedBounds = dashboardSlotBounds(dashboardLayout_.slotForWidget(DashboardWidgetKind::Compass));
  if (centerWidget == DashboardWidgetKind::Thermal) {
    thermalBounds.height -= kCenterMetricReservedHeight;
  }
  if (centerWidget == DashboardWidgetKind::Compass) {
    speedBounds.height -= kCenterMetricReservedHeight;
  }

  info_.setBounds(thermalBounds);
  vario_.setBounds(dashboardSlotBounds(dashboardLayout_.slotForWidget(DashboardWidgetKind::Vario)));
  speed_.setBounds(speedBounds);

  info_.setMode(centerWidget == DashboardWidgetKind::Thermal ? InfoGridWidget::Mode::ThermalOnly : InfoGridWidget::Mode::Full);
  speed_.setMode(centerWidget == DashboardWidgetKind::Compass ? SpeedGaugeWidget::Mode::CompassOnly : SpeedGaugeWidget::Mode::Full);

  VarioGaugeWidget::SideInfoMode varioSideInfo = VarioGaugeWidget::SideInfoMode::None;
  if (centerWidget == DashboardWidgetKind::Thermal) {
    varioSideInfo = VarioGaugeWidget::SideInfoMode::Altitude;
  } else if (centerWidget == DashboardWidgetKind::Compass) {
    varioSideInfo = VarioGaugeWidget::SideInfoMode::Speed;
  }
  vario_.setSideInfoMode(varioSideInfo);
}

MainScreen::VisualState MainScreen::buildVisualState(const VarioData& data) const {
  VisualState state;
  state.clockMinute = static_cast<uint16_t>((data.timeOfDaySeconds / 60UL) % 1440UL);
  state.batteryPercent = data.batteryPercent;
  state.batteryCharging = data.batteryCharging;
  state.gpsFix = data.gpsFix;
  state.satellites = data.satellites;
  state.wifiEnabled = data.wifiEnabled;
  state.bluetoothActive = data.bluetoothActive;
  state.bluetoothConnected = data.bluetoothConnected;
  state.varioTenths = roundToTenths(data.varioMs);
  state.ganhoM = roundToInt(data.ganhoTermicaM);
  state.glideTenths = roundToTenths(data.glideRatio);
  state.thermalCoreConfidencePercent = data.thermalCoreConfidencePercent;
  state.thermalCoreMetricVisible = showThermalCoreMetric(data);
  state.elapsedSeconds = data.elapsedSeconds;
  state.altGpsM = roundToInt(data.altitudeGpsM);
  state.altAglM = roundToInt(data.altitudeAglM);
  state.groundSpeedKmh = roundToInt(data.groundSpeedKmh);
  state.windSpeedKmh = roundToInt(data.windSpeedKmh);
  state.courseDeg = normalizeRoundedDeg(data.courseDeg);
  state.windDirectionDeg = normalizeRoundedDeg(data.windDirectionDeg);
  state.windQuality = static_cast<uint8_t>(data.windQuality);
  state.audioEnabled = data.audioEnabled;
  state.trackingEnabled = data.trackingEnabled;

  uint32_t thermalHash = 2166136261UL;
  thermalHash = mixHash(thermalHash, state.courseDeg / 10);
  thermalHash = mixHash(thermalHash, normalizeRoundedDeg(data.thermalDriftDeg));
  thermalHash = mixHash(thermalHash, roundToInt(data.thermalRangeM));
  thermalHash = mixHash(thermalHash, static_cast<int32_t>(data.thermalVisualMode));
  thermalHash = mixHash(thermalHash, static_cast<int32_t>(data.thermalDriftMode));
  thermalHash = mixHash(thermalHash, data.thermalCoreConfidencePercent);
  thermalHash = mixHash(thermalHash, roundToInt(data.thermalPilotEastM));
  thermalHash = mixHash(thermalHash, roundToInt(data.thermalPilotNorthM));
  thermalHash = mixHash(thermalHash, data.thermalPointCount);
  const uint8_t pointCount = data.thermalPointCount < kThermalAssistPoints ? data.thermalPointCount : kThermalAssistPoints;
  for (uint8_t i = 0; i < pointCount; ++i) {
    thermalHash = mixHash(thermalHash, roundToInt(data.thermalPoints[i].eastM));
    thermalHash = mixHash(thermalHash, roundToInt(data.thermalPoints[i].northM));
    thermalHash = mixHash(thermalHash, roundToTenths(data.thermalPoints[i].liftMs));
  }
  thermalHash = mixHash(thermalHash, data.thermalHistoryCount);
  const uint8_t historyCount = data.thermalHistoryCount < kThermalHistoryPoints ? data.thermalHistoryCount : kThermalHistoryPoints;
  for (uint8_t i = 0; i < historyCount; ++i) {
    thermalHash = mixHash(thermalHash, roundToInt(data.thermalHistory[i].eastM));
    thermalHash = mixHash(thermalHash, roundToInt(data.thermalHistory[i].northM));
    thermalHash = mixHash(thermalHash, roundToTenths(data.thermalHistory[i].coreMs));
    thermalHash = mixHash(thermalHash, data.thermalHistory[i].confidencePercent);
    thermalHash = mixHash(thermalHash, data.thermalHistory[i].ageMinutes);
    thermalHash = mixHash(thermalHash, data.thermalHistory[i].active ? 1 : 0);
  }
  state.thermalHash = thermalHash;
  return state;
}

TouchAction MainScreen::previewTouchAction(int32_t x, int32_t y) const {
  if (activePage_ == Page::Dashboard && dashboardStartupLock_) {
    return TouchAction::None;
  }
  if (powerConfirmVisible_ && activePage_ == Page::Dashboard) {
    if (pointInRect(powerConfirmYesButtonBounds(), x, y)) return TouchAction::PowerConfirmYes;
    if (pointInRect(powerConfirmNoButtonBounds(), x, y)) return TouchAction::PowerConfirmNo;
    if (pointInRect(powerConfirmPopupBounds(), x, y)) return TouchAction::PowerRequest;
  }
  if (activePage_ == Page::WeatherStation) {
    if (weatherInfoPopupVisible_) {
      if (pointInRect(weatherInfoCloseButtonBounds(), x, y)) return TouchAction::WeatherInfo;
      if (pointInRect(weatherInfoScrollUpButtonBounds(), x, y)) return TouchAction::WeatherInfoUp;
      if (pointInRect(weatherInfoScrollDownButtonBounds(), x, y)) return TouchAction::WeatherInfoDown;
      if (pointInRect(weatherInfoPopupBounds(), x, y)) return TouchAction::WeatherInfo;
    } else if (pointInRect(weatherInfoButtonBounds(), x, y)) {
      return TouchAction::WeatherInfo;
    } else if (!weatherStationSleepMode_ && !weatherHasActiveForecast() && pointInRect(weatherLocationLabelBounds(), x, y)) {
      return TouchAction::OpenWeatherLocation;
    }
  }
  if (activePage_ == Page::WeatherLocation) {
    if (weatherInfoPopupVisible_) {
      if (pointInRect(weatherInfoCloseButtonBounds(), x, y)) return TouchAction::WeatherInfo;
      if (pointInRect(weatherInfoScrollUpButtonBounds(), x, y)) return TouchAction::WeatherInfoUp;
      if (pointInRect(weatherInfoScrollDownButtonBounds(), x, y)) return TouchAction::WeatherInfoDown;
      if (pointInRect(weatherInfoPopupBounds(), x, y)) return TouchAction::WeatherInfo;
    } else if (pointInRect(weatherInfoButtonBounds(), x, y)) {
      return TouchAction::WeatherInfo;
    } else if (previewWeatherLocationTouch(x, y)) {
      return TouchAction::WeatherLocationAction;
    }
  }
  if (activePage_ == Page::ThermalAssistSettings) {
    if (thermalInfoPopupVisible_) {
      if (pointInRect(thermalInfoCloseButtonBounds(), x, y)) return TouchAction::ThermalInfo;
      if (pointInRect(thermalInfoScrollUpButtonBounds(), x, y)) return TouchAction::ThermalInfoUp;
      if (pointInRect(thermalInfoScrollDownButtonBounds(), x, y)) return TouchAction::ThermalInfoDown;
      if (pointInRect(thermalInfoPopupBounds(), x, y)) return TouchAction::ThermalInfo;
    } else if (pointInRect(thermalInfoButtonBounds(), x, y)) {
      return TouchAction::ThermalInfo;
    }
  }
  if (activePage_ == Page::ThermalCycleBeta) {
    if (thermalCycleInfoPopupVisible_) {
      if (pointInRect(thermalCycleInfoCloseButtonBounds(), x, y)) return TouchAction::ThermalCycleInfo;
      if (pointInRect(thermalCycleInfoScrollUpButtonBounds(), x, y)) return TouchAction::ThermalCycleInfoUp;
      if (pointInRect(thermalCycleInfoScrollDownButtonBounds(), x, y)) return TouchAction::ThermalCycleInfoDown;
      if (pointInRect(thermalCycleInfoPopupBounds(), x, y)) return TouchAction::ThermalCycleInfo;
    } else if (pointInRect(thermalCycleInfoButtonBounds(), x, y)) {
      return TouchAction::ThermalCycleInfo;
    }
  }
  if (activePage_ == Page::ManualLogic || activePage_ == Page::ManualUser) {
    if (pointInRect(manualScrollUpButtonBounds(), x, y)) return TouchAction::ManualPageUp;
    if (pointInRect(manualScrollDownButtonBounds(), x, y)) return TouchAction::ManualPageDown;
  }

  TouchAction action = footerActionAt(x, y);
  if (action != TouchAction::None) return action;

  action = actionAt(x, y);
  if (action != TouchAction::None) return action;

  switch (activePage_) {
    case Page::DashboardLayout: {
      DashboardSlot slot = DashboardSlot::Center;
      DashboardWidgetKind widget = DashboardWidgetKind::Vario;
      if (pointInRect(dashboardLayoutResetButtonBounds(), x, y)) return TouchAction::DashboardLayoutReset;
      if (dashboardSlotAt(x, y, slot) || dashboardPresetAt(x, y, widget)) return TouchAction::DashboardLayoutMove;
      break;
    }
    case Page::WifiSettings: {
      if (pointInRect(wifiScanButtonBounds(), x, y) || pointInRect(wifiConnectButtonBounds(), x, y) ||
          pointInRect(wifiClearButtonBounds(), x, y)) {
        return TouchAction::OpenWifiSettings;
      }
      for (uint8_t i = 0; i < 3; ++i) {
        if (pointInRect(wifiNetworkRowBounds(i), x, y)) return TouchAction::OpenWifiSettings;
      }
      for (uint8_t row = 0; row < 4; ++row) {
        const uint8_t cols = row == 2 ? 9 : 10;
        for (uint8_t col = 0; col < cols; ++col) {
          if (pointInRect(wifiKeyBounds(row, col), x, y)) return TouchAction::OpenWifiSettings;
        }
      }
      for (uint8_t i = 0; i < 4; ++i) {
        if (pointInRect(wifiSpecialKeyBounds(i), x, y)) return TouchAction::OpenWifiSettings;
      }
      break;
    }
    case Page::PilotProfile: {
      for (uint8_t i = 0; i < static_cast<uint8_t>(PilotProfileConfig::Field::Count); ++i) {
        if (pointInRect(profileFieldBounds(i), x, y)) return TouchAction::OpenPilotProfile;
      }
      for (uint8_t row = 0; row < 4; ++row) {
        const uint8_t cols = row == 2 ? 9 : 10;
        for (uint8_t col = 0; col < cols; ++col) {
          if (pointInRect(profileKeyBounds(row, col), x, y)) return TouchAction::OpenPilotProfile;
        }
      }
      for (uint8_t i = 0; i < 4; ++i) {
        if (pointInRect(profileSpecialKeyBounds(i), x, y)) return TouchAction::OpenPilotProfile;
      }
      break;
    }
    case Page::Tracklog: {
      if (tracklogDeleteConfirm_) {
        if (pointInRect(tracklogConfirmYesButtonBounds(), x, y) || pointInRect(tracklogConfirmNoButtonBounds(), x, y)) {
          return TouchAction::ToggleTracklog;
        }
      }
      for (uint8_t i = 0; i < 5; ++i) {
        if (pointInRect(tracklogRowDeleteButtonBounds(i), x, y) || pointInRect(tracklogRowBounds(i), x, y)) {
          return TouchAction::ToggleTracklog;
        }
      }
      if (pointInRect(tracklogPrevButtonBounds(), x, y) || pointInRect(tracklogSyncButtonBounds(), x, y) ||
          pointInRect(tracklogNextButtonBounds(), x, y)) {
        return TouchAction::ToggleTracklog;
      }
      break;
    }
    case Page::TracklogDetails:
      if (tracklogBle_ && tracklogBle_->visibleStatus() && pointInRect(tracklogBleCancelButtonBounds(), x, y)) {
        return TouchAction::TracklogBleCancel;
      }
      if (tracklogDeleteConfirm_) {
        if (pointInRect(tracklogConfirmYesButtonBounds(), x, y) || pointInRect(tracklogConfirmNoButtonBounds(), x, y)) {
          return TouchAction::ToggleTracklog;
        }
      }
      if (pointInRect(tracklogDeleteButtonBounds(), x, y) || pointInRect(tracklogExportButtonBounds(), x, y)) {
        return TouchAction::ToggleTracklog;
      }
      break;
    case Page::FirmwareUpdate:
      if (pointInRect(firmwareUpdateWifiButtonBounds(), x, y)) return TouchAction::OpenWifiSettings;
      if (pointInRect(firmwareUpdateStartButtonBounds(), x, y)) return TouchAction::OpenFirmwareUpdate;
      break;
    case Page::AudioEditor:
      if (pointInRect(audioVolumeSliderTouchBounds(), x, y)) return TouchAction::AudioVolumeSet;
      break;
    default:
      break;
  }

  return TouchAction::None;
}

bool MainScreen::handleTouch(int32_t x, int32_t y) {
  pageSwipeActive_ = false;
  pageSwipeConsumed_ = false;
  pageSwipeStartX_ = x;
  pageSwipeStartY_ = y;
  if (activePage_ == Page::ThermalCycleBeta) {
    thermalCycleBeta_.markArtifact(millis(), 15UL * 1000UL);
  }
  if ((activePage_ == Page::Dashboard || activePage_ == Page::Map) && !dashboardStartupLock_ && !powerConfirmVisible_) {
    const bool footerTouch = footerActionAt(x, y) != TouchAction::None;
    pageSwipeActive_ = !footerTouch;
  }

  if (activePage_ == Page::Dashboard && dashboardStartupLock_) {
    lastTouchAction_ = TouchAction::None;
    return true;
  }
  if (powerConfirmVisible_ && activePage_ == Page::Dashboard) {
    if (pointInRect(powerConfirmYesButtonBounds(), x, y)) {
      return dispatchTouchAction(TouchAction::PowerConfirmYes);
    }
    if (pointInRect(powerConfirmNoButtonBounds(), x, y)) {
      return dispatchTouchAction(TouchAction::PowerConfirmNo);
    }
    lastTouchAction_ = TouchAction::PowerRequest;
    return true;
  }
  if (activePage_ == Page::WeatherStation) {
    if (weatherInfoPopupVisible_) {
      if (pointInRect(weatherInfoCloseButtonBounds(), x, y)) {
        return dispatchTouchAction(TouchAction::WeatherInfo);
      }
      if (pointInRect(weatherInfoScrollUpButtonBounds(), x, y)) {
        return dispatchTouchAction(TouchAction::WeatherInfoUp);
      }
      if (pointInRect(weatherInfoScrollDownButtonBounds(), x, y)) {
        return dispatchTouchAction(TouchAction::WeatherInfoDown);
      }
      if (pointInRect(weatherInfoPopupBounds(), x, y)) {
        lastTouchAction_ = TouchAction::WeatherInfo;
        return true;
      }
    } else if (pointInRect(weatherInfoButtonBounds(), x, y)) {
      return dispatchTouchAction(TouchAction::WeatherInfo);
    } else if (!weatherStationSleepMode_ && !weatherHasActiveForecast() && pointInRect(weatherLocationLabelBounds(), x, y)) {
      return dispatchTouchAction(TouchAction::OpenWeatherLocation);
    }
  }
  if (activePage_ == Page::WeatherLocation) {
    if (weatherInfoPopupVisible_) {
      if (pointInRect(weatherInfoCloseButtonBounds(), x, y)) {
        return dispatchTouchAction(TouchAction::WeatherInfo);
      }
      if (pointInRect(weatherInfoScrollUpButtonBounds(), x, y)) {
        return dispatchTouchAction(TouchAction::WeatherInfoUp);
      }
      if (pointInRect(weatherInfoScrollDownButtonBounds(), x, y)) {
        return dispatchTouchAction(TouchAction::WeatherInfoDown);
      }
      lastTouchAction_ = TouchAction::WeatherInfo;
      return true;
    }
    if (pointInRect(weatherInfoButtonBounds(), x, y)) {
      return dispatchTouchAction(TouchAction::WeatherInfo);
    }
    if (handleWeatherLocationTouch(x, y)) {
      return true;
    }
  }
  if (activePage_ == Page::ThermalAssistSettings) {
    if (thermalInfoPopupVisible_) {
      if (pointInRect(thermalInfoCloseButtonBounds(), x, y)) {
        return dispatchTouchAction(TouchAction::ThermalInfo);
      }
      if (pointInRect(thermalInfoScrollUpButtonBounds(), x, y)) {
        return dispatchTouchAction(TouchAction::ThermalInfoUp);
      }
      if (pointInRect(thermalInfoScrollDownButtonBounds(), x, y)) {
        return dispatchTouchAction(TouchAction::ThermalInfoDown);
      }
      if (pointInRect(thermalInfoPopupBounds(), x, y)) {
        lastTouchAction_ = TouchAction::ThermalInfo;
        return true;
      }
    } else if (pointInRect(thermalInfoButtonBounds(), x, y)) {
      return dispatchTouchAction(TouchAction::ThermalInfo);
    }
  }
  if (activePage_ == Page::ThermalCycleBeta) {
    if (thermalCycleInfoPopupVisible_) {
      if (pointInRect(thermalCycleInfoCloseButtonBounds(), x, y)) {
        return dispatchTouchAction(TouchAction::ThermalCycleInfo);
      }
      if (pointInRect(thermalCycleInfoScrollUpButtonBounds(), x, y)) {
        return dispatchTouchAction(TouchAction::ThermalCycleInfoUp);
      }
      if (pointInRect(thermalCycleInfoScrollDownButtonBounds(), x, y)) {
        return dispatchTouchAction(TouchAction::ThermalCycleInfoDown);
      }
      if (pointInRect(thermalCycleInfoPopupBounds(), x, y)) {
        lastTouchAction_ = TouchAction::ThermalCycleInfo;
        return true;
      }
    } else if (pointInRect(thermalCycleInfoButtonBounds(), x, y)) {
      return dispatchTouchAction(TouchAction::ThermalCycleInfo);
    }
  }

  TouchAction action = footerActionAt(x, y);
  if (action == TouchAction::None && activePage_ == Page::DashboardLayout) {
    return handleDashboardLayoutTouch(x, y);
  }
  if (action == TouchAction::None && activePage_ == Page::WifiSettings) {
    return handleWifiTouch(x, y);
  }
  if (action == TouchAction::None && activePage_ == Page::PilotProfile) {
    return handlePilotProfileTouch(x, y);
  }
  if (action == TouchAction::None && activePage_ == Page::Tracklog) {
    return handleTracklogTouch(x, y);
  }
  if (action == TouchAction::None && activePage_ == Page::TracklogDetails) {
    return handleTracklogDetailsTouch(x, y);
  }
  if (action == TouchAction::None && activePage_ == Page::FirmwareUpdate) {
    return handleFirmwareUpdateTouch(x, y);
  }
  if (action == TouchAction::None && activePage_ == Page::AudioEditor && handleAudioEditorTouch(x, y)) {
    return true;
  }
  if (action == TouchAction::None) {
    action = actionAt(x, y);
  }

  if (action != TouchAction::None) {
    return dispatchTouchAction(action);
  }

  lastTouchAction_ = TouchAction::None;
  return false;
}

bool MainScreen::handleTouchHold(int32_t x, int32_t y) {
  if (!pageSwipeActive_ || pageSwipeConsumed_) {
    return false;
  }
  if (activePage_ != Page::Dashboard && activePage_ != Page::Map) {
    pageSwipeActive_ = false;
    return false;
  }

  const int32_t dx = x - pageSwipeStartX_;
  const int32_t dy = y - pageSwipeStartY_;
  const int64_t distanceSq = static_cast<int64_t>(dx) * static_cast<int64_t>(dx) + static_cast<int64_t>(dy) * static_cast<int64_t>(dy);
  if (distanceSq < static_cast<int64_t>(kPageDragMinDistancePx) * static_cast<int64_t>(kPageDragMinDistancePx)) {
    return false;
  }

  pageSwipeConsumed_ = true;
  pageSwipeActive_ = false;
  if (activePage_ == Page::Dashboard) {
    lastTouchAction_ = TouchAction::NextPage;
    openPage(Page::Map);
    return true;
  }

  lastTouchAction_ = TouchAction::Home;
  openDashboard();
  return true;
}

void MainScreen::renderLayoutFrame() {
  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);

  const int32_t headerBottom = layout_.header.y + layout_.header.height;
  const int32_t footerTop = layout_.trend.y;
  display_.drawLine(s.x, headerBottom, s.x + s.width - 1, headerBottom, AppConfig::kBlack);

  const int32_t leftRight = layout_.info.x + layout_.info.width;
  const int32_t centerRight = layout_.vario.x + layout_.vario.width;
  display_.drawLine(leftRight, headerBottom, leftRight, s.y + s.height - 1, AppConfig::kBlack);
  display_.drawLine(centerRight, headerBottom, centerRight, s.y + s.height - 1, AppConfig::kBlack);

  display_.drawLine(layout_.trend.x, footerTop, layout_.trend.x + layout_.trend.width - 1, footerTop, kFooterRuleInk);
  const int32_t buttonW = layout_.footerButtons.width / 5;
  drawFooterButtonFrames(display_, layout_.footerButtons, kFooterInk);
  const int32_t buttonCenterY = layout_.footerButtons.y + layout_.footerButtons.height / 2;
  drawSettingsIcon(display_, layout_.footerButtons.x + buttonW / 2, buttonCenterY, kFooterInk);
  drawAudioIcon(display_, layout_.footerButtons.x + buttonW + buttonW / 2, buttonCenterY, lastData_.audioEnabled, kFooterInk);
  drawTracklogIcon(display_, layout_.footerButtons.x + buttonW * 2 + buttonW / 2, buttonCenterY, lastData_.trackingEnabled, kFooterInk);
  drawMapIcon(display_, layout_.footerButtons.x + buttonW * 3 + buttonW / 2, buttonCenterY, kFooterInk);
  drawPowerIcon(display_, layout_.footerButtons.x + buttonW * 4 + buttonW / 2, buttonCenterY, kFooterInk);
}

void MainScreen::renderFooterDynamic(const VarioData& data) {
  const Rect_t area = footerDynamicBounds();
  display_.fillRect(area, AppConfig::kWhite);

  const int32_t buttonW = layout_.footerButtons.width / 5;
  const int32_t buttonCenterY = layout_.footerButtons.y + layout_.footerButtons.height / 2;
  drawFooterButtonFrames(display_, layout_.footerButtons, kFooterInk);
  drawSettingsIcon(display_, layout_.footerButtons.x + buttonW / 2, buttonCenterY, kFooterInk);
  drawAudioIcon(display_, layout_.footerButtons.x + buttonW + buttonW / 2, buttonCenterY, data.audioEnabled, kFooterInk);
  drawTracklogIcon(display_, layout_.footerButtons.x + buttonW * 2 + buttonW / 2, buttonCenterY, data.trackingEnabled, kFooterInk);
  drawMapIcon(display_, layout_.footerButtons.x + buttonW * 3 + buttonW / 2, buttonCenterY, kFooterInk);
  drawPowerIcon(display_, layout_.footerButtons.x + buttonW * 4 + buttonW / 2, buttonCenterY, kFooterInk);
}

void MainScreen::renderDashboardOverlays(const VarioData& data) {
  if (dashboardStartupLock_) {
    renderStartupLoadingPopup(data);
    return;
  }
  if (kGpsNoFixPopupEnabled && !data.gpsFix) {
    renderGpsNoFixPopup(data);
  }
  if (powerConfirmVisible_) {
    renderPowerConfirmPopup();
  }
}

void MainScreen::renderStartupLoadingPopup(const VarioData& data) {
  const Rect_t popup = startupLoadingPopupBounds();
  display_.fillRect(popup, AppConfig::kWhite);
  display_.drawRect(popup, AppConfig::kBlack);
  display_.drawRect({popup.x + 2, popup.y + 2, popup.width - 4, popup.height - 4}, AppConfig::kBlack);

  display_.drawSmallTextBoldAligned(data.gpsFix ? "GPS CONECTADO" : "GPS SEM FIX",
                                    popup.x + popup.width / 2,
                                    popup.y + 18,
                                    3,
                                    AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("CARREGANDO CACHE DE MAPA OFFLINE",
                                popup.x + popup.width / 2,
                                popup.y + 66,
                                2,
                                AppConfig::kBlack,
                                EpdDisplay::Align::Center);

  char status[48];
  snprintf(status,
           sizeof(status),
           "SATELITES: %u   TOUCH BLOQUEADO",
           static_cast<unsigned>(data.satellites));
  display_.drawSmallTextBoldAligned(status, popup.x + popup.width / 2, popup.y + 102, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  const Rect_t bar = {popup.x + 54, popup.y + 142, popup.width - 108, 20};
  display_.drawRect(bar, AppConfig::kBlack);
  uint32_t progress = dashboardStartupProgress_;
  if (progress < 4UL) progress = 4UL;
  if (progress > 100UL) progress = 100UL;
  const int32_t fillW = static_cast<int32_t>((bar.width - 4) * progress / 100UL);
  if (fillW > 0) {
    display_.fillRect({bar.x + 2, bar.y + 2, fillW, bar.height - 4}, AppConfig::kBlack);
  }

  display_.drawSmallTextAligned("A TELA PRINCIPAL SERA LIBERADA AO FINAL DO CACHE",
                                popup.x + popup.width / 2,
                                popup.y + popup.height - 26,
                                1,
                                AppConfig::kBlack,
                                EpdDisplay::Align::Center);
}

void MainScreen::renderPowerConfirmPopup() {
  const Rect_t popup = powerConfirmPopupBounds();
  display_.fillRect(popup, AppConfig::kWhite);
  display_.drawRect(popup, AppConfig::kBlack);
  display_.drawRect({popup.x + 2, popup.y + 2, popup.width - 4, popup.height - 4}, AppConfig::kBlack);
  display_.drawSmallTextBoldAligned("DESLIGAR O VARIO?", popup.x + popup.width / 2, popup.y + 22, 3, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("O EQUIPAMENTO ENTRARA EM MODO SLEEP", popup.x + popup.width / 2, popup.y + 70, 2, AppConfig::kBlack,
                                EpdDisplay::Align::Center);
  drawButton(powerConfirmYesButtonBounds(), "SIM", 3);
  drawButton(powerConfirmNoButtonBounds(), "NAO", 3);
}

void MainScreen::renderGpsNoFixPopup(const VarioData& data) {
  const Rect_t popup = gpsNoFixPopupBounds();
  display_.fillRect(popup, AppConfig::kWhite);
  display_.drawRect(popup, AppConfig::kBlack);
  display_.drawRect({popup.x + 2, popup.y + 2, popup.width - 4, popup.height - 4}, AppConfig::kBlack);
  display_.drawSmallTextBoldAligned("GPS SEM FIX", popup.x + popup.width / 2, popup.y + 18, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("AGUARDE O GPS CONECTAR ANTES DE DECOLAR", popup.x + popup.width / 2, popup.y + 66, 2,
                                AppConfig::kBlack, EpdDisplay::Align::Center);
  char sats[24];
  snprintf(sats, sizeof(sats), "SATELITES: %u", static_cast<unsigned>(data.satellites));
  display_.drawSmallTextBoldAligned(sats, popup.x + popup.width / 2, popup.y + 104, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
}

void MainScreen::refreshDashboardOverlayArea(const Rect_t& area) {
  if (activePage_ != Page::Dashboard) return;
  display_.restoreBaseBuffer();
  renderDynamic(lastData_);
  renderLayoutFrame();
  renderDashboardOverlays(lastData_);
  display_.updateAreas(&area, 1);
}

Rect_t MainScreen::footerDynamicBounds() const {
  return {layout_.footerButtons.x + 2, layout_.footerButtons.y + 2, layout_.footerButtons.width - 4, layout_.footerButtons.height - 4};
}

Rect_t MainScreen::settingsAudioEditorButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, false, 2);
}

Rect_t MainScreen::settingsAudioEditorTouchBounds() const {
  return settingsButtonTouchBounds(settingsAudioEditorButtonBounds());
}

Rect_t MainScreen::settingsDashboardLayoutButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, false, 0);
}

Rect_t MainScreen::settingsDashboardLayoutTouchBounds() const {
  return settingsButtonTouchBounds(settingsDashboardLayoutButtonBounds());
}

Rect_t MainScreen::settingsThermalAssistButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, false, 1);
}

Rect_t MainScreen::settingsThermalAssistTouchBounds() const {
  return settingsButtonTouchBounds(settingsThermalAssistButtonBounds());
}

Rect_t MainScreen::settingsWifiButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, true, 0);
}

Rect_t MainScreen::settingsWifiTouchBounds() const {
  return settingsButtonTouchBounds(settingsWifiButtonBounds());
}

Rect_t MainScreen::settingsFirmwareUpdateButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, true, 1);
}

Rect_t MainScreen::settingsFirmwareUpdateTouchBounds() const {
  return settingsButtonTouchBounds(settingsFirmwareUpdateButtonBounds());
}

Rect_t MainScreen::settingsWeatherStationButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, false, 3);
}

Rect_t MainScreen::settingsWeatherStationTouchBounds() const {
  return settingsButtonTouchBounds(settingsWeatherStationButtonBounds());
}

Rect_t MainScreen::settingsPilotProfileButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, false, 4);
}

Rect_t MainScreen::settingsPilotProfileTouchBounds() const {
  return settingsButtonTouchBounds(settingsPilotProfileButtonBounds());
}

Rect_t MainScreen::settingsThermalCycleButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, false, 5);
}

Rect_t MainScreen::settingsThermalCycleTouchBounds() const {
  return settingsButtonTouchBounds(settingsThermalCycleButtonBounds());
}

Rect_t MainScreen::settingsDeviceInfoButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, true, 3);
}

Rect_t MainScreen::settingsDeviceInfoTouchBounds() const {
  return settingsButtonTouchBounds(settingsDeviceInfoButtonBounds());
}

Rect_t MainScreen::settingsSystemStatusButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, true, 4);
}

Rect_t MainScreen::settingsSystemStatusTouchBounds() const {
  return settingsButtonTouchBounds(settingsSystemStatusButtonBounds());
}

Rect_t MainScreen::settingsManualButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, false, 6);
}

Rect_t MainScreen::settingsManualTouchBounds() const {
  return settingsButtonTouchBounds(settingsManualButtonBounds());
}

Rect_t MainScreen::manualLogicButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 92, s.y + 158, s.width - 184, 92};
}

Rect_t MainScreen::manualUserButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 92, s.y + 284, s.width - 184, 92};
}

Rect_t MainScreen::settingsStorageButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, true, 2);
}

Rect_t MainScreen::settingsStorageTouchBounds() const {
  return settingsButtonTouchBounds(settingsStorageButtonBounds());
}

Rect_t MainScreen::settingsAdvancedSystemButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return settingsColumnButtonBounds(s, true, 5);
}

Rect_t MainScreen::settingsAdvancedSystemTouchBounds() const {
  return settingsButtonTouchBounds(settingsAdvancedSystemButtonBounds());
}

Rect_t MainScreen::firmwareUpdateStartButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width / 2 - 170, s.y + 310, 340, 58};
}

Rect_t MainScreen::firmwareUpdateWifiButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width / 2 - 170, s.y + 310, 340, 58};
}

Rect_t MainScreen::dashboardLayoutSlotBounds(DashboardSlot slot) const {
  const Rect_t& s = layout_.screen;
  const int32_t boxW = 260;
  const int32_t boxH = 264;
  const int32_t y = s.y + 104;
  switch (slot) {
    case DashboardSlot::Left:
      return {s.x + 40, y, boxW, boxH};
    case DashboardSlot::Center:
      return {s.x + s.width / 2 - boxW / 2, y, boxW, boxH};
    case DashboardSlot::Right:
      return {s.x + s.width - boxW - 40, y, boxW, boxH};
    default:
      return {s.x + s.width / 2 - boxW / 2, y, boxW, boxH};
  }
}

Rect_t MainScreen::dashboardLayoutPresetBounds(DashboardWidgetKind widget) const {
  const Rect_t& s = layout_.screen;
  const int32_t buttonW = 260;
  const int32_t buttonH = 44;
  const int32_t y = s.y + 92;
  if (widget == DashboardWidgetKind::Vario) {
    return {s.x + 40, y, buttonW, buttonH};
  }
  if (widget == DashboardWidgetKind::Compass) {
    return {s.x + s.width / 2 - buttonW / 2, y, buttonW, buttonH};
  }
  return {s.x + s.width - buttonW - 40, y, buttonW, buttonH};
}

Rect_t MainScreen::dashboardLayoutSaveButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 200, layout_.trend.y - 68, 220, 52};
}

Rect_t MainScreen::dashboardLayoutResetButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width / 2 - 110, layout_.trend.y - 68, 220, 52};
}

Rect_t MainScreen::wifiScanButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 44, s.y + 64, 420, 38};
}

Rect_t MainScreen::wifiConnectButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 686, s.y + 64, 150, 38};
}

Rect_t MainScreen::wifiClearButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 500, s.y + 64, 170, 38};
}

Rect_t MainScreen::wifiNetworkRowBounds(uint8_t index) const {
  const Rect_t& s = layout_.screen;
  return {s.x + 44, s.y + 112 + static_cast<int32_t>(index) * 42, 420, 34};
}

Rect_t MainScreen::wifiStatusAreaBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 36, s.y + 58, s.width - 72, 196};
}

Rect_t MainScreen::wifiKeyboardAreaBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 36, s.y + 244, s.width - 72, 222};
}

Rect_t MainScreen::wifiKeyBounds(uint8_t row, uint8_t col) const {
  const int32_t keyW = 80;
  const int32_t keyH = 36;
  const int32_t gap = 7;
  const int32_t y = layout_.screen.y + 252 + static_cast<int32_t>(row) * 43;
  int32_t x = layout_.screen.x + 44 + static_cast<int32_t>(col) * (keyW + gap);
  if (row == 2) {
    x += 44;
  }
  return {x, y, keyW, keyH};
}

Rect_t MainScreen::wifiSpecialKeyBounds(uint8_t index) const {
  const int32_t y = layout_.screen.y + 424;
  if (index == 0) return {layout_.screen.x + 44, y, 150, 34};
  if (index == 1) return {layout_.screen.x + 214, y, 150, 34};
  if (index == 2) return {layout_.screen.x + 384, y, 270, 34};
  return {layout_.screen.x + 674, y, 220, 34};
}

char MainScreen::wifiKeyAt(uint8_t row, uint8_t col) const {
  return keyboardKeyAt(wifiKeyboardMode_, row, col);
}

Rect_t MainScreen::profileFieldBounds(uint8_t index) const {
  const Rect_t& s = layout_.screen;
  return {s.x + 74, s.y + 82 + static_cast<int32_t>(index) * 43, s.width - 148, 34};
}

Rect_t MainScreen::profileKeyboardAreaBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 36, s.y + 244, s.width - 72, 222};
}

Rect_t MainScreen::profileKeyBounds(uint8_t row, uint8_t col) const {
  const int32_t keyW = 80;
  const int32_t keyH = 34;
  const int32_t gap = 7;
  const int32_t y = layout_.screen.y + 252 + static_cast<int32_t>(row) * 42;
  int32_t x = layout_.screen.x + 44 + static_cast<int32_t>(col) * (keyW + gap);
  if (row == 2) {
    x += 44;
  }
  return {x, y, keyW, keyH};
}

Rect_t MainScreen::profileSpecialKeyBounds(uint8_t index) const {
  const int32_t y = layout_.screen.y + 420;
  if (index == 0) return {layout_.screen.x + 44, y, 150, 34};
  if (index == 1) return {layout_.screen.x + 214, y, 150, 34};
  if (index == 2) return {layout_.screen.x + 384, y, 270, 34};
  return {layout_.screen.x + 674, y, 220, 34};
}

char MainScreen::profileKeyAt(uint8_t row, uint8_t col) const {
  return keyboardKeyAt(profileKeyboardMode_, row, col);
}

Rect_t MainScreen::tracklogPrevButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 84, layout_.trend.y - 54, 210, 40};
}

Rect_t MainScreen::tracklogNextButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width - 294, layout_.trend.y - 54, 210, 40};
}

Rect_t MainScreen::tracklogSyncButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width / 2 - 140, layout_.trend.y - 54, 280, 40};
}

Rect_t MainScreen::tracklogRowBounds(uint8_t index) const {
  const Rect_t& s = layout_.screen;
  return {s.x + 54, s.y + 192 + static_cast<int32_t>(index) * 44, s.width - 108, 38};
}

Rect_t MainScreen::tracklogRowDeleteButtonBounds(uint8_t index) const {
  const Rect_t row = tracklogRowBounds(index);
  return {row.x + row.width - 116, row.y + 4, 104, 30};
}

Rect_t MainScreen::tracklogDeleteButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width / 2 - 236, layout_.trend.y - 58, 210, 44};
}

Rect_t MainScreen::tracklogExportButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width / 2 + 26, layout_.trend.y - 58, 210, 44};
}

Rect_t MainScreen::tracklogInfoButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width - 274, layout_.trend.y - 58, 210, 44};
}

Rect_t MainScreen::tracklogConfirmPopupBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 178, s.y + 158, s.width - 356, 174};
}

Rect_t MainScreen::tracklogConfirmYesButtonBounds() const {
  const Rect_t popup = tracklogConfirmPopupBounds();
  return {popup.x + 70, popup.y + 106, 180, 46};
}

Rect_t MainScreen::tracklogConfirmNoButtonBounds() const {
  const Rect_t popup = tracklogConfirmPopupBounds();
  return {popup.x + popup.width - 250, popup.y + 106, 180, 46};
}

Rect_t MainScreen::tracklogBleStatusPopupBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 96, s.y + s.height / 2 - 92, s.width - 192, 184};
}

Rect_t MainScreen::tracklogBleCancelButtonBounds() const {
  const Rect_t popup = tracklogBleStatusPopupBounds();
  return {popup.x + popup.width - 64, popup.y + 12, 44, 36};
}

Rect_t MainScreen::powerConfirmPopupBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 176, s.y + 150, s.width - 352, 180};
}

Rect_t MainScreen::powerConfirmYesButtonBounds() const {
  const Rect_t popup = powerConfirmPopupBounds();
  return {popup.x + 72, popup.y + 114, 190, 48};
}

Rect_t MainScreen::powerConfirmNoButtonBounds() const {
  const Rect_t popup = powerConfirmPopupBounds();
  return {popup.x + popup.width - 262, popup.y + 114, 190, 48};
}

Rect_t MainScreen::startupLoadingPopupBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 130, s.y + 78, s.width - 260, 210};
}

Rect_t MainScreen::gpsNoFixPopupBounds() const {
  const Rect_t& s = layout_.screen;
  // TODO: when kGpsNoFixPopupEnabled returns to true, recenter/refine this popup.
  return {s.x + 190, s.y + 72, s.width - 380, 148};
}

Rect_t MainScreen::weatherPrevDayButtonBounds() const {
  return {layout_.speed.x + 42, layout_.footerButtons.y + 9, 72, 42};
}

Rect_t MainScreen::weatherNextDayButtonBounds() const {
  return {layout_.speed.x + layout_.speed.width - 114, layout_.footerButtons.y + 9, 72, 42};
}

Rect_t MainScreen::weatherInfoButtonBounds() const {
  return {layout_.info.x + layout_.info.width / 2 - 22, layout_.footerButtons.y + 9, 44, 42};
}

Rect_t MainScreen::weatherLocationLabelBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 28, s.y + 43, s.width - 56, 32};
}

Rect_t MainScreen::weatherLocationEnterButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width - 224, s.y + 174, 170, 38};
}

Rect_t MainScreen::weatherLocationMenuButtonBounds(uint8_t index) const {
  const Rect_t& s = layout_.screen;
  const int32_t gap = 18;
  const int32_t buttonW = (s.width - 110 - gap) / 2;
  const int32_t x = s.x + 46 + (index % 2) * (buttonW + gap);
  const int32_t y = s.y + 252 + (index / 2) * 76;
  return {x, y, buttonW, 58};
}

Rect_t MainScreen::weatherLocationRowBounds(uint8_t index) const {
  const Rect_t& s = layout_.screen;
  if (weatherLocationView_ == WeatherLocationView::Catalog) {
    return {s.x + 42, s.y + 144 + static_cast<int32_t>(index) * 63, s.width - 84, 57};
  }
  return {s.x + 42, s.y + 112 + static_cast<int32_t>(index) * 57, s.width - 84, 52};
}

Rect_t MainScreen::weatherLocationRowActionBounds(uint8_t index) const {
  const Rect_t row = weatherLocationRowBounds(index);
  return {row.x + row.width - 146, row.y + 6, 134, row.height - 12};
}

Rect_t MainScreen::weatherCatalogControlBounds(uint8_t index) const {
  const Rect_t& s = layout_.screen;
  if (index == 0) {
    return {s.x + 42, s.y + 405, 180, 46};
  }
  if (index == 1) {
    return {s.x + 240, s.y + 405, s.width - 480, 46};
  }
  return {s.x + s.width - 222, s.y + 405, 180, 46};
}

Rect_t MainScreen::weatherCatalogSearchButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 42, s.y + 78, 650, 42};
}

Rect_t MainScreen::weatherCatalogStateButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 704, s.y + 78, s.width - 746, 42};
}

Rect_t MainScreen::weatherCatalogSearchFieldBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 70, s.y + 78, s.width - 140, 58};
}

Rect_t MainScreen::weatherCatalogSearchKeyBounds(uint8_t row, uint8_t col) const {
  const Rect_t& s = layout_.screen;
  const int32_t startX = row == 0 ? s.x + 42 : (row == 1 ? s.x + 84 : s.x + 171);
  return {startX + static_cast<int32_t>(col) * 87, s.y + 158 + static_cast<int32_t>(row) * 58, 78, 48};
}

Rect_t MainScreen::weatherCatalogSearchSpecialBounds(uint8_t index) const {
  const Rect_t& s = layout_.screen;
  const int32_t gap = 10;
  const int32_t width = (s.width - 84 - gap * 3) / 4;
  return {s.x + 42 + static_cast<int32_t>(index) * (width + gap), s.y + 342, width, 52};
}

Rect_t MainScreen::weatherCatalogStateChoiceBounds(uint8_t index) const {
  const Rect_t& s = layout_.screen;
  const int32_t gap = 8;
  const int32_t width = (s.width - 84 - gap * 6) / 7;
  return {s.x + 42 + static_cast<int32_t>(index % 7) * (width + gap),
          s.y + 92 + static_cast<int32_t>(index / 7) * 68,
          width,
          54};
}

Rect_t MainScreen::weatherManualFieldBounds(uint8_t index) const {
  const Rect_t& s = layout_.screen;
  return {s.x + 56 + static_cast<int32_t>(index) * 430, s.y + 126, 392, 56};
}

Rect_t MainScreen::weatherManualKeyBounds(uint8_t row, uint8_t col) const {
  const Rect_t& s = layout_.screen;
  return {s.x + 72 + static_cast<int32_t>(col) * 92, s.y + 212 + static_cast<int32_t>(row) * 55, 78, 46};
}

Rect_t MainScreen::weatherManualDeleteButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 410, s.y + 232, 220, 54};
}

Rect_t MainScreen::weatherManualSaveButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 410, s.y + 314, 420, 64};
}

Rect_t MainScreen::weatherInfoPopupBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 70, s.y + 58, s.width - 140, layout_.trend.y - s.y - 94};
}

Rect_t MainScreen::weatherInfoCloseButtonBounds() const {
  const Rect_t popup = weatherInfoPopupBounds();
  return {popup.x + popup.width - 58, popup.y + 12, 42, 36};
}

Rect_t MainScreen::weatherInfoScrollUpButtonBounds() const {
  const Rect_t popup = weatherInfoPopupBounds();
  return {popup.x + popup.width - 62, popup.y + 72, 44, 44};
}

Rect_t MainScreen::weatherInfoScrollDownButtonBounds() const {
  const Rect_t popup = weatherInfoPopupBounds();
  return {popup.x + popup.width - 62, popup.y + popup.height - 62, 44, 44};
}

Rect_t MainScreen::thermalInfoButtonBounds() const {
  return {layout_.info.x + layout_.info.width / 2 - 22, layout_.footerButtons.y + 9, 44, 42};
}

Rect_t MainScreen::thermalInfoPopupBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 70, s.y + 58, s.width - 140, layout_.trend.y - s.y - 94};
}

Rect_t MainScreen::thermalInfoCloseButtonBounds() const {
  const Rect_t popup = thermalInfoPopupBounds();
  return {popup.x + popup.width - 58, popup.y + 12, 42, 36};
}

Rect_t MainScreen::thermalInfoScrollUpButtonBounds() const {
  const Rect_t popup = thermalInfoPopupBounds();
  return {popup.x + popup.width - 62, popup.y + 72, 44, 44};
}

Rect_t MainScreen::thermalInfoScrollDownButtonBounds() const {
  const Rect_t popup = thermalInfoPopupBounds();
  return {popup.x + popup.width - 62, popup.y + popup.height - 62, 44, 44};
}

Rect_t MainScreen::thermalCycleInfoButtonBounds() const {
  return {layout_.info.x + layout_.info.width / 2 - 22, layout_.footerButtons.y + 9, 44, 42};
}

Rect_t MainScreen::thermalCycleInfoPopupBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 70, s.y + 58, s.width - 140, layout_.trend.y - s.y - 94};
}

Rect_t MainScreen::thermalCycleInfoCloseButtonBounds() const {
  const Rect_t popup = thermalCycleInfoPopupBounds();
  return {popup.x + popup.width - 58, popup.y + 12, 42, 36};
}

Rect_t MainScreen::thermalCycleInfoScrollUpButtonBounds() const {
  const Rect_t popup = thermalCycleInfoPopupBounds();
  return {popup.x + popup.width - 62, popup.y + 72, 44, 44};
}

Rect_t MainScreen::thermalCycleInfoScrollDownButtonBounds() const {
  const Rect_t popup = thermalCycleInfoPopupBounds();
  return {popup.x + popup.width - 62, popup.y + popup.height - 62, 44, 44};
}

Rect_t MainScreen::manualTabBounds(uint8_t index) const {
  if (index >= kManualPageCount) {
    index = kManualPageCount - 1;
  }
  const Rect_t& s = layout_.screen;
  const Rect_t tabArea = {s.x + 42, s.y + 88, s.width - 134, 50};
  const int32_t tabW = tabArea.width / kManualPageCount;
  return {tabArea.x + static_cast<int32_t>(index) * tabW,
          tabArea.y,
          index + 1 == kManualPageCount ? tabArea.width - tabW * index : tabW,
          tabArea.height};
}

Rect_t MainScreen::manualScrollUpButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width - 82, s.y + 146, 52, 52};
}

Rect_t MainScreen::manualScrollDownButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width - 82, layout_.trend.y - 116, 52, 52};
}

uint8_t MainScreen::manualActiveLineCount() const {
  if (manualPage_ >= kManualPageCount) {
    return 0;
  }
  if (activePage_ == Page::ManualLogic) {
    return manualLogicText(manualPage_).lineCount;
  }
  if (activePage_ == Page::ManualUser) {
    return manualUserText(manualPage_).lineCount;
  }
  return 0;
}

Rect_t MainScreen::storageRefreshButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 350, s.y + 382, 260, 56};
}

Rect_t MainScreen::storageClearMapsButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 648, s.y + 382, 286, 56};
}

Rect_t MainScreen::storageDownloadButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 52, s.y + 382, 260, 56};
}

Rect_t MainScreen::advancedRecoverDisplayButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 58, s.y + 116, 390, 54};
}

Rect_t MainScreen::advancedMoveIgcButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width - 448, s.y + 116, 390, 54};
}

Rect_t MainScreen::advancedClearWifiButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 58, s.y + 184, 390, 54};
}

Rect_t MainScreen::advancedResetSettingsButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width - 448, s.y + 184, 390, 54};
}

Rect_t MainScreen::advancedClearWeatherButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 58, s.y + 252, 390, 54};
}

Rect_t MainScreen::advancedFormatSystemButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width - 448, s.y + 252, 390, 54};
}

Rect_t MainScreen::advancedConfirmYesButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width / 2 - 230, s.y + 384, 200, 48};
}

Rect_t MainScreen::advancedConfirmNoButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width / 2 + 30, s.y + 384, 200, 48};
}

Rect_t MainScreen::mapRegionButtonBounds(uint8_t index) const {
  const Rect_t& s = layout_.screen;
  const int32_t w = 286;
  const int32_t h = 78;
  const int32_t gapX = 24;
  const int32_t gapY = 12;
  const int32_t totalW = w * 3 + gapX * 2;
  const int32_t col = static_cast<int32_t>(index % 3);
  const int32_t row = static_cast<int32_t>(index / 3);
  const int32_t x = s.x + (s.width - totalW) / 2 + col * (w + gapX);
  const int32_t y = s.y + 106 + row * (h + gapY);
  return {x, y, w, h};
}

Rect_t MainScreen::mapDownloadStartButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 214, s.y + 382, 250, 56};
}

Rect_t MainScreen::mapDownloadCancelButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width - 464, s.y + 382, 250, 56};
}

Rect_t MainScreen::mapZoomInButtonBounds() const {
  return {43, 176, 64, 64};
}

Rect_t MainScreen::mapZoomOutButtonBounds() const {
  return {43, 252, 64, 64};
}

Rect_t MainScreen::mapPanUpButtonBounds() const {
  return {54, 330, 42, 42};
}

Rect_t MainScreen::mapPanDownButtonBounds() const {
  return {54, 420, 42, 42};
}

Rect_t MainScreen::mapPanLeftButtonBounds() const {
  return {10, 375, 42, 42};
}

Rect_t MainScreen::mapPanRightButtonBounds() const {
  return {98, 375, 42, 42};
}

bool MainScreen::dashboardSlotAt(int32_t x, int32_t y, DashboardSlot& slot) const {
  const DashboardSlot slots[] = {
      DashboardSlot::Left,
      DashboardSlot::Center,
      DashboardSlot::Right,
  };
  for (DashboardSlot candidate : slots) {
    if (pointInRect(dashboardLayoutSlotBounds(candidate), x, y)) {
      slot = candidate;
      return true;
    }
  }
  return false;
}

bool MainScreen::dashboardPresetAt(int32_t x, int32_t y, DashboardWidgetKind& widget) const {
  const DashboardWidgetKind widgets[] = {
      DashboardWidgetKind::Vario,
      DashboardWidgetKind::Compass,
      DashboardWidgetKind::Thermal,
  };
  for (DashboardWidgetKind candidate : widgets) {
    if (pointInRect(dashboardLayoutPresetBounds(candidate), x, y)) {
      widget = candidate;
      return true;
    }
  }
  return false;
}

Rect_t MainScreen::audioAdjustButtonBounds(uint8_t row, bool plus) const {
  const Rect_t& s = layout_.screen;
  const int32_t buttonW = 88;
  const int32_t centerX = s.x + s.width / 2;
  const int32_t y = s.y + 108 + static_cast<int32_t>(row) * 96;
  const int32_t x = plus ? centerX + 132 : centerX - 220;
  return {x, y, buttonW, 46};
}

Rect_t MainScreen::audioSaveButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 200, layout_.trend.y - 68, 220, 52};
}

Rect_t MainScreen::audioResetButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width / 2 + 46, s.y + 396, 220, 46};
}

Rect_t MainScreen::audioVoiceToggleButtonBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + s.width / 2 - 306, s.y + 396, 260, 46};
}

Rect_t MainScreen::audioVolumeSliderBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 230, s.y + 306, s.width - 460, 30};
}

Rect_t MainScreen::audioVolumeSliderTouchBounds() const {
  const Rect_t slider = audioVolumeSliderBounds();
  return {slider.x - 36, slider.y - 28, slider.width + 72, slider.height + 56};
}

Rect_t MainScreen::audioEditorControlsBounds() const {
  const Rect_t& s = layout_.screen;
  return {s.x + 150, s.y + 76, s.width - 300, 372};
}

Rect_t MainScreen::thermalModeButtonBounds(ThermalAssistVisualMode mode) const {
  const Rect_t& s = layout_.screen;
  const int32_t buttonW = 360;
  const int32_t buttonH = 68;
  const int32_t y = s.y + 104;
  if (mode == ThermalAssistVisualMode::PilotCentered) {
    return {s.x + 94, y, buttonW, buttonH};
  }
  return {s.x + s.width - buttonW - 94, y, buttonW, buttonH};
}

void MainScreen::drawButton(const Rect_t& bounds, const char* label, uint8_t scale) {
  const int32_t radius = bounds.height < 40 ? 6 : 8;
  display_.drawRoundRect(bounds, radius, AppConfig::kBlack);
  display_.drawRoundRect({bounds.x + 1, bounds.y + 1, bounds.width - 2, bounds.height - 2}, radius - 1, AppConfig::kBlack);
  const int32_t textY = bounds.y + (bounds.height - 7 * scale) / 2;
  display_.drawSmallTextBoldAligned(label, bounds.x + bounds.width / 2, textY, scale, AppConfig::kBlack, EpdDisplay::Align::Center);
}

void MainScreen::openDashboard() {
  const Page previousPage = activePage_;
  if (previousPage == Page::PilotProfile) {
    savePilotProfileIfDirty();
  }
  if (previousPage == Page::AudioEditor) {
    saveAudioProfileIfDirty();
  }
  if (activePage_ == Page::WifiSettings && wifiManager_) {
    wifiManager_->disableRuntime();
  }
  const bool fromMap = activePage_ == Page::Map;
  const bool fullRefresh = shouldQuickRefreshTransition(previousPage, Page::Dashboard);
  activePage_ = Page::Dashboard;
  powerConfirmVisible_ = false;
  weatherInfoPopupVisible_ = false;
  weatherInfoScrollPage_ = 0;
  thermalCycleInfoPopupVisible_ = false;
  thermalCycleInfoScrollPage_ = 0;
  dashboardLayoutDragActive_ = false;
  configureDashboardWidgetBounds();
  visualState_ = buildVisualState(lastData_);
  visualStateValid_ = true;
  renderStatic(lastData_);
  display_.saveBaseBuffer();
  renderDynamic(lastData_);
  renderLayoutFrame();
  renderDashboardOverlays(lastData_);
  refreshPageTransition(fullRefresh, fromMap);
  partialCycleCount_ = 0;
  pageAutoRefreshMs_ = millis();
  pageFullRefreshMs_ = pageAutoRefreshMs_;
}

void MainScreen::openPage(Page page) {
  const Page previousPage = activePage_;
  if (previousPage == Page::PilotProfile && page != Page::PilotProfile) {
    savePilotProfileIfDirty();
  }
  if (previousPage == Page::AudioEditor && page != Page::AudioEditor) {
    saveAudioProfileIfDirty();
  }
  const bool fullRefresh = shouldQuickRefreshTransition(previousPage, page);
  const bool mapTransition = previousPage == Page::Map || page == Page::Map;
  if (previousPage == Page::WifiSettings && page != Page::WifiSettings && wifiManager_) {
    wifiManager_->disableRuntime();
    wifiLastRefreshMs_ = 0;
    wifiLastState_ = 255;
  }
  if (page == Page::WifiSettings && wifiManager_) {
    pauseBleBeforeWifi(tracklogBle_);
    wifiManager_->enableRuntime(true);
    if (wifiSelectedSsid_.length() == 0) {
      wifiSelectedSsid_ = wifiManager_->currentSsid();
    }
    wifiLastRefreshMs_ = 0;
    wifiLastState_ = 255;
  }
  activePage_ = page;
  powerConfirmVisible_ = false;
  if (page != Page::DashboardLayout) {
    dashboardLayoutDragActive_ = false;
  }
  if (page != Page::Storage) {
    storageClearConfirm_ = false;
  } else if (storageManager_) {
    storageManager_->refresh();
  }
  if (page != Page::AdvancedSystem) {
    advancedConfirmAction_ = TouchAction::None;
    advancedNotice_[0] = '\0';
  }
  if (page != Page::WeatherStation) {
    weatherInfoPopupVisible_ = false;
    weatherInfoScrollPage_ = 0;
  }
  if (page != Page::ThermalAssistSettings) {
    thermalInfoPopupVisible_ = false;
    thermalInfoScrollPage_ = 0;
  }
  if (page != Page::ThermalCycleBeta) {
    thermalCycleInfoPopupVisible_ = false;
    thermalCycleInfoScrollPage_ = 0;
  }
  if (page != Page::MapDownload) {
    mapDownloadLastProgress_ = 255;
    mapDownloadLastState_ = 255;
  }
  if (page == Page::ThermalCycleBeta && previousPage != Page::ThermalCycleBeta) {
    thermalCycleBeta_.reset();
    thermalCycleInfoPopupVisible_ = false;
    thermalCycleInfoScrollPage_ = 0;
  }
  if (page != Page::Dashboard && page != Page::Map) {
    lastData_.audioEnabled = false;
  }
  if (page == Page::WeatherStation && !weatherStationSleepMode_) {
    if (previousPage != Page::WeatherLocation) {
      weatherDayIndex_ = 0;
      requestWeatherForActiveLocation();
    }
  }
  if (page == Page::Map) {
    syncMapPageData(lastData_);
    display_.clearBuffer(AppConfig::kWhite);
    if (mapPage_.restorePreparedBaseCache()) {
      renderPageFooter(Page::Map);
      mapPage_.clearBaseDirty();
    } else {
      renderMapPageBase();
    }
    display_.saveBaseBuffer();
    renderMapPageDynamic(true);
    refreshPageTransition(fullRefresh, mapTransition);
    partialCycleCount_ = 0;
    pageAutoRefreshMs_ = millis();
    pageFullRefreshMs_ = pageAutoRefreshMs_;
    mapDynamicRefreshMs_ = pageAutoRefreshMs_;
    registerPageTouchZones();
    return;
  }
  renderPage(page);
  display_.saveBaseBuffer();
  refreshPageTransition(fullRefresh, mapTransition);
  partialCycleCount_ = 0;
  pageAutoRefreshMs_ = millis();
  pageFullRefreshMs_ = pageAutoRefreshMs_;
  registerPageTouchZones();
}

void MainScreen::openTracklogDetails(const char* filepath) {
  const Page previousPage = activePage_;
  if (filepath && filepath[0] != '\0') {
    snprintf(selectedTracklogPath_, sizeof(selectedTracklogPath_), "%s", filepath);
  }
  tracklogDeleteConfirm_ = false;
  activePage_ = Page::TracklogDetails;
  lastData_.audioEnabled = false;
  renderTracklogDetailsPage();
  display_.saveBaseBuffer();
  refreshPageTransition(shouldQuickRefreshTransition(previousPage, Page::TracklogDetails));
  partialCycleCount_ = 0;
  pageAutoRefreshMs_ = millis();
  pageFullRefreshMs_ = pageAutoRefreshMs_;
  registerPageTouchZones();
}

void MainScreen::refreshPageTransition(bool fullRefresh, bool reinforce) {
  if (fullRefresh) {
    display_.quickFullRefresh();
    markFooterDisplayed(activePage_);
    return;
  }
  Rect_t area = layout_.screen;
  if (reinforce) {
    Rect_t reinforceArea = {layout_.screen.x, layout_.screen.y, layout_.screen.width, layout_.trend.y - layout_.screen.y};
    display_.updateAreasAndReinforce(&area, 1, &reinforceArea, 1);
  } else {
    display_.updateAreas(&area, 1);
  }
  markFooterDisplayed(activePage_);
}

bool MainScreen::pageUsesFooterSettings(Page page) const {
  return page == Page::Map || page == Page::Tracklog;
}

bool MainScreen::shouldQuickRefreshTransition(Page previousPage, Page nextPage) const {
  if (previousPage == nextPage) return false;
  const bool previousMain = previousPage == Page::Dashboard || previousPage == Page::Settings || previousPage == Page::Map ||
                            previousPage == Page::Tracklog || previousPage == Page::TracklogDetails;
  const bool nextMain = nextPage == Page::Dashboard || nextPage == Page::Settings || nextPage == Page::Map ||
                        nextPage == Page::Tracklog || nextPage == Page::TracklogDetails;
  return previousMain && nextMain;
}

bool MainScreen::refreshIdlePageFullIfDue(uint32_t now) {
  if (activePage_ == Page::Dashboard || activePage_ == Page::Map) {
    return false;
  }
  if (pageFullRefreshMs_ == 0) {
    pageFullRefreshMs_ = now;
    return false;
  }
  const uint32_t refreshIntervalMs =
      activePage_ == Page::ThermalCycleBeta ? kThermalCyclePageFullRefreshMs : kIdlePageFullRefreshMs;
  if (now - pageFullRefreshMs_ < refreshIntervalMs) {
    return false;
  }

  Serial.printf("UI: full refresh preventivo pagina=%u apos %lu ms.\n",
                static_cast<unsigned>(activePage_),
                static_cast<unsigned long>(now - pageFullRefreshMs_));
  if (activePage_ == Page::ThermalCycleBeta) {
    thermalCycleBeta_.markArtifact(now, 45UL * 1000UL);
  }
  refreshActivePage(true);
  return true;
}

void MainScreen::refreshActivePage(bool fullRefresh) {
  if (activePage_ == Page::Map) {
    refreshMapPage(true, fullRefresh);
    pageAutoRefreshMs_ = millis();
    if (fullRefresh) {
      pageFullRefreshMs_ = pageAutoRefreshMs_;
    }
    mapDynamicRefreshMs_ = pageAutoRefreshMs_;
    registerPageTouchZones();
    return;
  }
  renderPage(activePage_);
  display_.saveBaseBuffer();
  if (fullRefresh) {
    display_.fullRefresh();
    markFooterDisplayed(activePage_);
  } else {
    refreshPageTransition();
  }
  pageAutoRefreshMs_ = millis();
  if (fullRefresh) {
    pageFullRefreshMs_ = pageAutoRefreshMs_;
  }
  registerPageTouchZones();
}

void MainScreen::refreshActivePageArea(const Rect_t& area) {
  refreshActivePageAreas(&area, 1);
}

void MainScreen::refreshActivePageAreas(const Rect_t* areas, size_t count, bool updateTouchZones) {
  if (!areas || count == 0) {
    return;
  }
  renderPage(activePage_);
  display_.saveBaseBuffer();
  display_.updateAreas(areas, count);
  pageAutoRefreshMs_ = millis();
  if (updateTouchZones) {
    registerPageTouchZones();
  }
}

void MainScreen::refreshAudioEditorControls() {
  if (activePage_ != Page::AudioEditor) {
    return;
  }
  refreshActivePageArea(audioEditorControlsBounds());
}

void MainScreen::refreshWifiStatusArea() {
  if (activePage_ != Page::WifiSettings) {
    return;
  }
  refreshActivePageArea(wifiStatusAreaBounds());
  wifiLastRefreshMs_ = millis();
}

void MainScreen::refreshWifiKeyboardArea() {
  if (activePage_ != Page::WifiSettings) {
    return;
  }
  refreshActivePageArea(wifiKeyboardAreaBounds());
  wifiLastRefreshMs_ = millis();
}

void MainScreen::refreshPilotProfileKeyboardArea() {
  if (activePage_ != Page::PilotProfile) {
    return;
  }
  refreshActivePageArea(profileKeyboardAreaBounds());
}

void MainScreen::refreshPilotProfileSelection(uint8_t previousIndex, uint8_t nextIndex) {
  if (activePage_ != Page::PilotProfile) {
    return;
  }
  if (previousIndex == nextIndex) {
    return;
  }
  Rect_t areas[2] = {
      profileFieldBounds(previousIndex),
      profileFieldBounds(nextIndex),
  };
  refreshActivePageAreas(areas, 2);
}

void MainScreen::renderPage(Page page) {
  if (page == Page::AudioEditor) {
    renderAudioEditorPage();
    return;
  }
  if (page == Page::DashboardLayout) {
    renderDashboardLayoutPage();
    return;
  }
  if (page == Page::ThermalAssistSettings) {
    renderThermalAssistSettingsPage();
    return;
  }
  if (page == Page::DeviceInfo) {
    renderDeviceInfoPage();
    return;
  }
  if (page == Page::SystemStatus) {
    renderSystemStatusPage();
    return;
  }
  if (page == Page::Manual) {
    renderManualPage();
    return;
  }
  if (page == Page::ManualLogic) {
    renderManualLogicPage();
    return;
  }
  if (page == Page::ManualUser) {
    renderManualUserPage();
    return;
  }
  if (page == Page::Storage) {
    renderStoragePage();
    return;
  }
  if (page == Page::AdvancedSystem) {
    renderAdvancedSystemPage();
    return;
  }
  if (page == Page::MapDownload) {
    renderMapDownloadPage();
    return;
  }
  if (page == Page::WifiSettings) {
    renderWifiSettingsPage();
    return;
  }
  if (page == Page::FirmwareUpdate) {
    renderFirmwareUpdatePage();
    return;
  }
  if (page == Page::WeatherStation) {
    renderWeatherStationPage();
    return;
  }
  if (page == Page::WeatherLocation) {
    renderWeatherLocationPage();
    return;
  }
  if (page == Page::PilotProfile) {
    renderPilotProfilePage();
    return;
  }
  if (page == Page::ThermalCycleBeta) {
    renderThermalCycleBetaPage();
    return;
  }
  if (page == Page::Tracklog) {
    renderTracklogPage();
    return;
  }
  if (page == Page::TracklogDetails) {
    renderTracklogDetailsPage();
    return;
  }
  if (page == Page::Map) {
    renderMapPageBase();
    renderMapPageDynamic(true);
    return;
  }

  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height, AppConfig::kBlack);

  const char* title = "CONFIGURAÇOES DO BRVARIO";
  const char* subtitle = nullptr;
  if (page == Page::Customize) {
    title = "PERSONALIZAÇAO";
    subtitle = "Teste de pagina para layout, widgets e atalhos";
  }

  display_.drawSmallTextBoldAligned(title, s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  if (subtitle && subtitle[0] != '\0') {
    display_.drawSmallTextAligned(subtitle, s.x + s.width / 2, s.y + 64, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  }

  const int32_t left = s.x + 64;
  const int32_t top = s.y + 112;
  const int32_t colW = 390;
  const int32_t rowH = 64;

  if (page == Page::Settings) {
    display_.drawSmallTextBoldAligned("USUARIO", s.x + s.width / 4, s.y + 86, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
    display_.drawSmallTextBoldAligned("SISTEMA", s.x + (s.width * 3) / 4, s.y + 86, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
    display_.drawLine(s.x + s.width / 2, s.y + 104, s.x + s.width / 2, layout_.trend.y - 8, AppConfig::kBlack);
  } else {
    for (uint8_t i = 0; i < 5; ++i) {
      const int32_t y = top + rowH * i;
      display_.drawLine(left, y + rowH - 10, left + colW * 2 + 34, y + rowH - 10, AppConfig::kBlack);
    }
  }

  if (page == Page::Settings) {
    drawButton(settingsDashboardLayoutButtonBounds(), "PERSONALIZAR TELA INICIAL", 2);
    drawButton(settingsThermalAssistButtonBounds(), "ASSISTENTE DE TERMICA", 2);
    drawButton(settingsAudioEditorButtonBounds(), "EDITOR DE AUDIO", 2);
    drawButton(settingsWeatherStationButtonBounds(), "ESTAÇAO METEOROLOGICA", 2);
    drawButton(settingsPilotProfileButtonBounds(), "DADOS DO PILOTO", 2);
    drawButton(settingsThermalCycleButtonBounds(), "CICLO TERMAL BETA", 2);
    drawButton(settingsManualButtonBounds(), "MANUAL", 2);
    drawButton(settingsWifiButtonBounds(), "REDE WIFI", 2);
    drawButton(settingsFirmwareUpdateButtonBounds(), "ATUALIZAÇAO", 2);
    drawButton(settingsStorageButtonBounds(), "BAIXAR MAPAS", 2);
    drawButton(settingsDeviceInfoButtonBounds(), "INFORMAÇAO", 2);
    drawButton(settingsSystemStatusButtonBounds(), "STATUS", 2);
    drawButton(settingsAdvancedSystemButtonBounds(), "AVANÇADO", 2);
  } else if (page == Page::Customize) {
    display_.drawSmallTextBold("PAGINA PRINCIPAL", left, top, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned("HORIZONTAL", left + colW, top, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
    display_.drawSmallTextBold("WIDGET CENTRAL", left, top + rowH, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned("VARIO", left + colW, top + rowH, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
    display_.drawSmallTextBold("COLUNA ESQUERDA", left, top + rowH * 2, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned("TERMICA", left + colW, top + rowH * 2, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
    display_.drawSmallTextBold("ATALHOS TOUCH", left, top + rowH * 3, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned("5 BOTOES", left + colW, top + rowH * 3, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
  } else {
    display_.drawSmallTextBold("STATUS", left, top, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned("PRONTO", left + colW, top, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
    display_.drawSmallTextBold("FORMATO", left, top + rowH, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned("GPX", left + colW, top + rowH, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
    display_.drawSmallTextBold("PONTOS", left, top + rowH * 2, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned("SIMULADO", left + colW, top + rowH * 2, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
    display_.drawSmallTextBold("MEMORIA", left, top + rowH * 3, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned("OK", left + colW, top + rowH * 3, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
  }

  renderPageFooter(page);
}

void MainScreen::syncMapPageData(const VarioData& data) {
  const bool trackingEnabled = data.trackingEnabled || (flightRecorder_ && flightRecorder_->recording());
  if (lastData_.trackingEnabled != trackingEnabled) {
    lastData_.trackingEnabled = trackingEnabled;
  }

  if (data.gpsFix) {
    mapPage_.setPosition(data.latitudeDeg, data.longitudeDeg);
    mapPage_.setHeading(data.courseDeg);
    if (data.windQuality != WindQuality::None && data.windSpeedKmh > 0.5F) {
      mapPage_.setWind(data.windDirectionDeg, data.windSpeedKmh);
    } else {
      mapPage_.clearWind();
    }
    mapPage_.setFlightData(data.altitudeM,
                           data.altitudeAglM,
                           data.groundSpeedKmh,
                           data.varioMs,
                           data.glideRatio,
                           data.ganhoTermicaM,
                           data.elapsedSeconds);
    mapPage_.setThermalOverlay(data.thermalPoints,
                               data.thermalPointCount,
                               data.thermalHistory,
                               data.thermalHistoryCount,
                               data.thermalPilotEastM,
                               data.thermalPilotNorthM,
                               data.thermalCoreConfidencePercent);
  } else {
    mapPage_.clearWind();
    mapPage_.setFlightData(data.altitudeM,
                           data.altitudeAglM,
                           0.0F,
                           data.varioMs,
                           0.0F,
                           0.0F,
                           trackingEnabled ? data.elapsedSeconds : 0UL);
    mapPage_.setThermalOverlay(nullptr, 0, nullptr, 0, 0.0F, 0.0F, 0);
  }
  mapPage_.setSystemStatus(data.satellites, data.batteryPercent, data.gpsFix, trackingEnabled, data.timeOfDaySeconds);
}

void MainScreen::renderMapPageBase() {
  display_.clearBuffer(AppConfig::kWhite);
  mapPage_.drawBase();
  renderPageFooter(Page::Map);
  mapPage_.savePreparedBaseCache();
  mapPage_.clearBaseDirty();
}

void MainScreen::renderMapPageDynamic(bool renderFooter) {
  mapPage_.drawDynamic();
  if (renderFooter) {
    renderPageFooter(Page::Map);
  }
  mapPage_.clearDynamicDirty();
}

void MainScreen::refreshMapPage(bool forceBase, bool fullRefresh) {
  const bool footerNeedsUpdate = forceBase || fullRefresh || footerNeedsPanelUpdate(Page::Map);
  if (forceBase) {
    if (mapPage_.restorePreparedBaseCache()) {
      renderPageFooter(Page::Map);
      mapPage_.clearBaseDirty();
    } else {
      renderMapPageBase();
    }
    display_.saveBaseBuffer();
  } else {
    display_.restoreBaseBuffer();
  }
  renderMapPageDynamic(footerNeedsUpdate);

  Rect_t areas[2];
  size_t areaCount = 0;
  areas[areaCount++] = mapPage_.dynamicBounds();
  if (footerNeedsUpdate) {
    areas[areaCount++] = footerDynamicBounds();
  }
  if (fullRefresh) {
    display_.fullRefresh();
  } else {
    Rect_t reinforceAreas[2];
    size_t reinforceCount = 0;
    if ((partialCycleCount_ % AppConfig::kGlobalContrastEveryCycles) == 0) {
      reinforceAreas[reinforceCount++] = mapPage_.mapBounds();
    }
    display_.updateAreasAndReinforce(areas, areaCount, reinforceAreas, reinforceCount);
  }
  if (footerNeedsUpdate) {
    markFooterDisplayed(Page::Map);
  }
  pageAutoRefreshMs_ = millis();
  if (fullRefresh) {
    pageFullRefreshMs_ = pageAutoRefreshMs_;
  }
}

void MainScreen::prepareMapPageBaseCache(bool force) {
  if (!display_.isReady()) {
    return;
  }

  const uint32_t now = millis();
  if (!force && now - mapBasePrepareMs_ < 2500UL) {
    return;
  }

  syncMapPageData(lastData_);
  if (!force && mapPage_.hasPreparedBaseCache() && !mapPage_.needsBaseRedraw()) {
    return;
  }

  mapBasePrepareMs_ = now;
  display_.clearBuffer(AppConfig::kWhite);
  mapPage_.drawBase();
  mapPage_.savePreparedBaseCache();
  mapPage_.clearBaseDirty();

  if (activePage_ == Page::Dashboard) {
    display_.restoreBaseBuffer();
    renderDynamic(lastData_);
    renderLayoutFrame();
    renderDashboardOverlays(lastData_);
  }
}

void MainScreen::renderTracklogPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  display_.drawSmallTextBold("TRACKLOG", s.x + 64, s.y + 12, 3, AppConfig::kBlack);
  display_.drawSmallTextAligned("ARQUIVOS IGC LOCAL E CARTAO SD", s.x + 292, s.y + 20, 2, AppConfig::kBlack, EpdDisplay::Align::Left);

  const bool bleVisible = tracklogBle_ && tracklogBle_->visibleStatus();
  const uint8_t pageSize = 5;
  FlightRecorder::TracklogEntry pageEntries[pageSize] = {};
  uint8_t entryCount = 0;
  uint16_t total = 0;
  uint32_t usedBytes = 0;
  if (flightRecorder_ && flightRecorder_->storageReady()) {
    flightRecorder_->tracklogPage(tracklogPage_, pageSize, pageEntries, entryCount, total, usedBytes);
  }
  const uint16_t maxPage = total == 0 ? 0 : static_cast<uint16_t>((total - 1) / pageSize);
  if (tracklogPage_ > maxPage) {
    tracklogPage_ = maxPage;
    entryCount = 0;
    if (flightRecorder_ && flightRecorder_->storageReady()) {
      flightRecorder_->tracklogPage(tracklogPage_, pageSize, pageEntries, entryCount, total, usedBytes);
    }
  }

  char statusLine[72];
  const char* statusSource = bleVisible ? tracklogBle_->statusText()
                                        : (tracklogNotice_[0] != '\0' ? tracklogNotice_
                                                                      : (flightRecorder_ ? flightRecorder_->statusText() : "NAO INICIADO"));
  snprintf(statusLine, sizeof(statusLine), "%s", statusSource ? statusSource : "");
  display_.drawSmallTextBold(bleVisible ? "BLE" : "STATUS", s.x + 64, s.y + 70, 2, AppConfig::kBlack);
  display_.drawSmallTextBold(statusLine, s.x + 184, s.y + 70, strlen(statusLine) > 34 ? 1 : 2, AppConfig::kBlack);
  char totalText[20];
  snprintf(totalText, sizeof(totalText), "VOOS: %u", static_cast<unsigned>(total));
  display_.drawSmallTextBoldAligned(totalText, s.x + s.width - 64, s.y + 70, 2, AppConfig::kBlack, EpdDisplay::Align::Right);

  if (!flightRecorder_ || !flightRecorder_->storageReady()) {
    display_.drawSmallTextBoldAligned("MEMORIA LITTLEFS INDISPONIVEL", s.x + s.width / 2, s.y + 230, 3, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    renderPageFooter(Page::Tracklog);
    return;
  }

  const int32_t left = s.x + 64;
  const int32_t right = s.x + s.width - 64;
  const int32_t memoryTop = s.y + 100;
  const int32_t storageGap = 28;
  const int32_t storageW = (right - left - storageGap) / 2;
  const auto drawStorageBar = [&](int32_t x, const char* label, uint64_t used, uint64_t totalBytes, bool available) {
    uint32_t percentValue = (available && totalBytes > 0) ? static_cast<uint32_t>((used * 100ULL) / totalBytes) : 0;
    if (percentValue > 100UL) percentValue = 100UL;
    const uint8_t percent = static_cast<uint8_t>(percentValue);
    char title[28];
    char value[32];
    if (available) {
      snprintf(title, sizeof(title), "%s %u%%", label, static_cast<unsigned>(percent));
      char usedText[14];
      char totalTextLocal[14];
      formatStorageSize(used, usedText, sizeof(usedText));
      formatStorageSize(totalBytes, totalTextLocal, sizeof(totalTextLocal));
      snprintf(value, sizeof(value), "%s/%s", usedText, totalTextLocal);
    } else {
      snprintf(title, sizeof(title), "%s ---", label);
      snprintf(value, sizeof(value), "SEM CARTAO");
    }

    display_.drawSmallTextBold(title, x, memoryTop, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned(value, x + storageW, memoryTop, strlen(value) > 15 ? 1 : 2, AppConfig::kBlack,
                                      EpdDisplay::Align::Right);
    const Rect_t bar = {x, memoryTop + 27, storageW, 14};
    display_.drawRect(bar, AppConfig::kBlack);
    const int32_t fillW = ((bar.width - 4) * percent) / 100;
    if (fillW > 0) {
      display_.fillRect({bar.x + 2, bar.y + 2, fillW, bar.height - 4}, AppConfig::kBlack);
    }
  };

  const uint32_t localTotalBytes = flightRecorder_->storageTotalBytes();
  drawStorageBar(left, "LOCAL", usedBytes, localTotalBytes, true);
  const bool sdMounted = storageManager_ && storageManager_->mounted() && storageManager_->totalBytes() > 0;
  drawStorageBar(left + storageW + storageGap,
                 "SD",
                 sdMounted ? storageManager_->usedBytes() : 0,
                 sdMounted ? storageManager_->totalBytes() : 0,
                 sdMounted);

  const int32_t listTop = s.y + 178;
  const int32_t deleteX = tracklogRowDeleteButtonBounds(0).x;
  const int32_t sizeX = deleteX - 18;
  display_.drawSmallTextBold("DATA / HORA", left, listTop - 24, 2, AppConfig::kBlack);
  display_.drawSmallTextBold("ORIGEM", left + 230, listTop - 24, 2, AppConfig::kBlack);
  display_.drawSmallTextBoldAligned("TAMANHO", sizeX, listTop - 24, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
  display_.drawSmallTextBoldAligned("APAGAR", right - 10, listTop - 24, 2, AppConfig::kBlack, EpdDisplay::Align::Right);

  if (total == 0) {
    display_.drawSmallTextBoldAligned("NENHUM VOO GRAVADO AINDA", s.x + s.width / 2, s.y + 246, 3, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    display_.drawSmallTextAligned("O IGC SERA CRIADO AUTOMATICAMENTE NA DECOLAGEM", s.x + s.width / 2, s.y + 300, 2,
                                  AppConfig::kBlack, EpdDisplay::Align::Center);
  } else {
    for (uint8_t i = 0; i < entryCount; ++i) {
      const FlightRecorder::TracklogEntry& entry = pageEntries[i];
      const Rect_t rowBounds = tracklogRowBounds(i);
      display_.drawLine(rowBounds.x, rowBounds.y + rowBounds.height - 1, rowBounds.x + rowBounds.width, rowBounds.y + rowBounds.height - 1,
                        AppConfig::kBlack);
      char dateTime[32];
      snprintf(dateTime,
               sizeof(dateTime),
               "%s  %s",
               entry.displayDate[0] != '\0' ? entry.displayDate : "--/--/--",
               entry.displayTime[0] != '\0' ? entry.displayTime : "--:--");
      display_.drawSmallTextBold(dateTime, left, rowBounds.y + 9, 2, AppConfig::kBlack);
      display_.drawSmallTextBold(entry.location[0] != '\0' ? entry.location : "LOCAL", left + 230, rowBounds.y + 9, 2, AppConfig::kBlack);
      char sizeText[16];
      formatFileSize(entry.sizeBytes, sizeText, sizeof(sizeText));
      display_.drawSmallTextBoldAligned(sizeText, sizeX, rowBounds.y + 8, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
      drawButton(tracklogRowDeleteButtonBounds(i), "APAGAR", 2);
    }
  }

  char pageText[24];
  snprintf(pageText, sizeof(pageText), "%u/%u", static_cast<unsigned>(tracklogPage_ + 1), static_cast<unsigned>(maxPage + 1));
  display_.drawSmallTextBoldAligned(pageText, s.x + s.width / 2, layout_.trend.y - 82, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  drawButton(tracklogPrevButtonBounds(), "ANTERIOR", 2);
  drawButton(tracklogSyncButtonBounds(), "SINCRONIZAR COM APP", 2);
  drawButton(tracklogNextButtonBounds(), "PROXIMO", 2);

  renderTracklogBleStatusBox();

  if (tracklogDeleteConfirm_) {
    const Rect_t popup = tracklogConfirmPopupBounds();
    display_.fillRect(popup, AppConfig::kWhite);
    display_.drawRect(popup, AppConfig::kBlack);
    display_.drawRect({popup.x + 2, popup.y + 2, popup.width - 4, popup.height - 4}, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned("APAGAR ESTE VOO?", popup.x + popup.width / 2, popup.y + 24, 3, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    const char* deletingName = fileNameFromPath(selectedTracklogPath_);
    display_.drawSmallTextBoldAligned(deletingName && deletingName[0] != '\0' ? deletingName : "ARQUIVO SELECIONADO",
                                      popup.x + popup.width / 2,
                                      popup.y + 72,
                                      strlen(deletingName) > 34 ? 1 : 2,
                                      AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    display_.drawSmallTextAligned("CONFIRME A EXCLUSAO DO TRACKLOG", popup.x + popup.width / 2, popup.y + 94, 1, AppConfig::kBlack,
                                  EpdDisplay::Align::Center);
    drawButton(tracklogConfirmYesButtonBounds(), "SIM", 3);
    drawButton(tracklogConfirmNoButtonBounds(), "NAO", 3);
  }

  renderPageFooter(Page::Tracklog);
}

void MainScreen::renderTracklogDetailsPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  display_.drawSmallTextBoldAligned("ESTATISTICA DO VOO", s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  if (tracklogNotice_[0] != '\0') {
    display_.drawSmallTextBoldAligned(tracklogNotice_, s.x + s.width / 2, s.y + 62, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  } else {
    display_.drawSmallTextAligned("RESUMO CALCULADO PELO ARQUIVO IGC", s.x + s.width / 2, s.y + 62, 2, AppConfig::kBlack,
                                  EpdDisplay::Align::Center);
  }

  FlightRecorder::TracklogStats stats;
  const bool ok = flightRecorder_ && flightRecorder_->tracklogStats(selectedTracklogPath_, stats);
  if (!ok) {
    display_.drawSmallTextBoldAligned("NAO FOI POSSIVEL LER O ARQUIVO", s.x + s.width / 2, s.y + 220, 3, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    drawButton(tracklogDeleteButtonBounds(), "APAGAR", 2);
    drawButton(tracklogExportButtonBounds(), "EXPORTAR", 2);
    renderPageFooter(Page::TracklogDetails);
    return;
  }

  const int32_t leftX = s.x + 62;
  const int32_t rightEdge = s.x + s.width - 62;
  const int32_t top = s.y + 88;
  const int32_t metricTop = s.y + 130;
  const int32_t colW = (s.width - 124) / 3;
  const int32_t rowH = 72;

  char value[32];
  char fileTitle[56];
  snprintf(fileTitle, sizeof(fileTitle), "%s [%s]", stats.name, stats.location[0] != '\0' ? stats.location : "LOCAL");
  display_.drawSmallTextBold("ARQUIVO", leftX, top, 2, AppConfig::kBlack);
  display_.drawSmallTextBoldAligned(fileTitle, rightEdge, top, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
  display_.drawLine(leftX, top + 30, rightEdge, top + 30, AppConfig::kBlack);
  display_.drawLine(leftX + colW, metricTop - 4, leftX + colW, layout_.trend.y - 72, AppConfig::kBlack);
  display_.drawLine(leftX + colW * 2, metricTop - 4, leftX + colW * 2, layout_.trend.y - 72, AppConfig::kBlack);

  const auto drawMetric = [&](uint8_t col, uint8_t row, const char* label, const char* text) {
    const int32_t x = leftX + static_cast<int32_t>(col) * colW + 14;
    const int32_t y = metricTop + static_cast<int32_t>(row) * rowH;
    display_.drawSmallTextBold(label, x, y, 2, AppConfig::kBlack);
    display_.drawSmallTextBold(text, x, y + 28, 2, AppConfig::kBlack);
  };

  formatLocalTimeOfDayText(stats.startUtcSeconds, value, sizeof(value));
  drawMetric(0, 0, "INICIO LOCAL", value);
  formatLocalTimeOfDayText(stats.endUtcSeconds, value, sizeof(value));
  drawMetric(1, 0, "FIM LOCAL", value);
  formatDurationText(stats.durationSeconds, value, sizeof(value));
  drawMetric(2, 0, "DURAÇAO", value);
  display_.drawLine(leftX, metricTop + rowH - 8, rightEdge, metricTop + rowH - 8, AppConfig::kBlack);

  snprintf(value, sizeof(value), "%.2f KM", static_cast<double>(stats.distanceKm));
  drawMetric(0, 1, "DISTANCIA", value);
  snprintf(value, sizeof(value), "%.0f KM/H", static_cast<double>(stats.averageGroundSpeedKmh));
  drawMetric(1, 1, "VEL MEDIA", value);
  snprintf(value, sizeof(value), "%.0f KM/H", static_cast<double>(stats.maxGroundSpeedKmh));
  drawMetric(2, 1, "VEL MAX", value);
  display_.drawLine(leftX, metricTop + rowH * 2 - 8, rightEdge, metricTop + rowH * 2 - 8, AppConfig::kBlack);

  snprintf(value, sizeof(value), "%.0f M", static_cast<double>(stats.maxGpsAltM));
  drawMetric(0, 2, "ALT GPS MAX", value);
  snprintf(value, sizeof(value), "%.0f M", static_cast<double>(stats.minGpsAltM));
  drawMetric(1, 2, "ALT GPS MIN", value);
  snprintf(value, sizeof(value), "%.0f M", static_cast<double>(stats.maxPressAltM));
  drawMetric(2, 2, "ALT BARO MAX", value);
  display_.drawLine(leftX, metricTop + rowH * 3 - 8, rightEdge, metricTop + rowH * 3 - 8, AppConfig::kBlack);

  snprintf(value, sizeof(value), "%u", static_cast<unsigned>(stats.fixCount));
  drawMetric(0, 3, "PONTOS", value);
  snprintf(value, sizeof(value), "%.1f M/S", static_cast<double>(stats.minVarioMs));
  drawMetric(1, 3, "VARIO MIN", value);
  snprintf(value, sizeof(value), "+%.1f M/S", static_cast<double>(stats.maxVarioMs));
  drawMetric(2, 3, "VARIO MAX", value);

  drawButton(tracklogDeleteButtonBounds(), "APAGAR", 2);
  drawButton(tracklogExportButtonBounds(), "EXPORTAR", 2);
  renderTracklogBleStatusBox();

  if (tracklogDeleteConfirm_) {
    const Rect_t popup = tracklogConfirmPopupBounds();
    display_.fillRect(popup, AppConfig::kWhite);
    display_.drawRect(popup, AppConfig::kBlack);
    display_.drawRect({popup.x + 2, popup.y + 2, popup.width - 4, popup.height - 4}, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned("APAGAR ESTE VOO?", popup.x + popup.width / 2, popup.y + 24, 3, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    const char* deletingName = fileNameFromPath(selectedTracklogPath_);
    display_.drawSmallTextBoldAligned(deletingName && deletingName[0] != '\0' ? deletingName : "ARQUIVO SELECIONADO",
                                      popup.x + popup.width / 2,
                                      popup.y + 72,
                                      strlen(deletingName) > 34 ? 1 : 2,
                                      AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    display_.drawSmallTextAligned("ESSA AÇAO NAO PODE SER DESFEITA", popup.x + popup.width / 2, popup.y + 94, 1, AppConfig::kBlack,
                                  EpdDisplay::Align::Center);
    drawButton(tracklogConfirmYesButtonBounds(), "SIM", 3);
    drawButton(tracklogConfirmNoButtonBounds(), "NAO", 3);
  }

  renderPageFooter(Page::TracklogDetails);
}

void MainScreen::renderTracklogBleStatusBox() {
  if (!tracklogBle_ || !tracklogBle_->visibleStatus()) {
    return;
  }

  const Rect_t& s = layout_.screen;
  const Rect_t box = tracklogBleStatusPopupBounds();
  display_.fillRect(box, AppConfig::kWhite);
  display_.drawRect(box, AppConfig::kBlack);
  display_.drawRect({box.x + 2, box.y + 2, box.width - 4, box.height - 4}, AppConfig::kBlack);

  if (tracklogBle_->canCancelPending()) {
    const Rect_t cancel = tracklogBleCancelButtonBounds();
    drawButton(cancel, "X", 2);
  }

  const char* status = tracklogBle_->statusText();
  const uint8_t statusScale = strlen(status) > 22 ? 2 : 3;
  display_.drawSmallTextBoldAligned(status, box.x + box.width / 2, box.y + 20, statusScale, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);

  char detail[64];
  if (tracklogBle_->syncAllActive() || tracklogBle_->syncTotal() > 0) {
    snprintf(detail,
             sizeof(detail),
             "ARQUIVOS %u/%u",
             static_cast<unsigned>(tracklogBle_->syncSentCount()),
             static_cast<unsigned>(tracklogBle_->syncTotal()));
  } else if (tracklogBle_->currentFileName()[0] != '\0') {
    snprintf(detail, sizeof(detail), "%s", tracklogBle_->currentFileName());
  } else if (tracklogBle_->connected()) {
    snprintf(detail, sizeof(detail), "APP CONECTADO");
  } else {
    snprintf(detail, sizeof(detail), "ABRA O APP BRVARIO");
  }
  display_.drawSmallTextBoldAligned(detail, box.x + box.width / 2, box.y + 78, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  const Rect_t bar = {box.x + 64, box.y + 126, box.width - 128, 26};
  const uint8_t progress = tracklogBle_->progressPercent();
  display_.drawRect(bar, AppConfig::kBlack);
  const int32_t fillW = ((bar.width - 4) * progress) / 100;
  if (fillW > 0) {
    display_.fillRect({bar.x + 2, bar.y + 2, fillW, bar.height - 4}, AppConfig::kBlack);
  }
  char percentText[8];
  snprintf(percentText, sizeof(percentText), "%u%%", static_cast<unsigned>(progress));
  display_.drawSmallTextBoldAligned(percentText, box.x + box.width / 2, bar.y + 38, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  if (!tracklogBle_->canCancelPending() && (tracklogBle_->activeTransfer() || tracklogBle_->syncAllActive())) {
    display_.drawSmallTextAligned("ENVIO INICIADO: AGUARDE CONCLUIR", box.x + box.width / 2, box.y + box.height - 22, 1,
                                  AppConfig::kBlack, EpdDisplay::Align::Center);
  }
}

void MainScreen::renderAudioEditorPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height, AppConfig::kBlack);

  display_.drawSmallTextBoldAligned("EDITOR DE AUDIO", s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("PERFIL DE SOM DO VARIO", s.x + s.width / 2, s.y + 58, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  if (!audioBuzzer_) {
    display_.drawSmallTextBoldAligned("BUZZER NAO INICIADO", s.x + s.width / 2, s.y + 220, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
    renderPageFooter(Page::AudioEditor);
    return;
  }

  const VarioBuzzer::AudioProfile profile = audioBuzzer_->profileSnapshot();
  char levelText[32];

  display_.drawSmallTextBoldAligned("RESPOSTA DO VARIO", s.x + s.width / 2, s.y + 82, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  display_.drawSmallTextBoldAligned("AJUSTE CADENCIA DO VARIO", s.x + s.width / 2, s.y + 102, 1, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  drawButton(audioAdjustButtonBounds(0, false), "-", 4);
  drawButton(audioAdjustButtonBounds(0, true), "+", 4);
  formatLevelText(levelText, sizeof(levelText), responseLevelLabel(profile.responseLevel), profile.responseLevel);
  display_.drawSmallTextBoldAligned(levelText, s.x + s.width / 2, s.y + 120, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  display_.drawSmallTextBoldAligned("TOM GERAL", s.x + s.width / 2, s.y + 186, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  drawButton(audioAdjustButtonBounds(1, false), "-", 4);
  drawButton(audioAdjustButtonBounds(1, true), "+", 4);
  formatLevelText(levelText, sizeof(levelText), pitchLevelLabel(profile.pitchLevel), profile.pitchLevel);
  display_.drawSmallTextBoldAligned(levelText, s.x + s.width / 2, s.y + 216, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  display_.drawSmallTextBoldAligned("VOLUME DO BEEP", s.x + s.width / 2, s.y + 272, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  const Rect_t slider = audioVolumeSliderBounds();
  display_.drawRect(slider, AppConfig::kBlack);
  display_.drawRect({slider.x + 1, slider.y + 1, slider.width - 2, slider.height - 2}, AppConfig::kBlack);
  const int32_t fillW = ((slider.width - 4) * static_cast<int32_t>(profile.beepVolumePercent)) / 100;
  if (fillW > 0) {
    display_.fillRect({slider.x + 2, slider.y + 2, fillW, slider.height - 4}, AppConfig::kBlack);
  }
  for (uint8_t tick = 0; tick <= 4; ++tick) {
    const int32_t tickX = slider.x + 2 + ((slider.width - 4) * static_cast<int32_t>(tick)) / 4;
    display_.drawLine(tickX, slider.y + slider.height + 4, tickX, slider.y + slider.height + 12, AppConfig::kBlack);
  }
  int32_t knobX = slider.x + 2 + fillW;
  if (knobX < slider.x + 8) knobX = slider.x + 8;
  if (knobX > slider.x + slider.width - 8) knobX = slider.x + slider.width - 8;
  display_.fillRect({knobX - 5, slider.y - 7, 10, slider.height + 14}, AppConfig::kBlack);
  snprintf(levelText, sizeof(levelText), "%u%%", static_cast<unsigned>(profile.beepVolumePercent));
  display_.drawSmallTextBoldAligned(levelText, s.x + s.width / 2, s.y + 354, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  drawButton(audioVoiceToggleButtonBounds(), profile.voiceEnabled ? "VOZ GPS ON" : "VOZ GPS OFF", 2);

  drawButton(audioResetButtonBounds(), "PADRAO", 2);

  renderPageFooter(Page::AudioEditor);
}

void MainScreen::renderDashboardLayoutPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height, AppConfig::kBlack);

  display_.drawSmallTextBoldAligned("PERSONALIZAR TELA", s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);

  const DashboardSlot slots[] = {
      DashboardSlot::Left,
      DashboardSlot::Center,
      DashboardSlot::Right,
  };
  for (DashboardSlot slot : slots) {
    const Rect_t box = dashboardLayoutSlotBounds(slot);
    const DashboardWidgetKind widget = dashboardLayout_.widgetForSlot(slot);
    display_.drawRoundRect(box, 8, AppConfig::kBlack);
    display_.drawRoundRect({box.x + 1, box.y + 1, box.width - 2, box.height - 2}, 7, AppConfig::kBlack);
    if (slot == DashboardSlot::Center) {
      display_.drawRoundRect({box.x + 4, box.y + 4, box.width - 8, box.height - 8}, 5, AppConfig::kBlack);
    }
    if (dashboardLayoutDragActive_ && widget == dashboardLayoutDragWidget_) {
      display_.drawRoundRect({box.x + 8, box.y + 8, box.width - 16, box.height - 16}, 5, AppConfig::kBlack);
      display_.drawRoundRect({box.x + 10, box.y + 10, box.width - 20, box.height - 20}, 4, AppConfig::kBlack);
    }

    display_.drawSmallTextBoldAligned(dashboardSlotLabel(slot), box.x + box.width / 2, box.y + 8, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
    display_.drawSmallTextBoldAligned(dashboardWidgetLabel(widget), box.x + box.width / 2, box.y + 42, 2, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    drawDashboardWidgetPreview(display_, box, widget);
  }

  drawButton(dashboardLayoutResetButtonBounds(), "PADRAO", 2);

  renderPageFooter(Page::DashboardLayout);
}

void MainScreen::renderThermalAssistSettingsPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  const ThermalAssistVisualMode mode = thermalAssistConfig_ ? thermalAssistConfig_->visualMode() : ThermalAssistVisualMode::PilotCentered;
  display_.drawSmallTextBoldAligned("ASSISTENTE TERMICA", s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("MODO VISUAL DO RADAR DE TERMICA", s.x + s.width / 2, s.y + 58, 2, AppConfig::kBlack,
                                EpdDisplay::Align::Center);

  const Rect_t pilotButton = thermalModeButtonBounds(ThermalAssistVisualMode::PilotCentered);
  const Rect_t thermalButton = thermalModeButtonBounds(ThermalAssistVisualMode::ThermalCentered);
  drawButton(pilotButton, "PILOTO NO CENTRO", 2);
  drawButton(thermalButton, "TERMICA NO CENTRO", 2);

  const Rect_t selected = mode == ThermalAssistVisualMode::PilotCentered ? pilotButton : thermalButton;
  display_.drawRoundRect({selected.x - 5, selected.y - 5, selected.width + 10, selected.height + 10}, 9, AppConfig::kBlack);
  display_.drawRoundRect({selected.x - 7, selected.y - 7, selected.width + 14, selected.height + 14}, 10, AppConfig::kBlack);

  display_.drawSmallTextAligned("PILOTO FIXO NO MEIO", pilotButton.x + pilotButton.width / 2, pilotButton.y + 86, 2, AppConfig::kBlack,
                                EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("SETA APONTA PARA O NUCLEO", pilotButton.x + pilotButton.width / 2, pilotButton.y + 112, 2,
                                AppConfig::kBlack, EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("NUCLEO DA TERMICA NO MEIO", thermalButton.x + thermalButton.width / 2, thermalButton.y + 86, 2,
                                AppConfig::kBlack, EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("PILOTO APARECE DESLOCADO", thermalButton.x + thermalButton.width / 2, thermalButton.y + 112, 2,
                                AppConfig::kBlack, EpdDisplay::Align::Center);

  display_.drawLine(s.x + 80, s.y + 282, s.x + s.width - 80, s.y + 282, AppConfig::kBlack);
  display_.drawSmallTextBoldAligned("AUTOMATICO EM VOO", s.x + s.width / 2, s.y + 304, 2, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("O BRVARIO ACOMPANHA A TERMICA PELO VENTO", s.x + s.width / 2, s.y + 340, 2, AppConfig::kBlack,
                                EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("HISTORICO MARCA SUBIDAS RECENTES NO MAPA", s.x + s.width / 2, s.y + 374, 2, AppConfig::kBlack,
                                EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("NUCLEO % MOSTRA A CONFIANÇA DO CENTRO", s.x + s.width / 2, s.y + 408, 2, AppConfig::kBlack,
                                EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("VOCE ESCOLHE APENAS COMO VER O RADAR", s.x + s.width / 2, s.y + 442, 2, AppConfig::kBlack,
                                EpdDisplay::Align::Center);

  renderPageFooter(Page::ThermalAssistSettings);
  const Rect_t infoButton = thermalInfoButtonBounds();
  display_.drawRoundRect(infoButton, 7, AppConfig::kBlack);
  display_.drawCircle(infoButton.x + infoButton.width / 2, infoButton.y + infoButton.height / 2, 13, AppConfig::kBlack);
  display_.drawSmallTextBoldAligned("i", infoButton.x + infoButton.width / 2, infoButton.y + 10, 3, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  if (thermalInfoPopupVisible_) {
    renderThermalInfoPopup();
  }
}

void MainScreen::renderDeviceInfoPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  display_.drawSmallTextBoldAligned("INFORMACOES", s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("BRVARIO - E-PAPER", s.x + s.width / 2, s.y + 64, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  const int32_t left = s.x + 76;
  const int32_t logoW = 240;
  const int32_t logoH = (BrvarioBootLogo::kHeight * logoW) / BrvarioBootLogo::kWidth;
  const int32_t logoX = s.x + s.width - logoW - 56;
  const int32_t logoY = s.y + 104;
  drawPackedIcon4bppScaled(display_, BrvarioBootLogo::kPixels, BrvarioBootLogo::kWidth, BrvarioBootLogo::kHeight, logoX, logoY, logoW, logoH);

  int32_t y = s.y + 98;
  display_.drawSmallTextBold("COMPUTADOR DE VOO PARA VOO-LIVRE", left, y, 2, AppConfig::kBlack);
  y += 36;
  display_.drawSmallTextBold("RECURSOS:", left, y, 2, AppConfig::kBlack);
  y += 30;
  display_.drawSmallText("- VARIO BAROMETRICO COM AUDIO", left, y, 2, AppConfig::kBlack);
  y += 28;
  display_.drawSmallText("- GPS, ALTITUDE, VELOCIDADE E RUMO", left, y, 2, AppConfig::kBlack);
  y += 28;
  display_.drawSmallText("- ESTIMATIVA DE VENTO E ASSISTENTE TERMICA", left, y, 2, AppConfig::kBlack);
  y += 28;
  display_.drawSmallText("- TRACKLOG IGC, WIFI, OTA E PERSONALIZAÇAO", left, y, 2, AppConfig::kBlack);
  y += 34;
  display_.drawSmallTextBold("ESTAÇAO METEOROLOGICA:", left, y, 2, AppConfig::kBlack);
  y += 28;
  display_.drawSmallText("- CALCULOS LOCAIS E DADOS DA INTERNET", left, y, 2, AppConfig::kBlack);
  y += 28;
  display_.drawSmallText("- VENTO, CHUVA, NUVENS E BASE/TETO ESTIMADA", left, y, 2, AppConfig::kBlack);
  y += 28;
  display_.drawSmallText("- RESUMO DE VOO E STATUS XC", left, y, 2, AppConfig::kBlack);
  y += 34;
  display_.drawSmallTextBold("DESENVOLVEDOR / CEO BRVARIO:", left, y, 2, AppConfig::kBlack);
  y += 32;
  display_.drawSmallTextBold("@RANDALRENYE", left, y, 2, AppConfig::kBlack);
  display_.drawSmallTextBoldAligned("@brvario", logoX + logoW / 2, logoY + logoH + 28, 3, AppConfig::kBlack, EpdDisplay::Align::Center);

  renderPageFooter(Page::DeviceInfo);
}

void MainScreen::renderSystemStatusPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  display_.drawSmallTextBoldAligned("STATUS DO SISTEMA", s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("LEITURA PRINCIPAL DOS SENSORES", s.x + s.width / 2, s.y + 58, 2, AppConfig::kBlack,
                                EpdDisplay::Align::Center);

  const int32_t panelGap = 18;
  const int32_t panelMargin = 34;
  const int32_t panelW = (s.width - panelMargin * 2 - panelGap * 2) / 3;
  const int32_t panelH = 330;
  const int32_t panelY = s.y + 96;
  const Rect_t gpsPanel = {s.x + panelMargin, panelY, panelW, panelH};
  const Rect_t espPanel = {gpsPanel.x + panelW + panelGap, panelY, panelW, panelH};
  const Rect_t baroPanel = {espPanel.x + panelW + panelGap, panelY, panelW, panelH};
  char value[56];
  char ageText[16];

  const auto drawSensorPanel = [&](const Rect_t& panel, const char* title) {
    display_.drawRect(panel, AppConfig::kBlack);
    display_.drawRect({panel.x + 2, panel.y + 2, panel.width - 4, panel.height - 4}, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned(title, panel.x + panel.width / 2, panel.y + 16, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
    display_.drawLine(panel.x + 16, panel.y + 58, panel.x + panel.width - 16, panel.y + 58, AppConfig::kBlack);
  };

  const auto drawPanelRow = [&](const Rect_t& panel, uint8_t index, const char* label, const char* text) {
    const int32_t rowY = panel.y + 78 + static_cast<int32_t>(index) * 40;
    const uint8_t valueScale = strlen(text) <= 12 ? 2 : 1;
    display_.drawSmallTextBold(label, panel.x + 14, rowY, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned(text,
                                      panel.x + panel.width - 14,
                                      rowY + (valueScale == 1 ? 6 : 0),
                                      valueScale,
                                      AppConfig::kBlack,
                                      EpdDisplay::Align::Right);
    display_.drawLine(panel.x + 14, rowY + 27, panel.x + panel.width - 14, rowY + 27, AppConfig::kBlack);
  };

  drawSensorPanel(gpsPanel, "GPS");
  snprintf(value, sizeof(value), "%s", compactGpsStatusText(lastData_.gpsStatus));
  drawPanelRow(gpsPanel, 0, "STATUS", value);
  snprintf(value, sizeof(value), "%uSAT H%.1f", static_cast<unsigned>(lastData_.satellites), lastData_.gpsHdop);
  drawPanelRow(gpsPanel, 1, "SINAL", value);
  formatAge(ageText, sizeof(ageText), lastData_.gpsLastSentenceAgeMs);
  snprintf(value, sizeof(value), "%s", ageText);
  drawPanelRow(gpsPanel, 2, "NMEA", value);
  if (lastData_.gpsStatus == GpsSensorStatus::NoData) {
    snprintf(value, sizeof(value), "RX GPIO44");
  } else if (lastData_.gpsStatus == GpsSensorStatus::StaleData) {
    snprintf(value, sizeof(value), "PARADO");
  } else if (lastData_.gpsStatus == GpsSensorStatus::NoFix) {
    snprintf(value, sizeof(value), "AGUARDANDO");
  } else {
    formatAge(ageText, sizeof(ageText), lastData_.gpsLastFixAgeMs);
    snprintf(value, sizeof(value), "%s", ageText);
  }
  drawPanelRow(gpsPanel, 3, "FIX", value);
  snprintf(value, sizeof(value), "%.5f", static_cast<double>(lastData_.latitudeDeg));
  drawPanelRow(gpsPanel, 4, "LAT", value);
  snprintf(value, sizeof(value), "%.5f", static_cast<double>(lastData_.longitudeDeg));
  drawPanelRow(gpsPanel, 5, "LON", value);

  drawSensorPanel(espPanel, "ESP32");
  snprintf(value, sizeof(value), "%lu MHz", static_cast<unsigned long>(ESP.getCpuFreqMHz()));
  drawPanelRow(espPanel, 0, "CPU", value);
  const uint32_t heapTotal = ESP.getHeapSize();
  const uint32_t heapFree = ESP.getFreeHeap();
  const uint32_t heapUsed = heapTotal > heapFree ? heapTotal - heapFree : 0;
  snprintf(value, sizeof(value), "%lu/%lu KB", static_cast<unsigned long>(heapUsed / 1024), static_cast<unsigned long>(heapTotal / 1024));
  drawPanelRow(espPanel, 1, "RAM", value);
  const uint32_t psramTotal = ESP.getPsramSize();
  const uint32_t psramFree = ESP.getFreePsram();
  if (psramTotal > 0) {
    const uint32_t psramUsed = psramTotal > psramFree ? psramTotal - psramFree : 0;
    snprintf(value, sizeof(value), "%lu/%lu KB", static_cast<unsigned long>(psramUsed / 1024), static_cast<unsigned long>(psramTotal / 1024));
  } else {
    snprintf(value, sizeof(value), "NAO");
  }
  drawPanelRow(espPanel, 2, "PSRAM", value);
  snprintf(value, sizeof(value), "%lu KB", static_cast<unsigned long>(ESP.getMinFreeHeap() / 1024));
  drawPanelRow(espPanel, 3, "MIN RAM", value);
  snprintf(value, sizeof(value), "%lu KB", static_cast<unsigned long>(ESP.getSketchSize() / 1024));
  drawPanelRow(espPanel, 4, "FW", value);
  snprintf(value, sizeof(value), "%u%% %.2fV", static_cast<unsigned>(lastData_.batteryPercent), lastData_.batteryVoltage);
  drawPanelRow(espPanel, 5, "BAT", value);

  drawSensorPanel(baroPanel, "BAROMETRO");
  snprintf(value, sizeof(value), "%s", compactBarometerStatusText(lastData_.barometerStatus));
  drawPanelRow(baroPanel, 0, "STATUS", value);
  snprintf(value, sizeof(value), "%.1f hPa", lastData_.pressureHpa);
  drawPanelRow(baroPanel, 1, "PRESSAO", value);
  snprintf(value, sizeof(value), "%.1f C", lastData_.temperatureC);
  drawPanelRow(baroPanel, 2, "TEMP", value);
  formatAge(ageText, sizeof(ageText), lastData_.barometerSampleAgeMs);
  snprintf(value, sizeof(value), "%s", ageText);
  drawPanelRow(baroPanel, 3, "AMOSTRA", value);
  snprintf(value, sizeof(value), "%lu/%lu", static_cast<unsigned long>(lastData_.barometerSampleCount),
           static_cast<unsigned long>(lastData_.barometerFailedReads));
  drawPanelRow(baroPanel, 4, "LEITURAS", value);
  if (lastData_.barometerI2cAddress != 0) {
    snprintf(value, sizeof(value), "0x%02X/0x%02X",
             static_cast<unsigned>(lastData_.barometerI2cAddress),
             static_cast<unsigned>(lastData_.barometerChipId));
  } else {
    snprintf(value, sizeof(value), "SDA18/SCL17");
  }
  drawPanelRow(baroPanel, 5, "I2C", value);

  renderPageFooter(Page::SystemStatus);
}

void MainScreen::renderManualPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  display_.drawSmallTextBoldAligned("MANUAL", s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("ESCOLHA O TIPO DE AJUDA", s.x + s.width / 2, s.y + 58, 2, AppConfig::kBlack,
                                EpdDisplay::Align::Center);

  const Rect_t logic = manualLogicButtonBounds();
  const Rect_t user = manualUserButtonBounds();
  drawButton(logic, "LOGICA DO BRVARIO", 3);
  drawButton(user, "MANUAL DE USO", 3);

  display_.drawSmallTextAligned("Explica os algoritmos, sensores, vento, audio e tracklog.",
                                logic.x + logic.width / 2,
                                logic.y + logic.height + 14,
                                2,
                                AppConfig::kBlack,
                                EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("Ensina como usar as telas, botoes e configuraçoes.",
                                user.x + user.width / 2,
                                user.y + user.height + 14,
                                2,
                                AppConfig::kBlack,
                                EpdDisplay::Align::Center);

  renderPageFooter(Page::Manual);
}

void MainScreen::renderManualTopicPage(bool userManual) {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  if (manualPage_ >= kManualPageCount) {
    manualPage_ = kManualPageCount - 1;
  }

  const ManualTextPage selected = userManual ? manualUserText(manualPage_) : manualLogicText(manualPage_);
  const uint8_t maxOffset = selected.lineCount > kManualVisibleLineCount ? selected.lineCount - kManualVisibleLineCount : 0;
  if (manualScrollOffset_ > maxOffset) {
    manualScrollOffset_ = maxOffset;
  }

  display_.drawSmallTextBoldAligned(userManual ? "MANUAL DE USO" : "LOGICA DO BRVARIO",
                                    s.x + s.width / 2,
                                    s.y + 12,
                                    3,
                                    AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  display_.drawSmallTextAligned(userManual ? "COMO USAR AS TELAS E FUNCOES" : "COMO O SISTEMA CALCULA E DECIDE",
                                s.x + s.width / 2,
                                s.y + 58,
                                2,
                                AppConfig::kBlack,
                                EpdDisplay::Align::Center);

  for (uint8_t i = 0; i < kManualPageCount; ++i) {
    const ManualTextPage tabText = userManual ? manualUserText(i) : manualLogicText(i);
    const Rect_t tab = manualTabBounds(i);
    display_.drawRoundRect(tab, 6, AppConfig::kBlack);
    if (i == manualPage_) {
      display_.drawRoundRect({tab.x + 3, tab.y + 3, tab.width - 6, tab.height - 6}, 4, AppConfig::kBlack);
    }
    display_.drawSmallTextBoldAligned(tabText.tab, tab.x + tab.width / 2, tab.y + 13, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  }

  const Rect_t content = {s.x + 50, s.y + 146, s.width - 150, layout_.trend.y - s.y - 170};
  display_.drawRoundRect(content, 8, AppConfig::kBlack);

  const int32_t left = content.x + 24;
  const int32_t top = content.y + 24;
  display_.drawSmallTextBold(selected.title, left, top, 2, AppConfig::kBlack);
  display_.drawLine(left, top + 32, content.x + content.width - 28, top + 32, AppConfig::kBlack);
  const uint8_t visibleEnd = manualScrollOffset_ + kManualVisibleLineCount < selected.lineCount
                                 ? manualScrollOffset_ + kManualVisibleLineCount
                                 : selected.lineCount;
  for (uint8_t i = manualScrollOffset_; i < visibleEnd; ++i) {
    display_.drawSmallText(selected.lines[i], left, top + 52 + static_cast<int32_t>(i - manualScrollOffset_) * 28, 2, AppConfig::kBlack);
  }

  const Rect_t up = manualScrollUpButtonBounds();
  const Rect_t down = manualScrollDownButtonBounds();
  drawButton(up, manualScrollOffset_ > 0 ? "^" : "-", 3);
  drawButton(down, manualScrollOffset_ < maxOffset ? "v" : "-", 3);

  char pageText[12];
  snprintf(pageText, sizeof(pageText), "PAG %u/%u", static_cast<unsigned>(manualPage_ + 1), static_cast<unsigned>(kManualPageCount));
  display_.drawSmallTextBoldAligned(pageText,
                                    up.x + up.width / 2,
                                    (up.y + up.height + down.y) / 2 - 9,
                                    2,
                                    AppConfig::kBlack,
                                    EpdDisplay::Align::Center);

  renderPageFooter(userManual ? Page::ManualUser : Page::ManualLogic);
}

void MainScreen::renderManualLogicPage() {
  renderManualTopicPage(false);
}

void MainScreen::renderManualUserPage() {
  renderManualTopicPage(true);
}

void MainScreen::renderStoragePage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  display_.drawSmallTextBoldAligned("MEMORIA E MAPAS OFFLINE", s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("microSD para tiles topograficos, relevo e regioes baixadas", s.x + s.width / 2, s.y + 58, 2,
                                AppConfig::kBlack, EpdDisplay::Align::Center);

  const Rect_t panel = {s.x + 46, s.y + 94, s.width - 92, 260};
  display_.drawRect(panel, AppConfig::kBlack);
  display_.drawRect({panel.x + 2, panel.y + 2, panel.width - 4, panel.height - 4}, AppConfig::kBlack);

  char value[64];
  char used[24];
  char total[24];
  char freeText[24];
  char mapsText[24];

  const auto drawRow = [&](uint8_t row, const char* leftLabel, const char* leftValue, const char* rightLabel, const char* rightValue) {
    const int32_t y = panel.y + 28 + static_cast<int32_t>(row) * 50;
    display_.drawSmallTextBold(leftLabel, panel.x + 28, y, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned(leftValue, panel.x + panel.width / 2 - 24, y, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
    display_.drawSmallTextBold(rightLabel, panel.x + panel.width / 2 + 36, y, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned(rightValue, panel.x + panel.width - 28, y, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
    display_.drawLine(panel.x + 24, y + 31, panel.x + panel.width - 24, y + 31, AppConfig::kBlack);
  };

  if (!storageManager_) {
    drawRow(0, "STATUS", "INDISPONIVEL", "SD", "---");
    drawRow(1, "TOTAL", "---", "LIVRE", "---");
    drawRow(2, "MAPAS", "---", "ARQUIVOS", "---");
    drawRow(3, "PASTAS", "---", "OBS", "SEM GERENCIADOR");
  } else {
    formatStorageSize(storageManager_->usedBytes(), used, sizeof(used));
    formatStorageSize(storageManager_->totalBytes(), total, sizeof(total));
    formatStorageSize(storageManager_->freeBytes(), freeText, sizeof(freeText));
    formatStorageSize(storageManager_->mapsBytes(), mapsText, sizeof(mapsText));
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(storageManager_->mapFileCount()));

    drawRow(0, "STATUS", storageManager_->mounted() ? "SD OK" : "SEM SD", "TIPO", storageManager_->cardTypeText());
    drawRow(1, "USADO", used, "TOTAL", total);
    drawRow(2, "LIVRE", freeText, "MAPAS", mapsText);
    drawRow(3, "ARQUIVOS", value, "PASTAS", storageManager_->mapsReady() ? "OK" : "FALTANDO");
    display_.drawSmallTextAligned(storageManager_->statusText(), s.x + s.width / 2, panel.y + panel.height - 34, 2, AppConfig::kBlack,
                                  EpdDisplay::Align::Center);
  }

  display_.drawSmallTextAligned("Formataçao completa do cartao: use FAT32 no computador. No vario, limpe apenas os mapas baixados.",
                                s.x + s.width / 2,
                                s.y + 356,
                                1,
                                AppConfig::kBlack,
                                EpdDisplay::Align::Center);

  drawButton(storageDownloadButtonBounds(), "DOWNLOAD", 2);
  drawButton(storageRefreshButtonBounds(), "ATUALIZAR", 2);
  drawButton(storageClearMapsButtonBounds(), storageClearConfirm_ ? "CONFIRMAR" : "LIMPAR", 2);

  renderPageFooter(Page::Storage);
}

void MainScreen::renderAdvancedSystemPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  display_.drawSmallTextBoldAligned("SISTEMA AVANÇADO", s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("Manutençao, recuperaçao de tela e limpeza de dados salvos", s.x + s.width / 2, s.y + 58, 2,
                                AppConfig::kBlack, EpdDisplay::Align::Center);

  drawButton(advancedRecoverDisplayButtonBounds(), "RECUPERAR TELA", 2);
  drawButton(advancedMoveIgcButtonBounds(), "IGC PARA SD", 2);
  drawButton(advancedClearWifiButtonBounds(), "LIMPAR WIFI", 2);
  drawButton(advancedResetSettingsButtonBounds(), "PADRAO CONFIG", 2);
  drawButton(advancedClearWeatherButtonBounds(), "LIMPAR METEO", 2);
  drawButton(advancedFormatSystemButtonBounds(), "FORMATAR GERAL", 2);

  char line[96];
  const uint16_t igcCount = flightRecorder_ ? flightRecorder_->tracklogCount() : 0;
  const uint8_t igcUsed = flightRecorder_ ? flightRecorder_->storageUsedPercent() : 0;
  snprintf(line,
           sizeof(line),
           "IGC INTERNOS: %u  MEMORIA IGC: %u%%  SD: %s",
           static_cast<unsigned>(igcCount),
           static_cast<unsigned>(igcUsed),
           storageManager_ && storageManager_->mounted() ? "OK" : "NAO MONTADO");
  display_.drawSmallTextBoldAligned(line, s.x + s.width / 2, s.y + 326, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  if (advancedNotice_[0] != '\0') {
    display_.drawSmallTextAligned(advancedNotice_, s.x + s.width / 2, s.y + 356, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  } else {
    display_.drawSmallTextAligned("FORMATAR GERAL recupera memoria interna, mas apaga configs e IGC.",
                                  s.x + s.width / 2,
                                  s.y + 356,
                                  2,
                                  AppConfig::kBlack,
                                  EpdDisplay::Align::Center);
  }

  if (advancedConfirmAction_ != TouchAction::None) {
    const Rect_t box = {s.x + 92, s.y + 374, s.width - 184, 68};
    display_.fillRect(box, AppConfig::kWhite);
    display_.drawRect(box, AppConfig::kBlack);
    display_.drawRect({box.x + 1, box.y + 1, box.width - 2, box.height - 2}, AppConfig::kBlack);
    snprintf(line, sizeof(line), "CONFIRMAR: %s?", advancedActionLabel(advancedConfirmAction_));
    display_.drawSmallTextBoldAligned(line, box.x + box.width / 2, box.y + 8, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
    drawButton(advancedConfirmYesButtonBounds(), "SIM", 2);
    drawButton(advancedConfirmNoButtonBounds(), "NAO", 2);
  }

  renderPageFooter(Page::AdvancedSystem);
}

void MainScreen::renderMapDownloadPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  display_.drawSmallTextBoldAligned(mapDownloadManager_ ? mapDownloadManager_->titleText() : "MAPAS OFFLINE",
                                    s.x + s.width / 2,
                                    s.y + 12,
                                    3,
                                    AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  display_.drawSmallTextAligned(mapDownloadManager_ ? mapDownloadManager_->subtitleText() : "GERENCIADOR INDISPONIVEL",
                                s.x + s.width / 2,
                                s.y + 58,
                                2,
                                AppConfig::kBlack,
                                EpdDisplay::Align::Center);

  const uint8_t selected = mapDownloadManager_ ? mapDownloadManager_->selectedTarget() : 0;
  const uint8_t targetCount = mapDownloadManager_ ? mapDownloadManager_->targetCount() : 0;
  for (uint8_t i = 0; i < 6; ++i) {
    if (!mapDownloadManager_ || i >= targetCount) {
      continue;
    }
    const Rect_t box = mapRegionButtonBounds(i);
    display_.drawRoundRect(box, 8, AppConfig::kBlack);
    display_.drawRoundRect({box.x + 2, box.y + 2, box.width - 4, box.height - 4}, 6, AppConfig::kBlack);
    if (i == selected) {
      display_.drawRoundRect({box.x + 6, box.y + 6, box.width - 12, box.height - 12}, 5, AppConfig::kBlack);
    }

    const MapDownloadTargetInfo& target = mapDownloadManager_->target(i);
    char text[40];
    display_.drawSmallTextBoldAligned(target.name, box.x + box.width / 2, box.y + 12, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
    display_.drawSmallTextAligned(target.subtitle, box.x + box.width / 2, box.y + 38, 1, AppConfig::kBlack, EpdDisplay::Align::Center);
    if (target.kind == MapDownloadTargetKind::DeleteMaps) {
      snprintf(text, sizeof(text), "APAGAR /MAPS");
    } else if (target.kind == MapDownloadTargetKind::CurrentRegion) {
      snprintf(text, sizeof(text), lastData_.gpsFix ? "GPS OK" : "SEM FIX");
    } else if (target.kind == MapDownloadTargetKind::MacroRegion) {
      snprintf(text, sizeof(text), "ENTRAR");
    } else if (target.kind == MapDownloadTargetKind::State || target.kind == MapDownloadTargetKind::StateBundle) {
      if (target.approxSizeMb > 0) {
        snprintf(text, sizeof(text), "%u MAPAS", static_cast<unsigned>(target.approxSizeMb));
      } else {
        snprintf(text, sizeof(text), "EM BREVE");
      }
    } else if (target.kind == MapDownloadTargetKind::PageNext || target.kind == MapDownloadTargetKind::PagePrev) {
      snprintf(text, sizeof(text), "TOCAR");
    } else if (mapDownloadManager_->targetDownloaded(i)) {
      snprintf(text, sizeof(text), "NO CARTAO");
    } else if (target.kind == MapDownloadTargetKind::Package) {
      snprintf(text, sizeof(text), "%u KM", static_cast<unsigned>(target.radiusKm));
    } else {
      snprintf(text, sizeof(text), "~%u MB", static_cast<unsigned>(target.approxSizeMb));
    }
    display_.drawSmallTextBoldAligned(text, box.x + box.width / 2, box.y + 56, 1, AppConfig::kBlack, EpdDisplay::Align::Center);
  }

  const Rect_t status = {s.x + 64, s.y + 292, s.width - 128, 68};
  display_.drawRect(status, AppConfig::kBlack);
  display_.drawRect({status.x + 1, status.y + 1, status.width - 2, status.height - 2}, AppConfig::kBlack);

  const char* statusText = mapDownloadManager_ ? mapDownloadManager_->statusText() : "GERENCIADOR INDISPONIVEL";
  display_.drawSmallTextBold("STATUS", status.x + 18, status.y + 12, 2, AppConfig::kBlack);
  display_.drawSmallTextBoldAligned(statusText, status.x + status.width - 18, status.y + 12, 2, AppConfig::kBlack, EpdDisplay::Align::Right);

  if (mapDownloadManager_ && mapDownloadManager_->busy()) {
    const Rect_t bar = {status.x + 126, status.y + 42, status.width - 152, 12};
    display_.drawRect(bar, AppConfig::kBlack);
    const int32_t fillW = (bar.width - 4) * mapDownloadManager_->progressPercent() / 100;
    if (fillW > 0) {
      display_.fillRect({bar.x + 2, bar.y + 2, fillW, bar.height - 4}, AppConfig::kBlack);
    }
  } else if (mapDownloadManager_ && !mapDownloadManager_->isConfigured()) {
    display_.drawSmallTextAligned("Configure MAP_DOWNLOAD_BASE_URL no platformio.ini", status.x + status.width / 2, status.y + 42, 1,
                                  AppConfig::kBlack, EpdDisplay::Align::Center);
  } else if (storageManager_) {
    char sizeText[24];
    char mapsText[56];
    formatStorageSize(storageManager_->mapsBytes(), sizeText, sizeof(sizeText));
    snprintf(mapsText, sizeof(mapsText), "SD: %u ARQ  %s", static_cast<unsigned>(storageManager_->mapFileCount()), sizeText);
    display_.drawSmallTextAligned(mapsText, status.x + status.width / 2, status.y + 42, 1, AppConfig::kBlack, EpdDisplay::Align::Center);
  }

  if (mapDownloadManager_ && mapDownloadManager_->busy()) {
    drawButton(mapDownloadCancelButtonBounds(), "CANCELAR", 2);
  } else {
    const char* actionLabel = "BAIXAR";
    if (mapDownloadManager_ && selected < targetCount) {
      const MapDownloadTargetInfo& target = mapDownloadManager_->target(selected);
      if (target.kind == MapDownloadTargetKind::DeleteMaps) {
        actionLabel = "APAGAR";
      } else if (target.kind == MapDownloadTargetKind::DownloadedMaps) {
        actionLabel = "VER SD";
      } else if (target.kind == MapDownloadTargetKind::CurrentRegion) {
        actionLabel = "BAIXAR GPS";
      } else if (target.kind == MapDownloadTargetKind::StateBundle) {
        actionLabel = "BAIXAR UF";
      } else if (target.kind == MapDownloadTargetKind::MacroRegion || target.kind == MapDownloadTargetKind::State) {
        actionLabel = "ENTRAR";
      } else if (target.kind == MapDownloadTargetKind::PageNext) {
        actionLabel = "PROXIMA";
      } else if (target.kind == MapDownloadTargetKind::PagePrev) {
        actionLabel = "ANTERIOR";
      }
    }
    drawButton(mapDownloadStartButtonBounds(), actionLabel, 2);
    drawButton(mapDownloadCancelButtonBounds(), mapDownloadManager_ && mapDownloadManager_->canGoBack() ? "VOLTAR" : "WIFI", 2);
  }

  renderPageFooter(Page::MapDownload);
}

void MainScreen::renderFirmwareUpdatePage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height, AppConfig::kBlack);

  display_.drawSmallTextBoldAligned("ATUALIZAÇAO", s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("OTA HTTPS VIA ARQUIVO .BIN", s.x + s.width / 2, s.y + 64, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

  const bool wifiConnected = wifiManager_ && wifiManager_->isConnected();
  const int32_t left = s.x + 80;
  const int32_t valueX = s.x + 360;
  int32_t y = s.y + 118;

  display_.drawSmallTextBold("VERSAO", left, y, 2, AppConfig::kBlack);
  display_.drawSmallTextBold("DEV", valueX, y, 2, AppConfig::kBlack);
  y += 44;
  display_.drawSmallTextBold("WIFI", left, y, 2, AppConfig::kBlack);
  display_.drawSmallTextBold(wifiConnected ? "CONECTADO" : "NAO CONECTADO", valueX, y, 2, AppConfig::kBlack);
  y += 44;
  display_.drawSmallTextBold("ORIGEM", left, y, 2, AppConfig::kBlack);
  display_.drawSmallTextBold("GITHUB .BIN", valueX, y, 2, AppConfig::kBlack);
  y += 44;
  display_.drawSmallTextBold("STATUS", left, y, 2, AppConfig::kBlack);
  display_.drawSmallTextBold(firmwareUpdater_ ? firmwareUpdater_->statusText() : "NAO INICIADO", valueX, y, 2, AppConfig::kBlack);

  if (firmwareUpdater_ && firmwareUpdater_->errorText().length() > 0) {
    String error = firmwareUpdater_->errorText();
    if (error.length() > 34) {
      error = error.substring(0, 34);
    }
    display_.drawSmallTextBold(error.c_str(), valueX, y + 34, 2, AppConfig::kBlack);
  }

  if (!wifiConnected) {
    drawButton(firmwareUpdateWifiButtonBounds(), "ABRIR WIFI", 2);
  } else {
    drawButton(firmwareUpdateStartButtonBounds(), "BUSCAR UPDATE", 2);
  }

  if (firmwareUpdater_ && !firmwareUpdater_->isConfigured()) {
    display_.drawSmallTextAligned("COLE O LINK .BIN EM FirmwareUpdateConfig.h", s.x + s.width / 2, s.y + 392, 2, AppConfig::kBlack,
                                  EpdDisplay::Align::Center);
  } else {
    display_.drawSmallTextAligned("NAO DESLIGUE DURANTE A ATUALIZAÇAO", s.x + s.width / 2, s.y + 392, 2, AppConfig::kBlack,
                                  EpdDisplay::Align::Center);
  }

  renderPageFooter(Page::FirmwareUpdate);
}

void MainScreen::renderWeatherStationPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  display_.drawSmallTextBoldAligned("ESTAÇAO METEOROLÓGICA PORTATIL - VOO LIVRE",
                                    s.x + s.width / 2,
                                    s.y + 4,
                                    3,
                                    AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  uint8_t weatherDayCount = weatherClient_ ? weatherClient_->forecastDayCount() : 0;
  const WeatherFlightData* weather = nullptr;
  const bool weatherLocationMatches =
      !weatherLocationManager_ || (weatherClient_ && weatherClient_->dataMatches(weatherLocationManager_->locationKey()));
  if (weatherClient_ && weatherClient_->hasData() && weatherLocationMatches) {
    if (weatherDayCount == 0) weatherDayCount = 1;
    if (weatherDayIndex_ >= weatherDayCount) weatherDayIndex_ = weatherDayCount - 1;
    weather = &weatherClient_->data(weatherDayIndex_);
  }
  const char* rampName = weatherLocationManager_ ? weatherLocationManager_->displayName() : "GPS ATUAL";
  char rampNameShort[72];
  copyUtf8Prefix(rampName, rampNameShort, sizeof(rampNameShort), 34);
  char stationHeaderLine[128];
  if (weather) {
    snprintf(stationHeaderLine,
             sizeof(stationHeaderLine),
             "RAMPA: %s   PREVISAO: %sRS",
             rampNameShort,
             weather->forecastDateText);
  } else {
    snprintf(stationHeaderLine,
             sizeof(stationHeaderLine),
             "RAMPA: %s   PREVISAO: %.42s",
             rampNameShort,
             weatherStationSleepMode_ ? "MODO SLEEP" : (weatherClient_ ? weatherClient_->statusText() : "OPENWEATHER NAO INICIADO"));
  }
  display_.drawSmallTextBoldAligned(stationHeaderLine, s.x + s.width / 2, s.y + 52, 2, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  if (!weather && !weatherStationSleepMode_) {
    display_.drawRoundRect(weatherLocationLabelBounds(), 6, AppConfig::kBlack);
    display_.drawRoundRect({weatherLocationLabelBounds().x + 2,
                            weatherLocationLabelBounds().y + 2,
                            weatherLocationLabelBounds().width - 4,
                            weatherLocationLabelBounds().height - 4},
                           4,
                           AppConfig::kBlack);
  }

  const int32_t margin = 34;
  const int32_t gap = 12;
  char value[48];

  const auto drawBox = [&](const Rect_t& box) {
    display_.drawRoundRect(box, 8, AppConfig::kBlack);
    display_.drawRoundRect({box.x + 1, box.y + 1, box.width - 2, box.height - 2}, 7, AppConfig::kBlack);
  };

  const auto drawPriorityMetric = [&](const Rect_t& box, const char* label, const char* text, const char* unit, uint8_t icon) {
    drawBox(box);
    const bool showIcon = box.width >= 176;
    const int32_t labelX = showIcon ? box.x + 62 : box.x + 12;
    if (showIcon) {
      drawWeatherIcon(display_, box.x + 28, box.y + box.height / 2, icon);
    }
    display_.drawSmallTextBold(label, labelX, box.y + 10, 2, AppConfig::kBlack);
    const bool longText = strlen(text) > 7;
    const uint8_t valueScale = longText ? 2 : 3;
    const int32_t valueRight = unit && unit[0] != '\0' ? box.x + box.width - 48 : box.x + box.width - 12;
    display_.drawSmallTextBoldAligned(text, valueRight, box.y + (valueScale == 3 ? 36 : 42), valueScale, AppConfig::kBlack,
                                      EpdDisplay::Align::Right);
    if (unit && unit[0] != '\0') {
      display_.drawSmallTextBoldAligned(unit, box.x + box.width - 10, box.y + box.height - 24, 2, AppConfig::kBlack,
                                        EpdDisplay::Align::Right);
    }
  };

  const auto drawRawMetric = [&](const Rect_t& box, const char* label, const char* text, const char* unit, const char* subText) {
    drawBox(box);
    display_.drawSmallTextBold(label, box.x + 12, box.y + 7, 2, AppConfig::kBlack);
    const int32_t valueRight = unit && unit[0] != '\0' ? box.x + box.width - 42 : box.x + box.width - 10;
    display_.drawSmallTextBoldAligned(text, valueRight, box.y + 30, 3, AppConfig::kBlack, EpdDisplay::Align::Right);
    if (unit && unit[0] != '\0') {
      display_.drawSmallTextBoldAligned(unit, box.x + box.width - 8, box.y + box.height - 24, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
    }
    if (subText && subText[0] != '\0') {
      display_.drawSmallTextAligned(subText, box.x + 12, box.y + box.height - 14, 1, AppConfig::kBlack, EpdDisplay::Align::Left);
    }
  };

  const auto drawWindSpeedBox = [&](const Rect_t& box, const WeatherFlightData& weatherData) {
    drawBox(box);
    drawWeatherIcon(display_, box.x + 28, box.y + box.height / 2, 1);
    display_.drawSmallTextBold("VENTO / RAJ.", box.x + 60, box.y + 8, 2, AppConfig::kBlack);

    char windText[16];
    snprintf(windText,
             sizeof(windText),
             "%.0f/%.0f",
             static_cast<double>(weatherData.windSpeedKmh),
             static_cast<double>(weatherData.windGustKmh));
    const uint8_t windValueScale = strlen(windText) > 6 ? 2 : 3;
    display_.drawSmallTextBoldAligned(windText,
                                      box.x + box.width - 58,
                                      box.y + (windValueScale == 3 ? 34 : 39),
                                      windValueScale,
                                      AppConfig::kBlack,
                                      EpdDisplay::Align::Right);
    display_.drawSmallTextBoldAligned("km/h", box.x + box.width - 10, box.y + 43, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
  };

  const auto drawPressureBox = [&](const Rect_t& box, const WeatherFlightData& weatherData) {
    drawBox(box);
    display_.drawSmallTextBold("PRESSAO", box.x + 12, box.y + 5, 2, AppConfig::kBlack);

    char pressureText[12];
    snprintf(pressureText, sizeof(pressureText), "%.0f", static_cast<double>(weatherData.pressure));
    display_.drawSmallTextBoldAligned(pressureText, box.x + box.width - 42, box.y + 23, 3, AppConfig::kBlack, EpdDisplay::Align::Right);
    display_.drawSmallTextBoldAligned("hPa", box.x + box.width - 8, box.y + 31, 2, AppConfig::kBlack, EpdDisplay::Align::Right);

    display_.drawSmallTextBold(weatherData.usingLocalPressure ? "BARO LOCAL" : "API LOCAL", box.x + 12, box.y + 43, 2,
                               AppConfig::kBlack);
  };

  const auto drawWindDirectionBox = [&](const Rect_t& box, const WeatherFlightData& weatherData) {
    drawBox(box);
    const int32_t cx = box.x + 44;
    const int32_t cy = box.y + box.height / 2 + 2;
    const int32_t r = 26;
    display_.drawCircle(cx, cy, r, AppConfig::kBlack);
    display_.drawSmallTextAligned("N", cx, cy - r - 7, 1, AppConfig::kBlack, EpdDisplay::Align::Center);
    display_.drawSmallTextAligned("S", cx, cy + r + 1, 1, AppConfig::kBlack, EpdDisplay::Align::Center);
    display_.drawSmallTextAligned("O", cx - r - 5, cy - 3, 1, AppConfig::kBlack, EpdDisplay::Align::Center);
    display_.drawSmallTextAligned("L", cx + r + 5, cy - 3, 1, AppConfig::kBlack, EpdDisplay::Align::Center);
    const float angle = (static_cast<float>(weatherData.windDirectionDeg) - 90.0F) * 0.0174532925F;
    const int32_t tipX = cx + static_cast<int32_t>(cosf(angle) * static_cast<float>(r - 4));
    const int32_t tipY = cy + static_cast<int32_t>(sinf(angle) * static_cast<float>(r - 4));
    const int32_t tailX = cx - static_cast<int32_t>(cosf(angle) * 6.0F);
    const int32_t tailY = cy - static_cast<int32_t>(sinf(angle) * 6.0F);
    drawThickLine(display_, tailX, tailY, tipX, tipY, AppConfig::kBlack);
    display_.fillCircle(tipX, tipY, 4, AppConfig::kBlack);

    display_.drawSmallTextBold("DIR VENTO", box.x + 86, box.y + 9, 2, AppConfig::kBlack);
    char dirText[18];
    snprintf(dirText, sizeof(dirText), "%s", weatherData.windDirectionText);
    display_.drawSmallTextBoldAligned(dirText, box.x + box.width - 16, box.y + 32, 3, AppConfig::kBlack, EpdDisplay::Align::Right);
  };

  if (!weather) {
    const Rect_t summary = {s.x + margin, s.y + 88, 270, 134};
    const Rect_t local = {summary.x + summary.width + gap, s.y + 88, s.width - margin * 2 - summary.width - gap, 134};
    drawBox(summary);
    display_.drawSmallTextBoldAligned("RESUMO DE VOO", summary.x + summary.width / 2, summary.y + 12, 2, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    display_.drawSmallTextBoldAligned("AGUARDANDO", summary.x + summary.width / 2, summary.y + 52, 3, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    display_.drawSmallTextBoldAligned("PREVISAO", summary.x + summary.width / 2, summary.y + 88, 3, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);

    drawBox(local);
    display_.drawSmallTextBold("DADOS LOCAIS", local.x + 18, local.y + 12, 2, AppConfig::kBlack);
    snprintf(value, sizeof(value), "TEMP %.1f C", static_cast<double>(lastData_.temperatureC));
    display_.drawSmallTextBold(value, local.x + 18, local.y + 48, 2, AppConfig::kBlack);
    snprintf(value, sizeof(value), "PRESSAO %.1f hPa", static_cast<double>(lastData_.pressureHpa));
    display_.drawSmallTextBold(value, local.x + 18, local.y + 80, 2, AppConfig::kBlack);
    display_.drawSmallTextBold(lastData_.sensorDataValid ? "BARO: OK" : "BARO: SEM DADOS", local.x + 330, local.y + 48, 2, AppConfig::kBlack);
    display_.drawSmallTextBold(lastData_.gpsFix ? "GPS: OK" : "GPS: SEM FIX", local.x + 330, local.y + 80, 2, AppConfig::kBlack);

    const Rect_t info = {s.x + margin, s.y + 246, s.width - margin * 2, 160};
    drawBox(info);
    display_.drawSmallTextBoldAligned(weatherClient_ ? weatherClient_->statusText() : "OPENWEATHER NAO INICIADO",
                                      info.x + info.width / 2,
                                      info.y + 26,
                                      3,
                                      AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    display_.drawSmallTextAligned(weatherLocationManager_ && weatherLocationManager_->source() != WeatherLocationSource::GpsCurrent
                                      ? "A pagina liga o WiFi e consulta as coordenadas do local selecionado."
                                      : "A pagina liga o WiFi automaticamente e usa a posiçao do GPS.",
                                  info.x + info.width / 2,
                                  info.y + 82,
                                  2,
                                  AppConfig::kBlack,
                                  EpdDisplay::Align::Center);
    display_.drawSmallTextAligned("Sem internet, ficam visiveis apenas os dados locais do BRVARIO.", info.x + info.width / 2, info.y + 116, 2,
                                  AppConfig::kBlack, EpdDisplay::Align::Center);
  } else {
    static constexpr int32_t kWeatherContentLift = 8;
    const Rect_t summary = {s.x + margin, s.y + 82 - kWeatherContentLift, 270, 138};
    const Rect_t calculated = {
        summary.x + summary.width + gap,
        s.y + 82 - kWeatherContentLift,
        s.width - margin * 2 - summary.width - gap,
        138,
    };
    drawBox(summary);
    display_.drawSmallTextBoldAligned("RESUMO DE VOO", summary.x + summary.width / 2, summary.y + 10, 2, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    display_.drawSmallTextBoldAligned(weatherDayIndex_ == 0 ? "VOO HOJE" : "PREVISAO", summary.x + summary.width / 2, summary.y + 38, 2,
                                      AppConfig::kBlack, EpdDisplay::Align::Center);
    display_.drawSmallTextBoldAligned(weather->flightRating, summary.x + summary.width / 2, summary.y + 62, 3, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    display_.drawSmallTextBoldAligned(weather->flightIndex >= 80 ? "XC RECOMENDADO" : weather->xcStatus,
                                      summary.x + summary.width / 2,
                                      summary.y + 100,
                                      2,
                                      AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    snprintf(value, sizeof(value), "INDICE %d", weather->flightIndex);
    display_.drawSmallTextBoldAligned(value, summary.x + summary.width / 2, summary.y + 120, 2, AppConfig::kBlack, EpdDisplay::Align::Center);

    drawBox(calculated);
    display_.drawSmallTextBold("DADOS CALCULADOS BRVARIO", calculated.x + 16, calculated.y + 10, 2, AppConfig::kBlack);
    const int32_t metricY = calculated.y + 42;
    const int32_t metricW = (calculated.width - 28) / 4;
    Rect_t metric = {calculated.x + 10, metricY, metricW, 84};
    snprintf(value, sizeof(value), "%.0f", weather->cloudBaseMeters);
    drawPriorityMetric(metric, "BASE", value, "m", 2);
    metric.x += metricW + 3;
    snprintf(value, sizeof(value), "%.0f", weather->cloudTopMeters);
    drawPriorityMetric(metric, "TETO", value, "m", 2);
    metric.x += metricW + 3;
    snprintf(value, sizeof(value), "+%.1f", static_cast<double>(weather->thermalStrengthMs));
    drawPriorityMetric(metric, "TERMICA", value, "m/s", 0);
    metric.x += metricW + 3;
    drawPriorityMetric(metric, "JANELA", weather->flightWindowText, "", 5);

    const int32_t rowY = s.y + 234 - kWeatherContentLift;
    const int32_t rowH = 74;
    const int32_t boxW = (s.width - margin * 2 - gap * 3) / 4;
    Rect_t box = {s.x + margin, rowY, boxW, rowH};
    drawWindSpeedBox(box, *weather);
    box.x += boxW + gap;
    drawWindDirectionBox(box, *weather);
    box.x += boxW + gap;
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(weather->cloudCover));
    drawPriorityMetric(box, "COBERTURA", value, "%", 2);
    box.x += boxW + gap;
    if (weather->visibility > 0) {
      snprintf(value, sizeof(value), "%u", static_cast<unsigned>(weather->visibility));
    } else {
      snprintf(value, sizeof(value), "--");
    }
    drawPriorityMetric(box, "VISIBILIDADE", value, "km", 4);

    const int32_t rawY = s.y + 312 - kWeatherContentLift;
    const int32_t rawH = 60;
    box = {s.x + margin, rawY, boxW, rawH};
    snprintf(value, sizeof(value), "%.0f/%.0f", weather->minTemperature, weather->maxTemperature);
    drawRawMetric(box, "TEMP MIN/MAX", value, "C", "");
    box.x += boxW + gap;
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(weather->humidity));
    drawRawMetric(box, "UMIDADE", value, "%", "");
    box.x += boxW + gap;
    drawPressureBox(box, *weather);
    box.x += boxW + gap;
    snprintf(value, sizeof(value), "%.0f", weather->dewPoint);
    drawRawMetric(box, "PONTO ORV.", value, "C", "");

    const Rect_t analysis = {
        s.x + margin,
        s.y + 376 - kWeatherContentLift,
        s.width - margin * 2,
        84 + kWeatherContentLift,
    };
    drawBox(analysis);
    display_.drawSmallTextBold("ANALISE DO DIA", analysis.x + 14, analysis.y + 6, 2, AppConfig::kBlack);

    char line[128];
    copyUtf8Prefix(weather->flightSummary, line, sizeof(line), 60);
    display_.drawSmallText(line, analysis.x + 200, analysis.y + 6, 2, AppConfig::kBlack);

    copyUtf8Prefix(weather->alertMessage, line, sizeof(line), 70);
    display_.drawSmallText(line, analysis.x + 14, analysis.y + 24, 2, AppConfig::kBlack);

    copyUtf8Prefix(weather->alertMessage2, line, sizeof(line), 70);
    display_.drawSmallText(line, analysis.x + 14, analysis.y + 42, 2, AppConfig::kBlack);

    snprintf(line, sizeof(line), "PICO %s  TERMICA %s  CHUVA %u%%  NUVENS %u%%  INDICE %u",
             weather->flightPeakTime,
             weather->thermalClass,
             static_cast<unsigned>(weather->maxRainProbability),
             static_cast<unsigned>(weather->maxCloudCover),
             static_cast<unsigned>(weather->flightIndex));
    display_.drawSmallText(line, analysis.x + 14, analysis.y + 60, 2, AppConfig::kBlack);

    const uint16_t acceptedQuadrants =
        weatherLocationManager_ ? weatherLocationManager_->windQuadrants() : 0;
    char quadrants[36];
    char compactQuadrants[24];
    FlightSiteCatalog::formatWindQuadrants(acceptedQuadrants, quadrants, sizeof(quadrants));
    size_t compactUsed = 0;
    for (const char* source = quadrants;
         *source != '\0' && compactUsed + 1 < sizeof(compactQuadrants);
         ++source) {
      if (*source != ' ') {
        compactQuadrants[compactUsed++] = *source;
      }
    }
    compactQuadrants[compactUsed] = '\0';
    snprintf(line,
             sizeof(line),
             "VENTO PREVISTO: %s  RAMPA ACEITA: %s  COMPAT.: %s",
             weather->windDirectionText,
             compactQuadrants,
             weatherWindCompatibility(acceptedQuadrants, weather->windDirectionDeg));
    display_.drawSmallText(line, analysis.x + 14, analysis.y + 78, 2, AppConfig::kBlack);
  }

  if (!weatherStationSleepMode_) {
    renderPageFooter(Page::WeatherStation);
    const Rect_t infoButton = weatherInfoButtonBounds();
    display_.drawRoundRect(infoButton, 7, AppConfig::kBlack);
    display_.drawCircle(infoButton.x + infoButton.width / 2, infoButton.y + infoButton.height / 2, 13, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned("i", infoButton.x + infoButton.width / 2, infoButton.y + 10, 3, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    if (weather && weatherDayCount > 1) {
      const Rect_t prev = weatherPrevDayButtonBounds();
      const Rect_t next = weatherNextDayButtonBounds();
      display_.drawRoundRect(prev, 8, AppConfig::kBlack);
      display_.drawRoundRect(next, 8, AppConfig::kBlack);
      const int32_t prevCx = prev.x + prev.width / 2;
      const int32_t prevCy = prev.y + prev.height / 2;
      const int32_t nextCx = next.x + next.width / 2;
      const int32_t nextCy = next.y + next.height / 2;
      if (weatherDayIndex_ > 0) {
        display_.fillTriangle(prevCx - 10, prevCy, prevCx + 10, prevCy - 14, prevCx + 10, prevCy + 14, AppConfig::kBlack);
      } else {
        display_.drawSmallTextAligned("-", prevCx, prevCy - 8, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
      }
      if (weatherDayIndex_ + 1 < weatherDayCount) {
        display_.fillTriangle(nextCx + 10, nextCy, nextCx - 10, nextCy - 14, nextCx - 10, nextCy + 14, AppConfig::kBlack);
      } else {
        display_.drawSmallTextAligned("-", nextCx, nextCy - 8, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
      }
      char dayText[8];
      snprintf(dayText, sizeof(dayText), "%u/%u", static_cast<unsigned>(weatherDayIndex_ + 1), static_cast<unsigned>(weatherDayCount));
      display_.drawSmallTextBoldAligned(dayText,
                                        layout_.speed.x + layout_.speed.width / 2,
                                        layout_.footerButtons.y + 22,
                                        2,
                                        AppConfig::kBlack,
                                        EpdDisplay::Align::Center);
    }
    if (weatherInfoPopupVisible_) {
      renderWeatherInfoPopup();
    }
  }
}

void MainScreen::renderWeatherInfoPopup() {
  const Rect_t popup = weatherInfoPopupBounds();
  display_.fillRect(popup, AppConfig::kWhite);
  display_.drawRect(popup, AppConfig::kBlack);
  display_.drawRect({popup.x + 2, popup.y + 2, popup.width - 4, popup.height - 4}, AppConfig::kBlack);

  const Rect_t close = weatherInfoCloseButtonBounds();
  display_.drawRoundRect(close, 7, AppConfig::kBlack);
  drawThickLine(display_, close.x + 12, close.y + 10, close.x + close.width - 12, close.y + close.height - 10, AppConfig::kBlack);
  drawThickLine(display_, close.x + close.width - 12, close.y + 10, close.x + 12, close.y + close.height - 10, AppConfig::kBlack);

  const Rect_t up = weatherInfoScrollUpButtonBounds();
  const Rect_t down = weatherInfoScrollDownButtonBounds();
  display_.drawRoundRect(up, 7, AppConfig::kBlack);
  display_.drawRoundRect(down, 7, AppConfig::kBlack);
  const int32_t upCx = up.x + up.width / 2;
  const int32_t upCy = up.y + up.height / 2;
  const int32_t downCx = down.x + down.width / 2;
  const int32_t downCy = down.y + down.height / 2;
  if (weatherInfoScrollPage_ > 0) {
    display_.fillTriangle(upCx, upCy - 12, upCx - 13, upCy + 10, upCx + 13, upCy + 10, AppConfig::kBlack);
  } else {
    display_.drawSmallTextAligned("-", upCx, upCy - 8, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  }
  if (weatherInfoScrollPage_ + 1 < kWeatherInfoPageCount) {
    display_.fillTriangle(downCx, downCy + 12, downCx - 13, downCy - 10, downCx + 13, downCy - 10, AppConfig::kBlack);
  } else {
    display_.drawSmallTextAligned("-", downCx, downCy - 8, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  }

  display_.drawSmallTextBoldAligned("ESTACAO METEO E LOCAIS", popup.x + popup.width / 2, popup.y + 14, 3, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  const int32_t left = popup.x + 34;
  const int32_t top = popup.y + 70;
  const int32_t lineStep = 24;
  const char* title = "DADOS REAIS";
  const char* lines[9] = {};
  uint8_t lineCount = 0;

  switch (weatherInfoScrollPage_) {
    case 0:
      title = "ESCOLHA DO LOCAL";
      lines[lineCount++] = "- GPS ATUAL usa a posicao fisica do instrumento.";
      lines[lineCount++] = "- RAMPA ou COORDENADA usa outro local na previsao.";
      lines[lineCount++] = "- O GPS de voo, mapa e IGC nunca sao alterados.";
      lines[lineCount++] = "- ENTRAR abre a previsao do local que ja esta ativo.";
      lines[lineCount++] = "- A escolha e os cinco favoritos ficam salvos.";
      lines[lineCount++] = "- Busque por nome/cidade ou filtre o catalogo por UF.";
      break;
    case 1:
      title = "FONTE, PRESSAO E CACHE";
      lines[lineCount++] = "- OpenWeather recebe as coordenadas do local escolhido.";
      lines[lineCount++] = "- Com GPS, o barometro pode fornecer pressao local.";
      lines[lineCount++] = "- Em rampa remota, vale a pressao da API daquele local.";
      lines[lineCount++] = "- Trocar o local invalida a previsao anterior.";
      lines[lineCount++] = "- Respostas antigas nao substituem o novo local.";
      lines[lineCount++] = "- A lista de rampas pode ser atualizada pela internet.";
      break;
    case 2:
      title = "CALCULOS E ANALISE DO DIA";
      lines[lineCount++] = "- BASE = temperatura - ponto de orvalho x 125.";
      lines[lineCount++] = "- TETO estima a profundidade util acima da base.";
      lines[lineCount++] = "- TERMICA estima potencial medio de subida.";
      lines[lineCount++] = "- INDICE 0-100 pondera vento, rajada, chuva,";
      lines[lineCount++] = "  nuvens, base, umidade e visibilidade.";
      lines[lineCount++] = "- HOJE calcula do horario atual ate tres horas a frente.";
      lines[lineCount++] = "- PROXIMOS DIAS usam 09-17 e destacam a melhor janela.";
      break;
    default:
      title = "VENTO, COMPATIBILIDADE E LIMITES";
      lines[lineCount++] = "- VENTO PREVISTO indica de onde o vento deve vir.";
      lines[lineCount++] = "- RAMPA ACEITA mostra os quadrantes do catalogo.";
      lines[lineCount++] = "- FAVORAVEL e setor aceito; MARGINAL e setor vizinho.";
      lines[lineCount++] = "- DESFAVORAVEL exige outra avaliacao de decolagem.";
      lines[lineCount++] = "- TERMICA nao e o vario em tempo real.";
      lines[lineCount++] = "- E uma previsao; confirme na rampa e no ar.";
      break;
  }

  display_.drawSmallTextBold(title, left, top, 2, AppConfig::kBlack);
  for (uint8_t i = 0; i < lineCount; ++i) {
    display_.drawSmallText(lines[i], left, top + 34 + lineStep * i, 2, AppConfig::kBlack);
  }

  char pageText[12];
  snprintf(pageText, sizeof(pageText), "PAG %u/%u", static_cast<unsigned>(weatherInfoScrollPage_ + 1), static_cast<unsigned>(kWeatherInfoPageCount));
  display_.drawSmallTextBoldAligned(pageText, popup.x + popup.width / 2, popup.y + popup.height - 34, 2, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
}

void MainScreen::beginWeatherCatalogMatchRebuild() {
  weatherCatalogMatchCount_ = 0;
  weatherCatalogScanIndex_ = 0;
  weatherCatalogScanTotal_ = FlightSiteCatalog::count();
  weatherCatalogLoadingProgress_ = 0;
  weatherCatalogRenderedProgress_ = 0;
  weatherCatalogFilterDirty_ = false;
  weatherCatalogLastProgress_ = flightSiteCatalogUpdater_ ? flightSiteCatalogUpdater_->progressPercent() : 0;
  weatherCatalogLastState_ =
      flightSiteCatalogUpdater_ ? static_cast<uint8_t>(flightSiteCatalogUpdater_->state()) : 0;
  weatherLocationView_ = WeatherLocationView::CatalogLoading;
}

bool MainScreen::processWeatherCatalogMatchChunk(uint16_t maxSites) {
  uint16_t processed = 0;
  while (weatherCatalogScanIndex_ < weatherCatalogScanTotal_ &&
         weatherCatalogMatchCount_ < FlightSiteCatalog::kMaxCatalogSites &&
         processed < maxSites) {
    const uint16_t catalogIndex = weatherCatalogScanIndex_++;
    ++processed;
    const FlightSite* site = FlightSiteCatalog::site(catalogIndex);
    if (!site) {
      continue;
    }
    if (weatherCatalogState_[0] != '\0' && strcmp(site->state, weatherCatalogState_) != 0) {
      continue;
    }
    if (weatherCatalogQuery_[0] != '\0' && !containsFoldedText(site->name, weatherCatalogQuery_) &&
        !containsFoldedText(site->city, weatherCatalogQuery_) && !containsFoldedText(site->state, weatherCatalogQuery_)) {
      continue;
    }
    weatherCatalogMatches_[weatherCatalogMatchCount_++] = catalogIndex;
  }

  weatherCatalogLoadingProgress_ =
      weatherCatalogScanTotal_ == 0
          ? 100
          : static_cast<uint8_t>((static_cast<uint32_t>(weatherCatalogScanIndex_) * 100U) / weatherCatalogScanTotal_);
  if (weatherCatalogScanIndex_ < weatherCatalogScanTotal_) {
    return false;
  }

  weatherCatalogLoadingProgress_ = 100;
  const uint16_t pageCount =
      static_cast<uint16_t>((weatherCatalogMatchCount_ + kWeatherCatalogRowsPerPage - 1) / kWeatherCatalogRowsPerPage);
  if (pageCount == 0) {
    weatherCatalogPage_ = 0;
  } else if (weatherCatalogPage_ >= pageCount) {
    weatherCatalogPage_ = pageCount - 1;
  }
  weatherLocationView_ = WeatherLocationView::Catalog;
  return true;
}

const FlightSite* MainScreen::weatherCatalogFilteredSite(uint16_t index) {
  if (weatherCatalogFilterDirty_) {
    return nullptr;
  }
  if (index >= weatherCatalogMatchCount_) {
    return nullptr;
  }
  return FlightSiteCatalog::site(weatherCatalogMatches_[index]);
}

bool MainScreen::appendWeatherCatalogSearchChar(char value) {
  const size_t length = strlen(weatherCatalogQuery_);
  if (length + 1 >= sizeof(weatherCatalogQuery_)) {
    return false;
  }
  if (value >= 'a' && value <= 'z') {
    value = static_cast<char>(value - ('a' - 'A'));
  }
  if ((value < 'A' || value > 'Z') && value != ' ' && (value < '0' || value > '9')) {
    return false;
  }
  if (value == ' ' && (length == 0 || weatherCatalogQuery_[length - 1] == ' ')) {
    return false;
  }
  weatherCatalogQuery_[length] = value;
  weatherCatalogQuery_[length + 1] = '\0';
  weatherCatalogFilterDirty_ = true;
  weatherCatalogPage_ = 0;
  return true;
}

void MainScreen::renderWeatherLocationPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  const char* title = "LOCAL DA PREVISAO";
  if (weatherLocationView_ == WeatherLocationView::Favorites) {
    title = "RAMPAS FAVORITAS";
  } else if (weatherLocationView_ == WeatherLocationView::CatalogLoading) {
    title = "PROCURAR RAMPA - CATALOGO BRVARIO";
  } else if (weatherLocationView_ == WeatherLocationView::Catalog) {
    title = "PROCURAR RAMPA - CATALOGO BRVARIO";
  } else if (weatherLocationView_ == WeatherLocationView::CatalogSearch) {
    title = "BUSCAR RAMPA POR NOME OU CIDADE";
  } else if (weatherLocationView_ == WeatherLocationView::CatalogStates) {
    title = "FILTRAR RAMPAS POR ESTADO";
  } else if (weatherLocationView_ == WeatherLocationView::Manual) {
    title = "COORDENADA MANUAL";
  }
  display_.drawSmallTextBoldAligned(title, s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);

  if (!weatherLocationManager_) {
    display_.drawSmallTextBoldAligned("GERENCIADOR DE LOCAL NAO INICIADO", s.x + s.width / 2, s.y + 190, 3, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    renderPageFooter(Page::WeatherLocation);
    return;
  }

  if (weatherLocationView_ == WeatherLocationView::Menu) {
    const Rect_t active = {s.x + 46, s.y + 72, s.width - 92, 150};
    display_.drawRoundRect(active, 8, AppConfig::kBlack);
    display_.drawRoundRect({active.x + 1, active.y + 1, active.width - 2, active.height - 2}, 7, AppConfig::kBlack);
    display_.drawSmallTextBold("LOCAL ATIVO", active.x + 18, active.y + 12, 2, AppConfig::kBlack);
    const char* activeName = weatherLocationManager_->displayName();
    display_.drawSmallTextBold(activeName, active.x + 18, active.y + 42, strlen(activeName) > 25 ? 2 : 3, AppConfig::kBlack);

    char line[96];
    const char* state = weatherLocationManager_->state();
    snprintf(line,
             sizeof(line),
             "%s%s%s",
             weatherLocationManager_->city(),
             state && state[0] != '\0' ? " / " : "",
             state && state[0] != '\0' ? state : "");
    display_.drawSmallTextBold(line, active.x + 18, active.y + 78, 2, AppConfig::kBlack);
    snprintf(line, sizeof(line), "FONTE: %s", weatherLocationManager_->sourceName());
    display_.drawSmallTextBold(line, active.x + 18, active.y + 108, 2, AppConfig::kBlack);

    snprintf(line,
             sizeof(line),
             "%.6f, %.6f",
             weatherLocationManager_->latitude(),
             weatherLocationManager_->longitude());
    display_.drawSmallTextBoldAligned(line, active.x + active.width - 18, active.y + 16, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
    if (weatherLocationManager_->altitudeM() != 0 || weatherLocationManager_->verticalDropM() != 0) {
      snprintf(line,
               sizeof(line),
               "ALT %d m  DESN %d m",
               static_cast<int>(weatherLocationManager_->altitudeM()),
               static_cast<int>(weatherLocationManager_->verticalDropM()));
      display_.drawSmallTextBoldAligned(line, active.x + active.width - 18, active.y + 47, 2, AppConfig::kBlack,
                                        EpdDisplay::Align::Right);
    }
    if (weatherLocationManager_->windQuadrants() != 0) {
      char quadrants[36];
      FlightSiteCatalog::formatWindQuadrants(weatherLocationManager_->windQuadrants(), quadrants, sizeof(quadrants));
      snprintf(line, sizeof(line), "ACEITA: %s", quadrants);
      display_.drawSmallTextBoldAligned(line, active.x + active.width - 18, active.y + 76, 2, AppConfig::kBlack,
                                        EpdDisplay::Align::Right);
    }
    drawButton(weatherLocationEnterButtonBounds(), "ENTRAR", 2);

    drawButton(weatherLocationMenuButtonBounds(0), "USAR GPS ATUAL", 2);
    drawButton(weatherLocationMenuButtonBounds(1), "RAMPAS FAVORITAS", 2);
    drawButton(weatherLocationMenuButtonBounds(2), "PROCURAR RAMPA", 2);
    drawButton(weatherLocationMenuButtonBounds(3), "COORDENADA MANUAL", 2);
  } else if (weatherLocationView_ == WeatherLocationView::CatalogLoading) {
    const Rect_t loadingPanel = {s.x + 100, s.y + 148, s.width - 200, 190};
    display_.drawRoundRect(loadingPanel, 8, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned("CARREGANDO CATALOGO DE RAMPAS",
                                      loadingPanel.x + loadingPanel.width / 2,
                                      loadingPanel.y + 24,
                                      3,
                                      AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    display_.drawSmallTextAligned("PREPARANDO A LISTA E A BUSCA...",
                                  loadingPanel.x + loadingPanel.width / 2,
                                  loadingPanel.y + 66,
                                  2,
                                  AppConfig::kBlack,
                                  EpdDisplay::Align::Center);

    const Rect_t progressBar = {loadingPanel.x + 46, loadingPanel.y + 105, loadingPanel.width - 92, 28};
    display_.drawRect(progressBar, AppConfig::kBlack);
    display_.drawRect({progressBar.x + 1, progressBar.y + 1, progressBar.width - 2, progressBar.height - 2}, AppConfig::kBlack);
    const int32_t fillWidth =
        static_cast<int32_t>((static_cast<uint32_t>(progressBar.width - 6) * weatherCatalogLoadingProgress_) / 100U);
    if (fillWidth > 0) {
      display_.fillRect({progressBar.x + 3, progressBar.y + 3, fillWidth, progressBar.height - 6}, AppConfig::kBlack);
    }

    char progressText[48];
    snprintf(progressText,
             sizeof(progressText),
             "%u%%  (%u/%u)",
             static_cast<unsigned>(weatherCatalogLoadingProgress_),
             static_cast<unsigned>(weatherCatalogScanIndex_),
             static_cast<unsigned>(weatherCatalogScanTotal_));
    display_.drawSmallTextBoldAligned(progressText,
                                      loadingPanel.x + loadingPanel.width / 2,
                                      loadingPanel.y + 148,
                                      2,
                                      AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
  } else if (weatherLocationView_ == WeatherLocationView::CatalogSearch) {
    const Rect_t field = weatherCatalogSearchFieldBounds();
    display_.drawRoundRect(field, 7, AppConfig::kBlack);
    display_.drawRoundRect({field.x + 2, field.y + 2, field.width - 4, field.height - 4}, 5, AppConfig::kBlack);
    display_.drawSmallTextBold("BUSCA", field.x + 14, field.y + 9, 2, AppConfig::kBlack);
    display_.drawSmallTextBoldAligned(weatherCatalogQuery_[0] != '\0' ? weatherCatalogQuery_ : "TODAS AS RAMPAS",
                                      field.x + field.width - 14,
                                      field.y + 27,
                                      2,
                                      AppConfig::kBlack,
                                      EpdDisplay::Align::Right);

    static const char* const kSearchRows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    static const uint8_t kSearchCols[] = {10, 9, 7};
    for (uint8_t row = 0; row < 3; ++row) {
      for (uint8_t col = 0; col < kSearchCols[row]; ++col) {
        char label[2] = {kSearchRows[row][col], '\0'};
        drawButton(weatherCatalogSearchKeyBounds(row, col), label, 2);
      }
    }
    drawButton(weatherCatalogSearchSpecialBounds(0), "ESPACO", 2);
    drawButton(weatherCatalogSearchSpecialBounds(1), "APAGAR", 2);
    drawButton(weatherCatalogSearchSpecialBounds(2), "LIMPAR", 2);
    drawButton(weatherCatalogSearchSpecialBounds(3), "APLICAR", 2);
  } else if (weatherLocationView_ == WeatherLocationView::CatalogStates) {
    display_.drawSmallTextAligned("SELECIONE UMA UF OU TODAS", s.x + s.width / 2, s.y + 61, 2, AppConfig::kBlack,
                                  EpdDisplay::Align::Center);
    for (uint8_t i = 0; i < kBrazilStateCount; ++i) {
      const Rect_t choice = weatherCatalogStateChoiceBounds(i);
      const bool selected =
          (i == 0 && weatherCatalogState_[0] == '\0') || (i > 0 && strcmp(weatherCatalogState_, kBrazilStates[i]) == 0);
      drawButton(choice, i == 0 ? "TODAS" : kBrazilStates[i], 2);
      if (selected) {
        display_.drawRoundRect({choice.x + 2, choice.y + 2, choice.width - 4, choice.height - 4}, 5, AppConfig::kBlack);
      }
    }
  } else if (weatherLocationView_ == WeatherLocationView::Manual) {
    display_.drawSmallTextAligned("INFORME LATITUDE E LONGITUDE EM GRAUS DECIMAIS", s.x + s.width / 2, s.y + 72, 2,
                                  AppConfig::kBlack, EpdDisplay::Align::Center);
    for (uint8_t i = 0; i < 2; ++i) {
      const Rect_t field = weatherManualFieldBounds(i);
      display_.drawRoundRect(field, 7, AppConfig::kBlack);
      if (weatherManualField_ == i) {
        display_.drawRoundRect({field.x + 2, field.y + 2, field.width - 4, field.height - 4}, 5, AppConfig::kBlack);
      }
      display_.drawSmallTextBold(i == 0 ? "LATITUDE" : "LONGITUDE", field.x + 12, field.y + 8, 2, AppConfig::kBlack);
      display_.drawSmallTextBoldAligned(i == 0 ? weatherManualLatitude_ : weatherManualLongitude_,
                                        field.x + field.width - 12,
                                        field.y + 27,
                                        2,
                                        AppConfig::kBlack,
                                        EpdDisplay::Align::Right);
    }

    static const char kKeys[4][3] = {
        {'1', '2', '3'},
        {'4', '5', '6'},
        {'7', '8', '9'},
        {'-', '0', '.'},
    };
    for (uint8_t row = 0; row < 4; ++row) {
      for (uint8_t col = 0; col < 3; ++col) {
        char label[2] = {kKeys[row][col], '\0'};
        drawButton(weatherManualKeyBounds(row, col), label, 2);
      }
    }
    drawButton(weatherManualDeleteButtonBounds(), "APAGAR", 2);
    drawButton(weatherManualSaveButtonBounds(), "USAR ESTA COORDENADA", 2);
  } else {
    const bool favoritesView = weatherLocationView_ == WeatherLocationView::Favorites;
    const uint16_t count = favoritesView ? weatherLocationManager_->favoriteCount() : weatherCatalogMatchCount_;
    const uint16_t pageCount =
        favoritesView ? 1
                      : static_cast<uint16_t>((count + kWeatherCatalogRowsPerPage - 1) / kWeatherCatalogRowsPerPage);
    if (!favoritesView && pageCount > 0 && weatherCatalogPage_ >= pageCount) {
      weatherCatalogPage_ = pageCount - 1;
    }
    const uint16_t firstIndex = favoritesView ? 0 : weatherCatalogPage_ * kWeatherCatalogRowsPerPage;
    const uint8_t visibleCount = favoritesView
                                     ? static_cast<uint8_t>(count)
                                     : static_cast<uint8_t>((count - firstIndex) < kWeatherCatalogRowsPerPage
                                                                ? count - firstIndex
                                                                : kWeatherCatalogRowsPerPage);

    if (!favoritesView) {
      char status[96];
      snprintf(status,
               sizeof(status),
               "%s  |  %s",
               FlightSiteCatalog::statusText(),
               flightSiteCatalogUpdater_ ? flightSiteCatalogUpdater_->statusText() : "ATUALIZADOR NAO INICIADO");
      if (weatherLocationNotice_[0] == '\0') {
        display_.drawSmallTextBoldAligned(status, s.x + s.width / 2, s.y + 53, strlen(status) > 68 ? 1 : 2,
                                          AppConfig::kBlack, EpdDisplay::Align::Center);
      }
      const Rect_t searchButton = weatherCatalogSearchButtonBounds();
      const Rect_t stateButton = weatherCatalogStateButtonBounds();
      drawButton(searchButton, weatherCatalogQuery_[0] != '\0' ? weatherCatalogQuery_ : "BUSCAR NOME / CIDADE", 2);
      char stateLabel[16];
      snprintf(stateLabel, sizeof(stateLabel), "UF: %s", weatherCatalogState_[0] != '\0' ? weatherCatalogState_ : "TODAS");
      drawButton(stateButton, stateLabel, 2);
      char pageText[32];
      snprintf(pageText,
               sizeof(pageText),
               "%u RAMPAS  %u/%u",
               static_cast<unsigned>(count),
               static_cast<unsigned>(pageCount == 0 ? 0 : weatherCatalogPage_ + 1),
               static_cast<unsigned>(pageCount));
      display_.drawSmallTextBoldAligned(pageText, s.x + s.width / 2, s.y + 124, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
    }
    if (count == 0) {
      display_.drawSmallTextBoldAligned(favoritesView ? "NENHUMA RAMPA FAVORITA" : "CATALOGO SEM RAMPAS",
                                        s.x + s.width / 2,
                                        s.y + 190,
                                        3,
                                        AppConfig::kBlack,
                                        EpdDisplay::Align::Center);
      display_.drawSmallTextAligned(favoritesView ? "VOLTE E USE PROCURAR RAMPA PARA ADICIONAR."
                                                  : "ATUALIZE A LISTA DE RAMPAS.",
                                    s.x + s.width / 2,
                                    s.y + 244,
                                    2,
                                    AppConfig::kBlack, EpdDisplay::Align::Center);
    }

    for (uint8_t i = 0; i < visibleCount; ++i) {
      const FlightSite* site = favoritesView ? weatherLocationManager_->favorite(i) : weatherCatalogFilteredSite(firstIndex + i);
      if (!site) {
        continue;
      }
      const Rect_t row = weatherLocationRowBounds(i);
      display_.drawRoundRect(row, 6, AppConfig::kBlack);

      char details[96];
      char quadrants[32];
      char quadrantsCompact[32];
      char nameShort[64];
      char cityShort[32];
      copyUtf8Prefix(site->name, nameShort, sizeof(nameShort), 30);
      copyUtf8Prefix(site->city, cityShort, sizeof(cityShort), 16);
      FlightSiteCatalog::formatWindQuadrants(site->windQuadrants, quadrants, sizeof(quadrants));
      size_t compactUsed = 0;
      for (const char* source = quadrants; *source != '\0' && compactUsed + 1 < sizeof(quadrantsCompact); ++source) {
        if (*source != ' ') {
          quadrantsCompact[compactUsed++] = *source;
        }
      }
      quadrantsCompact[compactUsed] = '\0';
      display_.drawSmallTextBold(nameShort, row.x + 12, row.y + 7, 2, AppConfig::kBlack);
      char windText[38];
      snprintf(windText, sizeof(windText), "V:%s", quadrantsCompact);
      display_.drawSmallTextBoldAligned(windText, weatherLocationRowActionBounds(i).x - 12, row.y + 7, 2, AppConfig::kBlack,
                                        EpdDisplay::Align::Right);
      snprintf(details,
               sizeof(details),
               "%s/%s  ALT:%d  DESN:%d",
               cityShort,
               site->state,
               static_cast<int>(site->altitudeM),
               static_cast<int>(site->verticalDropM));
      display_.drawSmallTextBold(details, row.x + 12, row.y + 31, 2, AppConfig::kBlack);

      const Rect_t action = weatherLocationRowActionBounds(i);
      drawButton(action,
                 favoritesView ? "REMOVER" : (weatherLocationManager_->isFavorite(*site) ? "FAVORITA" : "+ FAVORITO"),
                 2);
    }

    if (!favoritesView) {
      drawButton(weatherCatalogControlBounds(0), weatherCatalogPage_ > 0 ? "ANTERIOR" : "-", 2);
      drawButton(weatherCatalogControlBounds(1),
                 flightSiteCatalogUpdater_ && flightSiteCatalogUpdater_->busy() ? "CANCELAR ATUALIZACAO" : "ATUALIZAR LISTA",
                 2);
      drawButton(weatherCatalogControlBounds(2), weatherCatalogPage_ + 1 < pageCount ? "PROXIMA" : "-", 2);
    }
  }

  if (weatherLocationNotice_[0] != '\0') {
    display_.drawSmallTextBoldAligned(weatherLocationNotice_, s.x + s.width / 2, s.y + 57, 2, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
  }
  renderPageFooter(Page::WeatherLocation);
  const Rect_t infoButton = weatherInfoButtonBounds();
  display_.drawRoundRect(infoButton, 7, AppConfig::kBlack);
  display_.drawCircle(infoButton.x + infoButton.width / 2, infoButton.y + infoButton.height / 2, 13, AppConfig::kBlack);
  display_.drawSmallTextBoldAligned("i",
                                    infoButton.x + infoButton.width / 2,
                                    infoButton.y + 10,
                                    3,
                                    AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  if (weatherInfoPopupVisible_) {
    renderWeatherInfoPopup();
  }
}

bool MainScreen::previewWeatherLocationTouch(int32_t x, int32_t y) const {
  if (weatherLocationView_ == WeatherLocationView::Menu) {
    if (pointInRect(weatherLocationEnterButtonBounds(), x, y)) {
      return true;
    }
    for (uint8_t i = 0; i < 4; ++i) {
      if (pointInRect(weatherLocationMenuButtonBounds(i), x, y)) {
        return true;
      }
    }
    return false;
  }

  if (weatherLocationView_ == WeatherLocationView::CatalogSearch) {
    for (uint8_t row = 0; row < 3; ++row) {
      const uint8_t cols = row == 0 ? 10 : (row == 1 ? 9 : 7);
      for (uint8_t col = 0; col < cols; ++col) {
        if (pointInRect(weatherCatalogSearchKeyBounds(row, col), x, y)) {
          return true;
        }
      }
    }
    for (uint8_t i = 0; i < 4; ++i) {
      if (pointInRect(weatherCatalogSearchSpecialBounds(i), x, y)) {
        return true;
      }
    }
    return false;
  }

  if (weatherLocationView_ == WeatherLocationView::CatalogStates) {
    for (uint8_t i = 0; i < kBrazilStateCount; ++i) {
      if (pointInRect(weatherCatalogStateChoiceBounds(i), x, y)) {
        return true;
      }
    }
    return false;
  }

  if (weatherLocationView_ == WeatherLocationView::Manual) {
    if (pointInRect(weatherManualFieldBounds(0), x, y) || pointInRect(weatherManualFieldBounds(1), x, y) ||
        pointInRect(weatherManualDeleteButtonBounds(), x, y) || pointInRect(weatherManualSaveButtonBounds(), x, y)) {
      return true;
    }
    for (uint8_t row = 0; row < 4; ++row) {
      for (uint8_t col = 0; col < 3; ++col) {
        if (pointInRect(weatherManualKeyBounds(row, col), x, y)) {
          return true;
        }
      }
    }
    return false;
  }

  const uint8_t count = weatherLocationView_ == WeatherLocationView::Favorites
                            ? (weatherLocationManager_ ? weatherLocationManager_->favoriteCount() : 0)
                            : 0;
  if (weatherLocationView_ == WeatherLocationView::Catalog) {
    for (uint8_t control = 0; control < 3; ++control) {
      if (pointInRect(weatherCatalogControlBounds(control), x, y)) {
        return true;
      }
    }
    if (pointInRect(weatherCatalogSearchButtonBounds(), x, y) || pointInRect(weatherCatalogStateButtonBounds(), x, y)) {
      return true;
    }
    const uint16_t total = weatherCatalogMatchCount_;
    const uint16_t first = weatherCatalogPage_ * kWeatherCatalogRowsPerPage;
    const uint8_t visible = first < total
                                ? static_cast<uint8_t>(((total - first) < kWeatherCatalogRowsPerPage)
                                                           ? total - first
                                                           : kWeatherCatalogRowsPerPage)
                                : 0;
    for (uint8_t i = 0; i < visible; ++i) {
      if (pointInRect(weatherLocationRowBounds(i), x, y)) {
        return true;
      }
    }
    return false;
  }
  for (uint8_t i = 0; i < count; ++i) {
    if (pointInRect(weatherLocationRowBounds(i), x, y)) {
      return true;
    }
  }
  return false;
}

bool MainScreen::handleWeatherLocationTouch(int32_t x, int32_t y) {
  if (!weatherLocationManager_) {
    return false;
  }

  lastTouchAction_ = TouchAction::WeatherLocationAction;
  if (weatherLocationView_ == WeatherLocationView::Menu) {
    if (pointInRect(weatherLocationEnterButtonBounds(), x, y)) {
      weatherLocationSelectionRequired_ = false;
      if (!weatherHasActiveForecast()) {
        requestWeatherForActiveLocation();
      }
      openPage(Page::WeatherStation);
      return true;
    }
    if (pointInRect(weatherLocationMenuButtonBounds(0), x, y)) {
      weatherLocationManager_->useGpsLocation();
      weatherLocationSelectionRequired_ = false;
      requestWeatherForActiveLocation();
      openPage(Page::WeatherStation);
      return true;
    }
    if (pointInRect(weatherLocationMenuButtonBounds(1), x, y)) {
      weatherLocationView_ = WeatherLocationView::Favorites;
      weatherLocationNotice_[0] = '\0';
      refreshActivePage();
      return true;
    }
    if (pointInRect(weatherLocationMenuButtonBounds(2), x, y)) {
      weatherCatalogPage_ = 0;
      weatherCatalogFilterDirty_ = true;
      weatherCatalogLastProgress_ = 255;
      weatherCatalogLastState_ = 255;
      weatherLocationNotice_[0] = '\0';
      beginWeatherCatalogMatchRebuild();
      refreshActivePage();
      return true;
    }
    if (pointInRect(weatherLocationMenuButtonBounds(3), x, y)) {
      weatherLocationView_ = WeatherLocationView::Manual;
      weatherLocationNotice_[0] = '\0';
      prepareManualWeatherCoordinates();
      refreshActivePage();
      return true;
    }
    return false;
  }

  if (weatherLocationView_ == WeatherLocationView::CatalogSearch) {
    static const char* const kSearchRows[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
    static const uint8_t kSearchCols[] = {10, 9, 7};
    for (uint8_t row = 0; row < 3; ++row) {
      for (uint8_t col = 0; col < kSearchCols[row]; ++col) {
        if (pointInRect(weatherCatalogSearchKeyBounds(row, col), x, y)) {
          appendWeatherCatalogSearchChar(kSearchRows[row][col]);
          refreshActivePage();
          return true;
        }
      }
    }
    if (pointInRect(weatherCatalogSearchSpecialBounds(0), x, y)) {
      appendWeatherCatalogSearchChar(' ');
      refreshActivePage();
      return true;
    }
    if (pointInRect(weatherCatalogSearchSpecialBounds(1), x, y)) {
      const size_t length = strlen(weatherCatalogQuery_);
      if (length > 0) {
        weatherCatalogQuery_[length - 1] = '\0';
        weatherCatalogFilterDirty_ = true;
        weatherCatalogPage_ = 0;
      }
      refreshActivePage();
      return true;
    }
    if (pointInRect(weatherCatalogSearchSpecialBounds(2), x, y)) {
      weatherCatalogQuery_[0] = '\0';
      weatherCatalogFilterDirty_ = true;
      weatherCatalogPage_ = 0;
      refreshActivePage();
      return true;
    }
    if (pointInRect(weatherCatalogSearchSpecialBounds(3), x, y)) {
      beginWeatherCatalogMatchRebuild();
      refreshActivePage();
      return true;
    }
    return false;
  }

  if (weatherLocationView_ == WeatherLocationView::CatalogStates) {
    for (uint8_t i = 0; i < kBrazilStateCount; ++i) {
      if (!pointInRect(weatherCatalogStateChoiceBounds(i), x, y)) {
        continue;
      }
      snprintf(weatherCatalogState_, sizeof(weatherCatalogState_), "%s", kBrazilStates[i]);
      weatherCatalogFilterDirty_ = true;
      weatherCatalogPage_ = 0;
      beginWeatherCatalogMatchRebuild();
      refreshActivePage();
      return true;
    }
    return false;
  }

  if (weatherLocationView_ == WeatherLocationView::Manual) {
    if (pointInRect(weatherManualFieldBounds(0), x, y)) {
      weatherManualField_ = 0;
      refreshActivePage();
      return true;
    }
    if (pointInRect(weatherManualFieldBounds(1), x, y)) {
      weatherManualField_ = 1;
      refreshActivePage();
      return true;
    }
    static const char kKeys[4][3] = {
        {'1', '2', '3'},
        {'4', '5', '6'},
        {'7', '8', '9'},
        {'-', '0', '.'},
    };
    for (uint8_t row = 0; row < 4; ++row) {
      for (uint8_t col = 0; col < 3; ++col) {
        if (pointInRect(weatherManualKeyBounds(row, col), x, y)) {
          appendManualCoordinateChar(kKeys[row][col]);
          refreshActivePage();
          return true;
        }
      }
    }
    if (pointInRect(weatherManualDeleteButtonBounds(), x, y)) {
      char* text = weatherManualField_ == 0 ? weatherManualLatitude_ : weatherManualLongitude_;
      const size_t length = strlen(text);
      if (length > 0) {
        text[length - 1] = '\0';
      }
      refreshActivePage();
      return true;
    }
    if (pointInRect(weatherManualSaveButtonBounds(), x, y)) {
      if (saveManualWeatherCoordinates()) {
        weatherLocationSelectionRequired_ = false;
        requestWeatherForActiveLocation();
        openPage(Page::WeatherStation);
      } else {
        refreshActivePage();
      }
      return true;
    }
    return false;
  }

  const bool favoritesView = weatherLocationView_ == WeatherLocationView::Favorites;
  if (!favoritesView) {
    if (weatherCatalogFilterDirty_) {
      beginWeatherCatalogMatchRebuild();
      refreshActivePage();
      return true;
    }
    const uint16_t count = weatherCatalogMatchCount_;
    const uint16_t pageCount =
        static_cast<uint16_t>((count + kWeatherCatalogRowsPerPage - 1) / kWeatherCatalogRowsPerPage);
    if (pointInRect(weatherCatalogSearchButtonBounds(), x, y)) {
      weatherLocationView_ = WeatherLocationView::CatalogSearch;
      refreshActivePage();
      return true;
    }
    if (pointInRect(weatherCatalogStateButtonBounds(), x, y)) {
      weatherLocationView_ = WeatherLocationView::CatalogStates;
      refreshActivePage();
      return true;
    }
    if (pointInRect(weatherCatalogControlBounds(0), x, y)) {
      if (weatherCatalogPage_ > 0) {
        --weatherCatalogPage_;
        refreshActivePage();
      }
      return true;
    }
    if (pointInRect(weatherCatalogControlBounds(2), x, y)) {
      if (weatherCatalogPage_ + 1 < pageCount) {
        ++weatherCatalogPage_;
        refreshActivePage();
      }
      return true;
    }
    if (pointInRect(weatherCatalogControlBounds(1), x, y)) {
      if (flightSiteCatalogUpdater_) {
        if (flightSiteCatalogUpdater_->busy()) {
          flightSiteCatalogUpdater_->cancel();
        } else {
          flightSiteCatalogUpdater_->requestUpdate();
        }
      }
      refreshActivePage();
      return true;
    }
  }

  const uint16_t count = favoritesView ? weatherLocationManager_->favoriteCount() : weatherCatalogMatchCount_;
  const uint16_t firstIndex = favoritesView ? 0 : weatherCatalogPage_ * kWeatherCatalogRowsPerPage;
  const uint8_t visibleCount = favoritesView
                                   ? static_cast<uint8_t>(count)
                                   : (firstIndex < count
                                          ? static_cast<uint8_t>(((count - firstIndex) < kWeatherCatalogRowsPerPage)
                                                                     ? count - firstIndex
                                                                     : kWeatherCatalogRowsPerPage)
                                          : 0);
  for (uint8_t i = 0; i < visibleCount; ++i) {
    const FlightSite* site = favoritesView ? weatherLocationManager_->favorite(i) : weatherCatalogFilteredSite(firstIndex + i);
    if (!site || !pointInRect(weatherLocationRowBounds(i), x, y)) {
      continue;
    }

    if (pointInRect(weatherLocationRowActionBounds(i), x, y)) {
      if (favoritesView) {
        weatherLocationManager_->removeFavorite(i);
        snprintf(weatherLocationNotice_, sizeof(weatherLocationNotice_), "FAVORITO REMOVIDO");
      } else if (weatherLocationManager_->isFavorite(*site)) {
        for (uint8_t favoriteIndex = 0; favoriteIndex < weatherLocationManager_->favoriteCount(); ++favoriteIndex) {
          const FlightSite* favorite = weatherLocationManager_->favorite(favoriteIndex);
          if (favorite && favorite->id == site->id) {
            weatherLocationManager_->removeFavorite(favoriteIndex);
            break;
          }
        }
        snprintf(weatherLocationNotice_, sizeof(weatherLocationNotice_), "FAVORITO REMOVIDO");
      } else if (weatherLocationManager_->addFavorite(*site)) {
        snprintf(weatherLocationNotice_, sizeof(weatherLocationNotice_), "RAMPA ADICIONADA AOS FAVORITOS");
      } else {
        snprintf(weatherLocationNotice_, sizeof(weatherLocationNotice_), "LIMITE DE %u FAVORITOS", WeatherLocationManager::kMaxFavorites);
      }
      refreshActivePage();
      return true;
    }

    if (weatherLocationManager_->selectSite(*site)) {
      weatherLocationSelectionRequired_ = false;
      requestWeatherForActiveLocation();
      openPage(Page::WeatherStation);
    } else {
      snprintf(weatherLocationNotice_, sizeof(weatherLocationNotice_), "RAMPA INVALIDA");
      refreshActivePage();
    }
    return true;
  }
  return false;
}

bool MainScreen::weatherHasActiveForecast() const {
  if (!weatherClient_ || !weatherClient_->hasData()) {
    return false;
  }
  return !weatherLocationManager_ || weatherClient_->dataMatches(weatherLocationManager_->locationKey());
}

void MainScreen::requestWeatherForActiveLocation() {
  weatherDayIndex_ = 0;
  weatherInfoPopupVisible_ = false;
  if (!weatherClient_) {
    return;
  }
  if (!weatherLocationManager_) {
    weatherClient_->request(lastData_.latitudeDeg, lastData_.longitudeDeg, lastData_.pressureHpa);
    return;
  }

  weatherLocationManager_->updateGpsLocation(lastData_.latitudeDeg, lastData_.longitudeDeg, lastData_.gpsFix);
  const float localPressure =
      weatherLocationManager_->source() == WeatherLocationSource::GpsCurrent ? lastData_.pressureHpa : 0.0F;
  weatherClient_->request(weatherLocationManager_->latitude(),
                          weatherLocationManager_->longitude(),
                          localPressure,
                          weatherLocationManager_->locationKey());
}

void MainScreen::prepareManualWeatherCoordinates() {
  weatherManualField_ = 0;
  if (weatherLocationManager_ && weatherLocationManager_->hasValidLocation()) {
    snprintf(weatherManualLatitude_, sizeof(weatherManualLatitude_), "%.6f", weatherLocationManager_->latitude());
    snprintf(weatherManualLongitude_, sizeof(weatherManualLongitude_), "%.6f", weatherLocationManager_->longitude());
  } else {
    weatherManualLatitude_[0] = '\0';
    weatherManualLongitude_[0] = '\0';
  }
}

bool MainScreen::appendManualCoordinateChar(char value) {
  char* text = weatherManualField_ == 0 ? weatherManualLatitude_ : weatherManualLongitude_;
  const size_t capacity = weatherManualField_ == 0 ? sizeof(weatherManualLatitude_) : sizeof(weatherManualLongitude_);
  size_t length = strlen(text);
  if (length + 1 >= capacity) {
    return false;
  }
  if (value == '-') {
    if (length == 0) {
      text[0] = '-';
      text[1] = '\0';
      return true;
    }
    if (text[0] == '-') {
      memmove(text, text + 1, length);
      return true;
    }
    memmove(text + 1, text, length + 1);
    text[0] = '-';
    return true;
  }
  if (value == '.' && strchr(text, '.') != nullptr) {
    return false;
  }
  text[length] = value;
  text[length + 1] = '\0';
  return true;
}

bool MainScreen::saveManualWeatherCoordinates() {
  char* latEnd = nullptr;
  char* lonEnd = nullptr;
  const double latitude = strtod(weatherManualLatitude_, &latEnd);
  const double longitude = strtod(weatherManualLongitude_, &lonEnd);
  const bool parsed = latEnd && lonEnd && latEnd != weatherManualLatitude_ && lonEnd != weatherManualLongitude_ &&
                      *latEnd == '\0' && *lonEnd == '\0';
  if (!parsed || !isfinite(latitude) || !isfinite(longitude) || latitude < -90.0 || latitude > 90.0 ||
      longitude < -180.0 || longitude > 180.0 || (fabs(latitude) < 0.0001 && fabs(longitude) < 0.0001)) {
    snprintf(weatherLocationNotice_, sizeof(weatherLocationNotice_), "COORDENADA INVALIDA");
    return false;
  }
  if (!weatherLocationManager_->setManualLocation("COORDENADA MANUAL", latitude, longitude)) {
    snprintf(weatherLocationNotice_, sizeof(weatherLocationNotice_), "NAO FOI POSSIVEL SALVAR");
    return false;
  }
  weatherLocationNotice_[0] = '\0';
  return true;
}

void MainScreen::renderThermalInfoPopup() {
  const Rect_t popup = thermalInfoPopupBounds();
  display_.fillRect(popup, AppConfig::kWhite);
  display_.drawRect(popup, AppConfig::kBlack);
  display_.drawRect({popup.x + 2, popup.y + 2, popup.width - 4, popup.height - 4}, AppConfig::kBlack);

  const Rect_t close = thermalInfoCloseButtonBounds();
  display_.drawRoundRect(close, 7, AppConfig::kBlack);
  drawThickLine(display_, close.x + 12, close.y + 10, close.x + close.width - 12, close.y + close.height - 10, AppConfig::kBlack);
  drawThickLine(display_, close.x + close.width - 12, close.y + 10, close.x + 12, close.y + close.height - 10, AppConfig::kBlack);

  const Rect_t up = thermalInfoScrollUpButtonBounds();
  const Rect_t down = thermalInfoScrollDownButtonBounds();
  display_.drawRoundRect(up, 7, AppConfig::kBlack);
  display_.drawRoundRect(down, 7, AppConfig::kBlack);
  const int32_t upCx = up.x + up.width / 2;
  const int32_t upCy = up.y + up.height / 2;
  const int32_t downCx = down.x + down.width / 2;
  const int32_t downCy = down.y + down.height / 2;
  if (thermalInfoScrollPage_ > 0) {
    display_.fillTriangle(upCx, upCy - 12, upCx - 13, upCy + 10, upCx + 13, upCy + 10, AppConfig::kBlack);
  } else {
    display_.drawSmallTextAligned("-", upCx, upCy - 8, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  }
  if (thermalInfoScrollPage_ + 1 < kThermalInfoPageCount) {
    display_.fillTriangle(downCx, downCy + 12, downCx - 13, downCy - 10, downCx + 13, downCy - 10, AppConfig::kBlack);
  } else {
    display_.drawSmallTextAligned("-", downCx, downCy - 8, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  }

  display_.drawSmallTextBoldAligned("TERMICA EM PALAVRAS SIMPLES", popup.x + popup.width / 2, popup.y + 14, 3, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  const int32_t left = popup.x + 34;
  const int32_t top = popup.y + 70;
  const int32_t lineStep = 24;
  const char* title = "TERMICA";
  const char* lines[9] = {};
  uint8_t lineCount = 0;

  switch (thermalInfoScrollPage_) {
    case 0:
      title = "O QUE E UMA TERMICA";
      lines[lineCount++] = "- Termica e ar subindo, como uma bolha invisivel.";
      lines[lineCount++] = "- O piloto gira para ficar dentro dessa subida.";
      lines[lineCount++] = "- O vario apita quando voce esta ganhando altura.";
      lines[lineCount++] = "- O assistente marca onde essa subida apareceu.";
      lines[lineCount++] = "- Quanto melhor a subida, mais forte fica a marca.";
      lines[lineCount++] = "- Sem GPS fixo, ele esconde a estimativa no mapa.";
      break;
    case 1:
      title = "COMO LER NO MAPA";
      lines[lineCount++] = "- Pontos maiores/escuros = subida mais forte.";
      lines[lineCount++] = "- A seta indica para onde procurar o miolo.";
      lines[lineCount++] = "- Miolo e a parte que sobe melhor dentro da termica.";
      lines[lineCount++] = "- NUCLEO % alto = centro mais confiavel.";
      lines[lineCount++] = "- Historico mostra marcas de subidas recentes.";
      lines[lineCount++] = "- AG = agora; 5m = visto ha cinco minutos.";
      lines[lineCount++] = "- Use isso para voltar ao miolo se sair dele.";
      break;
    default:
      title = "CUIDADOS EM VOO";
      lines[lineCount++] = "- NAO e uma previsao de novas termicas.";
      lines[lineCount++] = "- Ele so ajuda a ler a termica que voce achou.";
      lines[lineCount++] = "- O vento empurra a termica; ele acompanha sozinho.";
      lines[lineCount++] = "- GPS ruim ou muito vento reduzem a precisao.";
      lines[lineCount++] = "- Confirme com o vario, a vela e seu corpo.";
      lines[lineCount++] = "- Prioridade: olhar fora e manter separaçao.";
      break;
  }

  display_.drawSmallTextBold(title, left, top, 2, AppConfig::kBlack);
  for (uint8_t i = 0; i < lineCount; ++i) {
    display_.drawSmallText(lines[i], left, top + 34 + lineStep * i, 2, AppConfig::kBlack);
  }

  char pageText[12];
  snprintf(pageText, sizeof(pageText), "PAG %u/%u", static_cast<unsigned>(thermalInfoScrollPage_ + 1),
           static_cast<unsigned>(kThermalInfoPageCount));
  display_.drawSmallTextBoldAligned(pageText, popup.x + popup.width / 2, popup.y + popup.height - 34, 2, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
}

void MainScreen::renderThermalCycleInfoPopup() {
  const Rect_t popup = thermalCycleInfoPopupBounds();
  display_.fillRect(popup, AppConfig::kWhite);
  display_.drawRect(popup, AppConfig::kBlack);
  display_.drawRect({popup.x + 2, popup.y + 2, popup.width - 4, popup.height - 4}, AppConfig::kBlack);

  const Rect_t close = thermalCycleInfoCloseButtonBounds();
  display_.drawRoundRect(close, 7, AppConfig::kBlack);
  drawThickLine(display_, close.x + 12, close.y + 10, close.x + close.width - 12, close.y + close.height - 10, AppConfig::kBlack);
  drawThickLine(display_, close.x + close.width - 12, close.y + 10, close.x + 12, close.y + close.height - 10, AppConfig::kBlack);

  const Rect_t up = thermalCycleInfoScrollUpButtonBounds();
  const Rect_t down = thermalCycleInfoScrollDownButtonBounds();
  display_.drawRoundRect(up, 7, AppConfig::kBlack);
  display_.drawRoundRect(down, 7, AppConfig::kBlack);
  const int32_t upCx = up.x + up.width / 2;
  const int32_t upCy = up.y + up.height / 2;
  const int32_t downCx = down.x + down.width / 2;
  const int32_t downCy = down.y + down.height / 2;
  if (thermalCycleInfoScrollPage_ > 0) {
    display_.fillTriangle(upCx, upCy - 12, upCx - 13, upCy + 10, upCx + 13, upCy + 10, AppConfig::kBlack);
  } else {
    display_.drawSmallTextAligned("-", upCx, upCy - 8, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  }
  if (thermalCycleInfoScrollPage_ + 1 < kThermalCycleInfoPageCount) {
    display_.fillTriangle(downCx, downCy + 12, downCx - 13, downCy - 10, downCx + 13, downCy - 10, AppConfig::kBlack);
  } else {
    display_.drawSmallTextAligned("-", downCx, downCy - 8, 2, AppConfig::kBlack, EpdDisplay::Align::Center);
  }

  display_.drawSmallTextBoldAligned("CICLO TERMAL BETA - INFO", popup.x + popup.width / 2, popup.y + 14, 3, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  const int32_t left = popup.x + 34;
  const int32_t top = popup.y + 70;
  const int32_t lineStep = 22;
  const char* title = "EXPERIMENTAL";
  const char* lines[9] = {};
  uint8_t lineCount = 0;

  switch (thermalCycleInfoScrollPage_) {
    case 0:
      title = "O QUE ESTE BETA FAZ";
      lines[lineCount++] = "- Procura pulsos barometricos repetidos no solo.";
      lines[lineCount++] = "- O BRVARIO deve ficar parado no mesmo ponto.";
      lines[lineCount++] = "- Usa somente a pressao medida pelo BMP280.";
      lines[lineCount++] = "- Nao usa GPS, vento, temperatura, umidade ou IA.";
      lines[lineCount++] = "- A saida e uma hipotese local, nao uma certeza.";
      lines[lineCount++] = "- Ele ajuda a observar ritmo, nao decide decolagem.";
      break;
    case 1:
      title = "COMO A PRESSAO VIRA SINAL";
      lines[lineCount++] = "- A cada cerca de 1 s ele junta as amostras brutas.";
      lines[lineCount++] = "- Nos 5 min iniciais aprende a base do local.";
      lines[lineCount++] = "- EMA curta acompanha mudancas recentes de pressao.";
      lines[lineCount++] = "- EMA longa acompanha a tendencia lenta do dia.";
      lines[lineCount++] = "- Sinal = EMA curta menos EMA longa.";
      lines[lineCount++] = "- Tambem calcula ruido e velocidade dP/dt.";
      break;
    case 2:
      title = "EVENTO, CICLO E CONFIANCA";
      lines[lineCount++] = "- Um evento nasce quando sinal e dP/dt vencem ruido.";
      lines[lineCount++] = "- Eventos muito perto sao ignorados por 3 min.";
      lines[lineCount++] = "- Intervalos validos ficam entre 3 e 40 min.";
      lines[lineCount++] = "- O ciclo mostrado usa mediana e media dos intervalos.";
      lines[lineCount++] = "- Previsao = ultimo evento + ciclo mediano.";
      lines[lineCount++] = "- Confianca sobe com repeticao e regularidade.";
      lines[lineCount++] = "- Confianca cai com ruido alto e pulso fraco.";
      break;
    case 3:
      title = "LIMITES FISICOS";
      lines[lineCount++] = "- Um sensor parado mede so um ponto da atmosfera.";
      lines[lineCount++] = "- Ele nao sabe direcao, distancia nem tamanho da bolha.";
      lines[lineCount++] = "- Rajada, vortex, onda, turbulencia ou frente enganam.";
      lines[lineCount++] = "- Mover o aparelho parece variacao de altitude.";
      lines[lineCount++] = "- Caixa aquecida ou abafada pode deformar a leitura.";
      lines[lineCount++] = "- Pressao sinotica lenta e filtrada, mas nao some toda.";
      break;
    default:
      title = "USO SEGURO E TELA";
      lines[lineCount++] = "- Deixe o BRVARIO fixo, sombreado e ventilado.";
      lines[lineCount++] = "- Evite tocar ou mover durante a coleta.";
      lines[lineCount++] = "- Use no minimo 5 min; 30-60 min fica melhor.";
      lines[lineCount++] = "- A tela atualiza parcial a cada 5 s nesta pagina.";
      lines[lineCount++] = "- Um full refresh preventivo ocorre a cada 10 min.";
      lines[lineCount++] = "- Isso reduz fantasmas no e-paper em uso por horas.";
      lines[lineCount++] = "- Em voo, confirme tudo pelo vario real e seguranca.";
      break;
  }

  display_.drawSmallTextBold(title, left, top, 2, AppConfig::kBlack);
  for (uint8_t i = 0; i < lineCount; ++i) {
    display_.drawSmallText(lines[i], left, top + 32 + lineStep * i, 2, AppConfig::kBlack);
  }

  char pageText[12];
  snprintf(pageText, sizeof(pageText), "PAG %u/%u", static_cast<unsigned>(thermalCycleInfoScrollPage_ + 1),
           static_cast<unsigned>(kThermalCycleInfoPageCount));
  display_.drawSmallTextBoldAligned(pageText, popup.x + popup.width / 2, popup.y + popup.height - 34, 2, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
}

void MainScreen::renderThermalCycleBetaPage() {
  thermalCyclePage_.render(display_, layout_.screen, layout_.trend.y, thermalCycleBeta_.snapshot(millis()));
  renderPageFooter(Page::ThermalCycleBeta);
  const Rect_t infoButton = thermalCycleInfoButtonBounds();
  display_.drawRoundRect(infoButton, 7, AppConfig::kBlack);
  display_.drawCircle(infoButton.x + infoButton.width / 2, infoButton.y + infoButton.height / 2, 13, AppConfig::kBlack);
  display_.drawSmallTextBoldAligned("i", infoButton.x + infoButton.width / 2, infoButton.y + 10, 3, AppConfig::kBlack,
                                    EpdDisplay::Align::Center);
  if (thermalCycleInfoPopupVisible_) {
    renderThermalCycleInfoPopup();
  }
}

void MainScreen::renderPilotProfilePage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height,
                    AppConfig::kBlack);

  display_.drawSmallTextBoldAligned("DADOS DO PILOTO", s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);
  display_.drawSmallTextAligned("USADO NO CABECALHO DOS ARQUIVOS IGC", s.x + s.width / 2, s.y + 58, 2, AppConfig::kBlack,
                                EpdDisplay::Align::Center);

  if (!pilotProfile_) {
    display_.drawSmallTextBoldAligned("PERFIL NAO INICIADO", s.x + s.width / 2, s.y + 180, 3, AppConfig::kBlack,
                                      EpdDisplay::Align::Center);
    renderPageFooter(Page::PilotProfile);
    return;
  }

  for (uint8_t i = 0; i < static_cast<uint8_t>(PilotProfileConfig::Field::Count); ++i) {
    const Rect_t field = profileFieldBounds(i);
    const PilotProfileConfig::Field profileField = static_cast<PilotProfileConfig::Field>(i);
    display_.drawRoundRect(field, 6, AppConfig::kBlack);
    if (profileSelectedField_ == i) {
      display_.drawRoundRect({field.x + 2, field.y + 2, field.width - 4, field.height - 4}, 4, AppConfig::kBlack);
    }
    display_.drawSmallTextBold(pilotProfile_->fieldLabel(profileField), field.x + 10, field.y + 8, 2, AppConfig::kBlack);
    drawPilotProfileFieldValue(i);
  }

  for (uint8_t row = 0; row < 4; ++row) {
    const uint8_t cols = row == 2 ? 9 : 10;
    for (uint8_t col = 0; col < cols; ++col) {
      const Rect_t key = profileKeyBounds(row, col);
      char keyText[2] = {profileKeyAt(row, col), '\0'};
      drawButton(key, keyText, 2);
    }
  }
  drawButton(profileSpecialKeyBounds(0), profileKeyboardMode_ == 1 ? "CAPS ON" : "CAPS", 2);
  drawButton(profileSpecialKeyBounds(1), profileKeyboardMode_ == 2 ? "ABC" : "SIMB", 2);
  drawButton(profileSpecialKeyBounds(2), "ESPACO", 2);
  drawButton(profileSpecialKeyBounds(3), "APAGAR", 2);

  renderPageFooter(Page::PilotProfile);
}

void MainScreen::drawWifiPasswordValue() {
  const Rect_t& s = layout_.screen;
  const int32_t infoX = s.x + 500;
  String passwordText = wifiPassword_;
  if (passwordText.length() > 26) {
    passwordText = passwordText.substring(passwordText.length() - 26);
  }
  const Rect_t valueArea = {infoX + 88, s.y + 218, s.x + s.width - infoX - 112, 34};
  display_.fillRect(valueArea, AppConfig::kWhite);
  display_.drawSmallTextBold(passwordText.length() > 0 ? passwordText.c_str() : "---", infoX + 94, s.y + 226, 2, AppConfig::kBlack);
}

void MainScreen::refreshWifiPasswordValue() {
  drawWifiPasswordValue();
  const Rect_t& s = layout_.screen;
  const int32_t infoX = s.x + 500;
  const Rect_t valueArea = {infoX + 88, s.y + 218, s.x + s.width - infoX - 112, 34};
  display_.updateAreas(&valueArea, 1);
  wifiLastRefreshMs_ = millis();
}

void MainScreen::drawPilotProfileFieldValue(uint8_t index) {
  if (!pilotProfile_ || index >= static_cast<uint8_t>(PilotProfileConfig::Field::Count)) {
    return;
  }
  const Rect_t field = profileFieldBounds(index);
  const Rect_t valueArea = {field.x + 246, field.y + 4, field.width - 258, field.height - 8};
  display_.fillRect(valueArea, AppConfig::kWhite);
  const PilotProfileConfig::Field profileField = static_cast<PilotProfileConfig::Field>(index);
  const char* value = pilotProfile_->fieldText(profileField);
  display_.drawSmallTextBold(value[0] != '\0' ? value : "---", field.x + 250, field.y + 8, 2, AppConfig::kBlack);
}

void MainScreen::refreshPilotProfileFieldValue(uint8_t index) {
  if (index >= static_cast<uint8_t>(PilotProfileConfig::Field::Count)) {
    return;
  }
  drawPilotProfileFieldValue(index);
  const Rect_t field = profileFieldBounds(index);
  const Rect_t valueArea = {field.x + 246, field.y + 4, field.width - 258, field.height - 8};
  display_.updateAreas(&valueArea, 1);
}

void MainScreen::savePilotProfileIfDirty() {
  if (!profileDirty_ || !pilotProfile_) {
    return;
  }
  profileSavedNotice_ = pilotProfile_->save();
  profileDirty_ = false;
}

void MainScreen::saveAudioProfileIfDirty() {
  if (!audioProfileDirty_ || !audioBuzzer_) {
    return;
  }
  audioSavedNotice_ = audioBuzzer_->saveProfile();
  audioProfileDirty_ = false;
}

void MainScreen::renderWifiSettingsPage() {
  display_.clearBuffer(AppConfig::kWhite);

  const Rect_t& s = layout_.screen;
  display_.drawRect(s, AppConfig::kBlack);
  display_.drawRect({s.x + 1, s.y + 1, s.width - 2, s.height - 2}, AppConfig::kBlack);
  display_.drawLine(s.x, layout_.header.y + layout_.header.height, s.x + s.width - 1, layout_.header.y + layout_.header.height, AppConfig::kBlack);

  display_.drawSmallTextBoldAligned("CONFIGURAR WIFI", s.x + s.width / 2, s.y + 12, 3, AppConfig::kBlack, EpdDisplay::Align::Center);

  const bool wifiOn = wifiManager_ && wifiManager_->isEnabled();
  drawButton(wifiScanButtonBounds(), "CLIQUE AQUI PARA BUSCAR REDES", 2);
  drawButton(wifiConnectButtonBounds(), "CONECTAR", 2);
  drawButton(wifiClearButtonBounds(), "LIMPAR", 2);

  const int32_t infoX = s.x + 500;
  display_.drawSmallTextBold("STATUS", infoX, s.y + 116, 2, AppConfig::kBlack);
  display_.drawSmallTextBold(wifiManager_ ? wifiManager_->statusText() : "WIFI OFF", infoX + 118, s.y + 116, 2, AppConfig::kBlack);

  String currentIp = wifiManager_ ? wifiManager_->ipText() : String();
  if (currentIp.length() > 0) {
    display_.drawSmallTextBold("IP", infoX, s.y + 146, 2, AppConfig::kBlack);
    display_.drawSmallTextBold(currentIp.c_str(), infoX + 56, s.y + 146, 2, AppConfig::kBlack);
  } else {
    char savedText[24];
    snprintf(savedText,
             sizeof(savedText),
             "SALVAS %u/10",
             static_cast<unsigned>(wifiManager_ ? wifiManager_->savedCredentialCount() : 0));
    display_.drawSmallTextBold(savedText, infoX, s.y + 146, 2, AppConfig::kBlack);
  }

  display_.drawSmallTextBold("SSID:", infoX, s.y + 176, 2, AppConfig::kBlack);
  String selectedSsid = wifiSelectedSsid_.length() > 0 ? wifiSelectedSsid_ : String("---");
  if (selectedSsid.length() > 22) {
    selectedSsid = selectedSsid.substring(0, 22);
  }
  display_.drawSmallTextBold(selectedSsid.c_str(), infoX + 86, s.y + 176, 2, AppConfig::kBlack);

  display_.drawSmallTextBold("SENHA", infoX, s.y + 226, 2, AppConfig::kBlack);
  drawWifiPasswordValue();

  const uint8_t availableCount = wifiManager_ ? wifiManager_->networkCount() : 0;
  const uint8_t count = availableCount > 3 ? 3 : availableCount;
  if (count == 0) {
    const bool scanning = wifiManager_ && wifiManager_->state() == WifiConnectionState::Scanning;
    display_.drawSmallTextAligned(scanning ? "BUSCANDO REDES..." : (wifiOn ? "TOQUE EM BUSCAR REDES" : "LIGANDO WIFI"),
                                  s.x + 254,
                                  s.y + 176,
                                  2,
                                  AppConfig::kBlack,
                                  EpdDisplay::Align::Center);
  }
  for (uint8_t i = 0; i < count; ++i) {
    const Rect_t row = wifiNetworkRowBounds(i);
    const bool selectedRow = wifiManager_->networkSsid(i) == wifiSelectedSsid_;
    display_.drawRoundRect(row, 6, AppConfig::kBlack);
    if (selectedRow) {
      display_.drawRoundRect({row.x + 2, row.y + 2, row.width - 4, row.height - 4}, 4, AppConfig::kBlack);
    }
    String ssid = wifiManager_->networkSsid(i);
    if (ssid.length() > 24) {
      ssid = ssid.substring(0, 24);
    }
    display_.drawSmallTextBold(ssid.c_str(), row.x + 10, row.y + 8, 2, AppConfig::kBlack);
    char rssiText[10];
    snprintf(rssiText, sizeof(rssiText), "%ld", static_cast<long>(wifiManager_->networkRssi(i)));
    display_.drawSmallTextBoldAligned(rssiText, row.x + row.width - 10, row.y + 8, 2, AppConfig::kBlack, EpdDisplay::Align::Right);
  }

  for (uint8_t row = 0; row < 4; ++row) {
    const uint8_t cols = row == 2 ? 9 : 10;
    for (uint8_t col = 0; col < cols; ++col) {
      const Rect_t key = wifiKeyBounds(row, col);
      char keyText[2] = {wifiKeyAt(row, col), '\0'};
      drawButton(key, keyText, 2);
    }
  }
  drawButton(wifiSpecialKeyBounds(0), wifiKeyboardMode_ == 1 ? "CAPS ON" : "CAPS", 2);
  drawButton(wifiSpecialKeyBounds(1), wifiKeyboardMode_ == 2 ? "ABC" : "SIMB", 2);
  drawButton(wifiSpecialKeyBounds(2), "ESPACO", 2);
  drawButton(wifiSpecialKeyBounds(3), "APAGAR", 2);

  renderPageFooter(Page::WifiSettings);
}

void MainScreen::renderPageFooter(Page page) {
  const Rect_t& s = layout_.screen;
  const int32_t footerTop = layout_.trend.y;
  const int32_t buttonW = layout_.footerButtons.width / 5;
  const int32_t cy = layout_.footerButtons.y + layout_.footerButtons.height / 2;

  display_.fillRect(layout_.footerButtons, AppConfig::kWhite);
  if (page == Page::Map) {
    display_.drawLine(layout_.footerButtons.x, footerTop, layout_.footerButtons.x + layout_.footerButtons.width - 1, footerTop, kFooterRuleInk);
  } else {
    display_.drawLine(s.x, footerTop, s.x + s.width - 1, footerTop, kFooterRuleInk);
  }
  drawFooterButtonFrames(display_, layout_.footerButtons, kFooterInk);

  if (page == Page::Dashboard || pageUsesFooterSettings(page)) {
    drawSettingsIcon(display_, layout_.footerButtons.x + buttonW / 2, cy, kFooterInk);
  } else {
    drawBackIcon(display_, layout_.footerButtons.x + buttonW / 2, cy, kFooterInk);
  }
  const bool tracklogActive = page == Page::Dashboard ? lastData_.trackingEnabled : page == Page::Tracklog;
  drawAudioIcon(display_, layout_.footerButtons.x + buttonW + buttonW / 2, cy, lastData_.audioEnabled, kFooterInk);
  drawTracklogIcon(display_, layout_.footerButtons.x + buttonW * 2 + buttonW / 2, cy, tracklogActive, kFooterInk);
  drawMapIcon(display_, layout_.footerButtons.x + buttonW * 3 + buttonW / 2, cy, kFooterInk);
  if (page == Page::Dashboard) {
    drawPowerIcon(display_, layout_.footerButtons.x + buttonW * 4 + buttonW / 2, cy, kFooterInk);
  } else {
    drawBrvarioHomeIcon(display_, layout_.footerButtons.x + buttonW * 4 + buttonW / 2, cy, kFooterInk);
  }
}

void MainScreen::registerTouchZones() {
  touchZoneCount_ = 0;
  const int32_t buttonW = layout_.footerButtons.width / 5;
  const int32_t touchY = layout_.footerButtons.y - 24;
  const int32_t touchH = EPD_HEIGHT - touchY;
  addTouchZone({layout_.footerButtons.x, touchY, buttonW, touchH}, TouchAction::OpenSettings);
  addTouchZone({layout_.footerButtons.x + buttonW, touchY, buttonW, touchH}, TouchAction::ToggleAudio);
  addTouchZone({layout_.footerButtons.x + buttonW * 2, touchY, buttonW, touchH}, TouchAction::ToggleTracklog);
  addTouchZone({layout_.footerButtons.x + buttonW * 3, touchY, buttonW, touchH}, TouchAction::NextPage);
  addTouchZone({layout_.footerButtons.x + buttonW * 4, touchY, layout_.footerButtons.width - buttonW * 4, touchH}, TouchAction::PowerRequest);
}

void MainScreen::registerPageTouchZones() {
  touchZoneCount_ = 0;
  const int32_t buttonW = layout_.footerButtons.width / 5;
  const int32_t touchY = layout_.footerButtons.y - 24;
  const int32_t touchH = EPD_HEIGHT - touchY;
  addTouchZone({layout_.footerButtons.x, touchY, buttonW, touchH},
               pageUsesFooterSettings(activePage_) ? TouchAction::OpenSettings : TouchAction::Back);
  addTouchZone({layout_.footerButtons.x + buttonW, touchY, buttonW, touchH}, TouchAction::ToggleAudio);
  addTouchZone({layout_.footerButtons.x + buttonW * 2, touchY, buttonW, touchH}, TouchAction::ToggleTracklog);
  addTouchZone({layout_.footerButtons.x + buttonW * 3, touchY, buttonW, touchH}, TouchAction::NextPage);
  addTouchZone({layout_.footerButtons.x + buttonW * 4, touchY, layout_.footerButtons.width - buttonW * 4, touchH}, TouchAction::Home);

  if (activePage_ == Page::Settings) {
    addTouchZone(settingsDashboardLayoutTouchBounds(), TouchAction::OpenDashboardLayout);
    addTouchZone(settingsThermalAssistTouchBounds(), TouchAction::OpenThermalAssistSettings);
    addTouchZone(settingsAudioEditorTouchBounds(), TouchAction::OpenAudioEditor);
    addTouchZone(settingsWeatherStationTouchBounds(), TouchAction::OpenWeatherStation);
    addTouchZone(settingsPilotProfileTouchBounds(), TouchAction::OpenPilotProfile);
    addTouchZone(settingsThermalCycleTouchBounds(), TouchAction::OpenThermalCycleBeta);
    addTouchZone(settingsManualTouchBounds(), TouchAction::OpenManual);
    addTouchZone(settingsWifiTouchBounds(), TouchAction::OpenWifiSettings);
    addTouchZone(settingsFirmwareUpdateTouchBounds(), TouchAction::OpenFirmwareUpdate);
    addTouchZone(settingsStorageTouchBounds(), TouchAction::OpenStorageManager);
    addTouchZone(settingsDeviceInfoTouchBounds(), TouchAction::OpenDeviceInfo);
    addTouchZone(settingsSystemStatusTouchBounds(), TouchAction::OpenSystemStatus);
    addTouchZone(settingsAdvancedSystemTouchBounds(), TouchAction::OpenAdvancedSystem);
  } else if (activePage_ == Page::AudioEditor) {
    addTouchZone(audioAdjustButtonBounds(0, false), TouchAction::AudioResponseDown);
    addTouchZone(audioAdjustButtonBounds(0, true), TouchAction::AudioResponseUp);
    addTouchZone(audioAdjustButtonBounds(1, false), TouchAction::AudioPitchDown);
    addTouchZone(audioAdjustButtonBounds(1, true), TouchAction::AudioPitchUp);
    addTouchZone(audioVoiceToggleButtonBounds(), TouchAction::AudioVoiceToggle);
    addTouchZone(audioResetButtonBounds(), TouchAction::AudioReset);
  } else if (activePage_ == Page::ThermalAssistSettings) {
    addTouchZone(thermalModeButtonBounds(ThermalAssistVisualMode::PilotCentered), TouchAction::ThermalModePilot);
    addTouchZone(thermalModeButtonBounds(ThermalAssistVisualMode::ThermalCentered), TouchAction::ThermalModeThermal);
    addTouchZone(thermalInfoButtonBounds(), TouchAction::ThermalInfo);
    if (thermalInfoPopupVisible_) {
      addTouchZone(thermalInfoCloseButtonBounds(), TouchAction::ThermalInfo);
      addTouchZone(thermalInfoScrollUpButtonBounds(), TouchAction::ThermalInfoUp);
      addTouchZone(thermalInfoScrollDownButtonBounds(), TouchAction::ThermalInfoDown);
    }
  } else if (activePage_ == Page::ThermalCycleBeta) {
    addTouchZone(thermalCycleInfoButtonBounds(), TouchAction::ThermalCycleInfo);
    if (thermalCycleInfoPopupVisible_) {
      addTouchZone(thermalCycleInfoCloseButtonBounds(), TouchAction::ThermalCycleInfo);
      addTouchZone(thermalCycleInfoScrollUpButtonBounds(), TouchAction::ThermalCycleInfoUp);
      addTouchZone(thermalCycleInfoScrollDownButtonBounds(), TouchAction::ThermalCycleInfoDown);
    }
  } else if (activePage_ == Page::WeatherStation) {
    addTouchZone(weatherInfoButtonBounds(), TouchAction::WeatherInfo);
    if (!weatherStationSleepMode_ && !weatherHasActiveForecast()) {
      addTouchZone(weatherLocationLabelBounds(), TouchAction::OpenWeatherLocation);
    }
    if (weatherInfoPopupVisible_) {
      addTouchZone(weatherInfoCloseButtonBounds(), TouchAction::WeatherInfo);
      addTouchZone(weatherInfoScrollUpButtonBounds(), TouchAction::WeatherInfoUp);
      addTouchZone(weatherInfoScrollDownButtonBounds(), TouchAction::WeatherInfoDown);
    }
    if (weatherClient_ && weatherClient_->hasData() && weatherClient_->forecastDayCount() > 1 &&
        (!weatherLocationManager_ || weatherClient_->dataMatches(weatherLocationManager_->locationKey()))) {
      addTouchZone(weatherPrevDayButtonBounds(), TouchAction::WeatherPrevDay);
      addTouchZone(weatherNextDayButtonBounds(), TouchAction::WeatherNextDay);
    }
  } else if (activePage_ == Page::WeatherLocation) {
    addTouchZone(weatherInfoButtonBounds(), TouchAction::WeatherInfo);
    if (weatherInfoPopupVisible_) {
      addTouchZone(weatherInfoCloseButtonBounds(), TouchAction::WeatherInfo);
      addTouchZone(weatherInfoScrollUpButtonBounds(), TouchAction::WeatherInfoUp);
      addTouchZone(weatherInfoScrollDownButtonBounds(), TouchAction::WeatherInfoDown);
    } else if (weatherLocationView_ == WeatherLocationView::Menu) {
      addTouchZone(weatherLocationEnterButtonBounds(), TouchAction::WeatherLocationAction);
      for (uint8_t i = 0; i < 4; ++i) {
        addTouchZone(weatherLocationMenuButtonBounds(i), TouchAction::WeatherLocationAction);
      }
    } else if (weatherLocationView_ == WeatherLocationView::CatalogLoading) {
      // A leitura do catálogo é incremental; os controles ficam bloqueados até a conclusão.
    } else if (weatherLocationView_ == WeatherLocationView::CatalogSearch) {
      for (uint8_t row = 0; row < 3; ++row) {
        const uint8_t cols = row == 0 ? 10 : (row == 1 ? 9 : 7);
        for (uint8_t col = 0; col < cols; ++col) {
          addTouchZone(weatherCatalogSearchKeyBounds(row, col), TouchAction::WeatherLocationAction);
        }
      }
      for (uint8_t i = 0; i < 4; ++i) {
        addTouchZone(weatherCatalogSearchSpecialBounds(i), TouchAction::WeatherLocationAction);
      }
    } else if (weatherLocationView_ == WeatherLocationView::CatalogStates) {
      for (uint8_t i = 0; i < kBrazilStateCount; ++i) {
        addTouchZone(weatherCatalogStateChoiceBounds(i), TouchAction::WeatherLocationAction);
      }
    } else if (weatherLocationView_ == WeatherLocationView::Manual) {
      addTouchZone(weatherManualFieldBounds(0), TouchAction::WeatherLocationAction);
      addTouchZone(weatherManualFieldBounds(1), TouchAction::WeatherLocationAction);
      for (uint8_t row = 0; row < 4; ++row) {
        for (uint8_t col = 0; col < 3; ++col) {
          addTouchZone(weatherManualKeyBounds(row, col), TouchAction::WeatherLocationAction);
        }
      }
      addTouchZone(weatherManualDeleteButtonBounds(), TouchAction::WeatherLocationAction);
      addTouchZone(weatherManualSaveButtonBounds(), TouchAction::WeatherLocationAction);
    } else {
      const uint16_t total = weatherLocationView_ == WeatherLocationView::Favorites
                                 ? (weatherLocationManager_ ? weatherLocationManager_->favoriteCount() : 0)
                                 : weatherCatalogMatchCount_;
      const uint16_t first =
          weatherLocationView_ == WeatherLocationView::Catalog ? weatherCatalogPage_ * kWeatherCatalogRowsPerPage : 0;
      const uint8_t rowCount =
          first < total
              ? static_cast<uint8_t>(((total - first) < kWeatherCatalogRowsPerPage) ? total - first
                                                                                   : kWeatherCatalogRowsPerPage)
              : 0;
      for (uint8_t i = 0; i < rowCount; ++i) {
        addTouchZone(weatherLocationRowBounds(i), TouchAction::WeatherLocationAction);
        addTouchZone(weatherLocationRowActionBounds(i), TouchAction::WeatherLocationAction);
      }
      if (weatherLocationView_ == WeatherLocationView::Catalog) {
        addTouchZone(weatherCatalogSearchButtonBounds(), TouchAction::WeatherLocationAction);
        addTouchZone(weatherCatalogStateButtonBounds(), TouchAction::WeatherLocationAction);
        addTouchZone(weatherCatalogControlBounds(0), TouchAction::WeatherLocationAction);
        addTouchZone(weatherCatalogControlBounds(1), TouchAction::WeatherLocationAction);
        addTouchZone(weatherCatalogControlBounds(2), TouchAction::WeatherLocationAction);
      }
    }
  } else if (activePage_ == Page::Manual) {
    const Rect_t logic = manualLogicButtonBounds();
    const Rect_t user = manualUserButtonBounds();
    addTouchZone({logic.x - 8, logic.y - 8, logic.width + 16, logic.height + 16}, TouchAction::OpenManualLogic);
    addTouchZone({user.x - 8, user.y - 8, user.width + 16, user.height + 16}, TouchAction::OpenManualUser);
  } else if (activePage_ == Page::ManualLogic || activePage_ == Page::ManualUser) {
    static const TouchAction kManualTabActions[kManualPageCount] = {
        TouchAction::ManualTab0,
        TouchAction::ManualTab1,
        TouchAction::ManualTab2,
        TouchAction::ManualTab3,
        TouchAction::ManualTab4,
    };
    for (uint8_t i = 0; i < kManualPageCount; ++i) {
      const Rect_t tab = manualTabBounds(i);
      addTouchZone({tab.x - 4, tab.y - 8, tab.width + 8, tab.height + 16}, kManualTabActions[i]);
    }
    addTouchZone(manualScrollUpButtonBounds(), TouchAction::ManualPageUp);
    addTouchZone(manualScrollDownButtonBounds(), TouchAction::ManualPageDown);
  } else if (activePage_ == Page::Storage) {
    addTouchZone(storageDownloadButtonBounds(), TouchAction::OpenMapDownload);
    addTouchZone(storageRefreshButtonBounds(), TouchAction::StorageRefresh);
    addTouchZone(storageClearMapsButtonBounds(), TouchAction::StorageClearMaps);
  } else if (activePage_ == Page::AdvancedSystem) {
    if (advancedConfirmAction_ != TouchAction::None) {
      addTouchZone(advancedConfirmYesButtonBounds(), TouchAction::AdvancedConfirmYes);
      addTouchZone(advancedConfirmNoButtonBounds(), TouchAction::AdvancedConfirmNo);
    } else {
      addTouchZone(advancedRecoverDisplayButtonBounds(), TouchAction::AdvancedRecoverDisplay);
      addTouchZone(advancedMoveIgcButtonBounds(), TouchAction::AdvancedMoveIgcToSd);
      addTouchZone(advancedClearWifiButtonBounds(), TouchAction::AdvancedClearWifi);
      addTouchZone(advancedResetSettingsButtonBounds(), TouchAction::AdvancedResetSettings);
      addTouchZone(advancedClearWeatherButtonBounds(), TouchAction::AdvancedClearWeatherCache);
      addTouchZone(advancedFormatSystemButtonBounds(), TouchAction::AdvancedFormatSystem);
    }
  } else if (activePage_ == Page::MapDownload) {
    const uint8_t targetCount = mapDownloadManager_ ? mapDownloadManager_->targetCount() : 0;
    if (targetCount > 0) addTouchZone(mapRegionButtonBounds(0), TouchAction::MapDownloadSelect0);
    if (targetCount > 1) addTouchZone(mapRegionButtonBounds(1), TouchAction::MapDownloadSelect1);
    if (targetCount > 2) addTouchZone(mapRegionButtonBounds(2), TouchAction::MapDownloadSelect2);
    if (targetCount > 3) addTouchZone(mapRegionButtonBounds(3), TouchAction::MapDownloadSelect3);
    if (targetCount > 4) addTouchZone(mapRegionButtonBounds(4), TouchAction::MapDownloadSelect4);
    if (targetCount > 5) addTouchZone(mapRegionButtonBounds(5), TouchAction::MapDownloadSelect5);
    if (mapDownloadManager_ && mapDownloadManager_->busy()) {
      addTouchZone(mapDownloadCancelButtonBounds(), TouchAction::MapDownloadCancel);
    } else {
      addTouchZone(mapDownloadStartButtonBounds(), TouchAction::MapDownloadStart);
      addTouchZone(mapDownloadCancelButtonBounds(), mapDownloadManager_ && mapDownloadManager_->canGoBack() ? TouchAction::MapDownloadBack
                                                                                                            : TouchAction::OpenWifiSettings);
    }
  } else if (activePage_ == Page::Map) {
    if (mapPage_.showMapControls()) {
      if (mapPage_.canZoomIn()) {
        addTouchZone(mapZoomInButtonBounds(), TouchAction::MapZoomIn);
      }
      if (mapPage_.canZoomOut()) {
        addTouchZone(mapZoomOutButtonBounds(), TouchAction::MapZoomOut);
      }
      addTouchZone(mapPanUpButtonBounds(), TouchAction::MapPanUp);
      addTouchZone(mapPanDownButtonBounds(), TouchAction::MapPanDown);
      addTouchZone(mapPanLeftButtonBounds(), TouchAction::MapPanLeft);
      addTouchZone(mapPanRightButtonBounds(), TouchAction::MapPanRight);
    }
  }
}

bool MainScreen::handleAudioEditorTouch(int32_t x, int32_t y) {
  if (!audioBuzzer_) return false;

  const Rect_t touchBounds = audioVolumeSliderTouchBounds();
  if (!pointInRect(touchBounds, x, y)) {
    return false;
  }

  const Rect_t slider = audioVolumeSliderBounds();
  int32_t clampedX = x;
  if (clampedX < slider.x) clampedX = slider.x;
  if (clampedX > slider.x + slider.width) clampedX = slider.x + slider.width;

  uint8_t volume = 0;
  if (slider.width > 0) {
    const int32_t relativeX = clampedX - slider.x;
    volume = static_cast<uint8_t>((relativeX * 100L + slider.width / 2) / slider.width);
    if (volume > VarioBuzzer::kMaxBeepVolumePercent) {
      volume = VarioBuzzer::kMaxBeepVolumePercent;
    }
  }

  audioBuzzer_->setBeepVolume(volume);
  audioProfileDirty_ = true;
  audioSavedNotice_ = false;
  audioBuzzer_->playVolumePreview();
  lastTouchAction_ = TouchAction::AudioVolumeSet;
  refreshAudioEditorControls();
  return true;
}

bool MainScreen::handleDashboardLayoutTouch(int32_t x, int32_t y) {
  if (pointInRect(dashboardLayoutResetButtonBounds(), x, y)) {
    dashboardLayout_.resetDefault();
    dashboardLayoutSavedNotice_ = dashboardLayout_.save();
    dashboardLayoutDragActive_ = false;
    lastTouchAction_ = TouchAction::DashboardLayoutReset;
    refreshActivePage();
    return true;
  }

  DashboardSlot slot = DashboardSlot::Center;
  if (dashboardSlotAt(x, y, slot)) {
    if (!dashboardLayoutDragActive_) {
      dashboardLayoutDragWidget_ = dashboardLayout_.widgetForSlot(slot);
      dashboardLayoutDragActive_ = true;
      dashboardLayoutSavedNotice_ = false;
      lastTouchAction_ = TouchAction::DashboardLayoutMove;
      refreshActivePage();
      return true;
    }

    if (dashboardLayout_.widgetForSlot(slot) != dashboardLayoutDragWidget_) {
      dashboardLayout_.moveWidgetToSlot(dashboardLayoutDragWidget_, slot);
      dashboardLayoutSavedNotice_ = dashboardLayout_.save();
      dashboardLayoutDragActive_ = false;
      lastTouchAction_ = TouchAction::DashboardLayoutMove;
      refreshActivePage();
      return true;
    }
    return true;
  }

  if (dashboardLayoutDragActive_) {
    dashboardLayoutDragActive_ = false;
    dashboardLayoutSavedNotice_ = false;
    lastTouchAction_ = TouchAction::DashboardLayoutMove;
    refreshActivePage();
    return true;
  }

  lastTouchAction_ = TouchAction::None;
  return false;
}

bool MainScreen::handleFirmwareUpdateTouch(int32_t x, int32_t y) {
  if (!firmwareUpdater_) {
    return false;
  }

  const bool wifiConnected = wifiManager_ && wifiManager_->isConnected();
  if (!wifiConnected && pointInRect(firmwareUpdateWifiButtonBounds(), x, y)) {
    lastTouchAction_ = TouchAction::OpenWifiSettings;
    if (wifiManager_ && wifiSelectedSsid_.length() == 0) {
      wifiSelectedSsid_ = wifiManager_->currentSsid();
    }
    openPage(Page::WifiSettings);
    return true;
  }

  if (wifiConnected && pointInRect(firmwareUpdateStartButtonBounds(), x, y)) {
    lastTouchAction_ = TouchAction::OpenFirmwareUpdate;
    renderFirmwareUpdatePage();
    display_.saveBaseBuffer();
    refreshPageTransition();
    const bool updated = firmwareUpdater_->beginUpdate();
    renderFirmwareUpdatePage();
    display_.saveBaseBuffer();
    refreshPageTransition();
    if (updated) {
      delay(1200);
      ESP.restart();
    }
    return true;
  }

  lastTouchAction_ = TouchAction::None;
  return false;
}

bool MainScreen::handleWifiTouch(int32_t x, int32_t y) {
  if (!wifiManager_) {
    return false;
  }

  if (pointInRect(wifiScanButtonBounds(), x, y)) {
    lastTouchAction_ = TouchAction::OpenWifiSettings;
    if (!wifiManager_->isEnabled()) {
      wifiManager_->enableRuntime(false);
    }
    wifiManager_->scanNetworks();
    wifiLastRefreshMs_ = millis();
    refreshWifiStatusArea();
    return true;
  }

  if (pointInRect(wifiConnectButtonBounds(), x, y)) {
    lastTouchAction_ = TouchAction::OpenWifiSettings;
    if (!wifiManager_->isEnabled()) {
      wifiManager_->enableRuntime(false);
    }
    wifiManager_->connectTo(wifiSelectedSsid_, wifiPassword_);
    wifiLastRefreshMs_ = millis();
    refreshWifiStatusArea();
    return true;
  }

  if (pointInRect(wifiClearButtonBounds(), x, y)) {
    lastTouchAction_ = TouchAction::OpenWifiSettings;
    wifiSelectedSsid_ = "";
    wifiPassword_ = "";
    refreshWifiStatusArea();
    return true;
  }

  const uint8_t availableCount = wifiManager_->networkCount();
  const uint8_t count = availableCount > 3 ? 3 : availableCount;
  for (uint8_t i = 0; i < count; ++i) {
    if (pointInRect(wifiNetworkRowBounds(i), x, y)) {
      lastTouchAction_ = TouchAction::OpenWifiSettings;
      wifiSelectedSsid_ = wifiManager_->networkSsid(i);
      wifiPassword_ = wifiManager_->savedPasswordFor(wifiSelectedSsid_);
      refreshWifiStatusArea();
      return true;
    }
  }

  for (uint8_t row = 0; row < 4; ++row) {
    const uint8_t cols = row == 2 ? 9 : 10;
    for (uint8_t col = 0; col < cols; ++col) {
      if (pointInRect(wifiKeyBounds(row, col), x, y)) {
        const char key = wifiKeyAt(row, col);
        if (key != '\0' && wifiPassword_.length() < 32) {
          wifiPassword_ += key;
          refreshWifiPasswordValue();
        }
        lastTouchAction_ = TouchAction::OpenWifiSettings;
        return true;
      }
    }
  }

  if (pointInRect(wifiSpecialKeyBounds(0), x, y)) {
    wifiKeyboardMode_ = wifiKeyboardMode_ == 1 ? 0 : 1;
    lastTouchAction_ = TouchAction::OpenWifiSettings;
    refreshWifiKeyboardArea();
    return true;
  }
  if (pointInRect(wifiSpecialKeyBounds(1), x, y)) {
    wifiKeyboardMode_ = wifiKeyboardMode_ == 2 ? 0 : 2;
    lastTouchAction_ = TouchAction::OpenWifiSettings;
    refreshWifiKeyboardArea();
    return true;
  }
  if (pointInRect(wifiSpecialKeyBounds(2), x, y)) {
    const uint32_t now = millis();
    const bool acceptSpace = keyboardSpaceLastMs_ == 0 || now - keyboardSpaceLastMs_ >= kKeyboardSpaceRepeatGuardMs;
    keyboardSpaceLastMs_ = now;
    if (acceptSpace && wifiPassword_.length() < 32) {
      wifiPassword_ += ' ';
      refreshWifiPasswordValue();
    }
    lastTouchAction_ = TouchAction::OpenWifiSettings;
    return true;
  }
  if (pointInRect(wifiSpecialKeyBounds(3), x, y)) {
    if (wifiPassword_.length() > 0) {
      wifiPassword_.remove(wifiPassword_.length() - 1);
      refreshWifiPasswordValue();
    }
    lastTouchAction_ = TouchAction::OpenWifiSettings;
    return true;
  }

  lastTouchAction_ = TouchAction::None;
  return false;
}

bool MainScreen::handlePilotProfileTouch(int32_t x, int32_t y) {
  if (!pilotProfile_) {
    return false;
  }

  for (uint8_t i = 0; i < static_cast<uint8_t>(PilotProfileConfig::Field::Count); ++i) {
    if (pointInRect(profileFieldBounds(i), x, y)) {
      if (profileSelectedField_ != i) {
        const uint8_t previousField = profileSelectedField_;
        profileSelectedField_ = i;
        profileSavedNotice_ = false;
        refreshPilotProfileSelection(previousField, profileSelectedField_);
      }
      lastTouchAction_ = TouchAction::OpenPilotProfile;
      return true;
    }
  }

  const PilotProfileConfig::Field field = static_cast<PilotProfileConfig::Field>(profileSelectedField_);
  for (uint8_t row = 0; row < 4; ++row) {
    const uint8_t cols = row == 2 ? 9 : 10;
    for (uint8_t col = 0; col < cols; ++col) {
      if (pointInRect(profileKeyBounds(row, col), x, y)) {
        const char key = profileKeyAt(row, col);
        if (key != '\0' && pilotProfile_->appendChar(field, key)) {
          profileDirty_ = true;
          profileSavedNotice_ = false;
          refreshPilotProfileFieldValue(profileSelectedField_);
        }
        lastTouchAction_ = TouchAction::OpenPilotProfile;
        return true;
      }
    }
  }

  if (pointInRect(profileSpecialKeyBounds(0), x, y)) {
    profileKeyboardMode_ = profileKeyboardMode_ == 1 ? 0 : 1;
    profileSavedNotice_ = false;
    lastTouchAction_ = TouchAction::OpenPilotProfile;
    refreshPilotProfileKeyboardArea();
    return true;
  }
  if (pointInRect(profileSpecialKeyBounds(1), x, y)) {
    profileKeyboardMode_ = profileKeyboardMode_ == 2 ? 1 : 2;
    profileSavedNotice_ = false;
    lastTouchAction_ = TouchAction::OpenPilotProfile;
    refreshPilotProfileKeyboardArea();
    return true;
  }
  if (pointInRect(profileSpecialKeyBounds(2), x, y)) {
    const uint32_t now = millis();
    const bool acceptSpace = keyboardSpaceLastMs_ == 0 || now - keyboardSpaceLastMs_ >= kKeyboardSpaceRepeatGuardMs;
    keyboardSpaceLastMs_ = now;
    if (acceptSpace && pilotProfile_->appendChar(field, ' ')) {
      profileDirty_ = true;
      profileSavedNotice_ = false;
      refreshPilotProfileFieldValue(profileSelectedField_);
    }
    lastTouchAction_ = TouchAction::OpenPilotProfile;
    return true;
  }
  if (pointInRect(profileSpecialKeyBounds(3), x, y)) {
    if (pilotProfile_->backspace(field)) {
      profileDirty_ = true;
      profileSavedNotice_ = false;
      refreshPilotProfileFieldValue(profileSelectedField_);
    }
    lastTouchAction_ = TouchAction::OpenPilotProfile;
    return true;
  }

  lastTouchAction_ = TouchAction::None;
  return false;
}

bool MainScreen::handleTracklogBleStatusTouch(int32_t x, int32_t y) {
  if (!tracklogBle_ || !tracklogBle_->visibleStatus()) {
    return false;
  }

  if (tracklogBle_->canCancelPending() && pointInRect(tracklogBleCancelButtonBounds(), x, y)) {
    tracklogBle_->cancelPending();
    snprintf(tracklogNotice_, sizeof(tracklogNotice_), "OPERAÇAO CANCELADA");
    lastTouchAction_ = TouchAction::TracklogBleCancel;
    refreshActivePage();
    return true;
  }

  if (pointInRect(tracklogBleStatusPopupBounds(), x, y)) {
    lastTouchAction_ = TouchAction::ToggleTracklog;
    return true;
  }

  return false;
}

bool MainScreen::handleTracklogTouch(int32_t x, int32_t y) {
  if (!flightRecorder_) {
    return false;
  }

  if (handleTracklogBleStatusTouch(x, y)) {
    return true;
  }

  const uint8_t pageSize = 5;
  FlightRecorder::TracklogEntry pageEntries[pageSize] = {};
  uint8_t entryCount = 0;
  uint16_t total = 0;
  uint32_t usedBytes = 0;
  flightRecorder_->tracklogPage(tracklogPage_, pageSize, pageEntries, entryCount, total, usedBytes);
  const uint16_t maxPage = total == 0 ? 0 : static_cast<uint16_t>((total - 1) / pageSize);

  if (tracklogDeleteConfirm_) {
    if (pointInRect(tracklogConfirmYesButtonBounds(), x, y)) {
      tracklogDeleteConfirm_ = false;
      if (selectedTracklogPath_[0] != '\0' && flightRecorder_->deleteTracklog(selectedTracklogPath_)) {
        snprintf(tracklogNotice_, sizeof(tracklogNotice_), "ARQUIVO APAGADO");
        selectedTracklogPath_[0] = '\0';
      } else {
        snprintf(tracklogNotice_, sizeof(tracklogNotice_), "NAO FOI POSSIVEL APAGAR");
      }
      lastTouchAction_ = TouchAction::ToggleTracklog;
      refreshActivePage();
      return true;
    }

    if (pointInRect(tracklogConfirmNoButtonBounds(), x, y)) {
      tracklogDeleteConfirm_ = false;
      selectedTracklogPath_[0] = '\0';
      snprintf(tracklogNotice_, sizeof(tracklogNotice_), "EXCLUSAO CANCELADA");
      lastTouchAction_ = TouchAction::ToggleTracklog;
      refreshActivePage();
      return true;
    }

    lastTouchAction_ = TouchAction::ToggleTracklog;
    return true;
  }

  for (uint8_t i = 0; i < entryCount; ++i) {
    if (pointInRect(tracklogRowDeleteButtonBounds(i), x, y)) {
      snprintf(selectedTracklogPath_, sizeof(selectedTracklogPath_), "%s", pageEntries[i].path);
      tracklogDeleteConfirm_ = true;
      tracklogNotice_[0] = '\0';
      lastTouchAction_ = TouchAction::ToggleTracklog;
      refreshActivePage();
      return true;
    }

    if (pointInRect(tracklogRowBounds(i), x, y)) {
      tracklogNotice_[0] = '\0';
      openTracklogDetails(pageEntries[i].path);
      lastTouchAction_ = TouchAction::ToggleTracklog;
      return true;
    }
  }

  if (pointInRect(tracklogPrevButtonBounds(), x, y)) {
    if (tracklogPage_ > 0) {
      --tracklogPage_;
    }
    lastTouchAction_ = TouchAction::ToggleTracklog;
    refreshActivePage();
    return true;
  }

  if (pointInRect(tracklogSyncButtonBounds(), x, y)) {
    if (!kTracklogBleTransferEnabled || !tracklogBle_) {
      snprintf(tracklogNotice_, sizeof(tracklogNotice_), "BLE DESATIVADO PARA RECUPERAÇAO");
    } else if (total == 0) {
      snprintf(tracklogNotice_, sizeof(tracklogNotice_), "NENHUM VOO PARA SINCRONIZAR");
    } else {
      pauseWifiBeforeBle(wifiManager_);
      if (!tracklogBle_->prepareSyncAll()) {
        snprintf(tracklogNotice_, sizeof(tracklogNotice_), tracklogBle_->activeTransfer() ? "BLE OCUPADO" : "FALHA AO PREPARAR SYNC");
      } else if (tracklogBle_->connected()) {
        snprintf(tracklogNotice_, sizeof(tracklogNotice_), "SINCRONIZANDO VIA BLE");
      } else {
        snprintf(tracklogNotice_, sizeof(tracklogNotice_), "ABRA O APP BRVARIO");
      }
    }
    lastTouchAction_ = TouchAction::ToggleTracklog;
    refreshActivePage();
    return true;
  }

  if (pointInRect(tracklogNextButtonBounds(), x, y)) {
    if (tracklogPage_ < maxPage) {
      ++tracklogPage_;
    }
    lastTouchAction_ = TouchAction::ToggleTracklog;
    refreshActivePage();
    return true;
  }

  lastTouchAction_ = TouchAction::None;
  return false;
}

bool MainScreen::handleTracklogDetailsTouch(int32_t x, int32_t y) {
  if (!flightRecorder_) {
    return false;
  }

  if (handleTracklogBleStatusTouch(x, y)) {
    return true;
  }

  if (tracklogDeleteConfirm_) {
    if (pointInRect(tracklogConfirmYesButtonBounds(), x, y)) {
      tracklogDeleteConfirm_ = false;
      if (selectedTracklogPath_[0] != '\0' && flightRecorder_->deleteTracklog(selectedTracklogPath_)) {
        snprintf(tracklogNotice_, sizeof(tracklogNotice_), "ARQUIVO APAGADO");
        selectedTracklogPath_[0] = '\0';
        activePage_ = Page::Tracklog;
        refreshActivePage();
      } else {
        snprintf(tracklogNotice_, sizeof(tracklogNotice_), "NAO FOI POSSIVEL APAGAR");
        refreshActivePage();
      }
      lastTouchAction_ = TouchAction::ToggleTracklog;
      return true;
    }

    if (pointInRect(tracklogConfirmNoButtonBounds(), x, y)) {
      tracklogDeleteConfirm_ = false;
      snprintf(tracklogNotice_, sizeof(tracklogNotice_), "EXCLUSAO CANCELADA");
      lastTouchAction_ = TouchAction::ToggleTracklog;
      refreshActivePage();
      return true;
    }

    lastTouchAction_ = TouchAction::ToggleTracklog;
    return true;
  }

  if (pointInRect(tracklogDeleteButtonBounds(), x, y)) {
    tracklogDeleteConfirm_ = true;
    tracklogNotice_[0] = '\0';
    lastTouchAction_ = TouchAction::ToggleTracklog;
    refreshActivePage();
    return true;
  }

  if (pointInRect(tracklogExportButtonBounds(), x, y)) {
    if (!kTracklogBleTransferEnabled || !tracklogBle_) {
      snprintf(tracklogNotice_, sizeof(tracklogNotice_), "BLE DESATIVADO PARA RECUPERAÇAO");
    } else if (selectedTracklogPath_[0] == '\0') {
      snprintf(tracklogNotice_, sizeof(tracklogNotice_), "NENHUM ARQUIVO SELECIONADO");
    } else {
      pauseWifiBeforeBle(wifiManager_);
      if (!tracklogBle_->prepareTracklog(selectedTracklogPath_)) {
        snprintf(tracklogNotice_, sizeof(tracklogNotice_), tracklogBle_->activeTransfer() ? "BLE OCUPADO" : "FALHA AO PREPARAR BLE");
      } else if (tracklogBle_->connected()) {
        snprintf(tracklogNotice_, sizeof(tracklogNotice_), "ENVIANDO VIA BLE");
      } else {
        snprintf(tracklogNotice_, sizeof(tracklogNotice_), "ABRA O APP BRVARIO");
      }
    }
    lastTouchAction_ = TouchAction::ToggleTracklog;
    refreshActivePage();
    return true;
  }

  lastTouchAction_ = TouchAction::None;
  return false;
}

void MainScreen::setAdvancedNotice(const char* text) {
  snprintf(advancedNotice_, sizeof(advancedNotice_), "%s", text ? text : "");
}

void MainScreen::requestAdvancedConfirmation(TouchAction action, const char* notice) {
  advancedConfirmAction_ = action;
  setAdvancedNotice(notice);
  refreshActivePage();
}

bool MainScreen::resetSavedRuntimeSettings(bool includeWifi) {
  bool ok = true;
  ok = clearPrefsNamespace("brvario") && ok;
  ok = clearPrefsNamespace("dashLayout") && ok;
  ok = clearPrefsNamespace("thermalCfg") && ok;
  ok = clearPrefsNamespace("pilot") && ok;
  ok = clearPrefsNamespace("varioAudio") && ok;
  ok = clearPrefsNamespace("weatherWind") && ok;
  ok = clearPrefsNamespace("weatherLoc") && ok;
  if (includeWifi && wifiManager_) {
    ok = wifiManager_->clearCredentials() && ok;
    wifiSelectedSsid_ = "";
    wifiPassword_ = "";
  }

  dashboardLayout_.resetDefault();
  ok = dashboardLayout_.save() && ok;
  configureDashboardWidgetBounds();
  if (thermalAssistConfig_) {
    thermalAssistConfig_->resetDefault();
    ok = thermalAssistConfig_->save() && ok;
  }
  if (pilotProfile_) {
    pilotProfile_->resetDefault();
    ok = pilotProfile_->save() && ok;
  }
  if (audioBuzzer_) {
    audioBuzzer_->resetDefaultProfile();
    ok = audioBuzzer_->saveProfile() && ok;
  }
  return ok;
}

void MainScreen::executeAdvancedAction(TouchAction action) {
  advancedConfirmAction_ = TouchAction::None;

  switch (action) {
    case TouchAction::AdvancedRecoverDisplay: {
      setAdvancedNotice("RECUPERANDO TELA...");
      renderAdvancedSystemPage();
      display_.saveBaseBuffer();
      display_.fullRefresh();
      display_.recoverPanel();
      setAdvancedNotice("RECUPERAÇAO CONCLUIDA");
      renderAdvancedSystemPage();
      display_.saveBaseBuffer();
      display_.fullRefresh();
      registerPageTouchZones();
      break;
    }
    case TouchAction::AdvancedMoveIgcToSd: {
      if (flightRecorder_ && flightRecorder_->recording()) {
        setAdvancedNotice("BLOQUEADO: VOO EM GRAVAÇAO");
        refreshActivePage();
        break;
      }
      if (tracklogBle_ && tracklogBle_->activeTransfer()) {
        setAdvancedNotice("BLOQUEADO: TRANSFERENCIA BLE ATIVA");
        refreshActivePage();
        break;
      }
      if (!flightRecorder_ || !storageManager_) {
        setAdvancedNotice("GERENCIADOR INDISPONIVEL");
        refreshActivePage();
        break;
      }
      if (!storageManager_->refresh()) {
        setAdvancedNotice("CARTAO SD NAO MONTADO");
        refreshActivePage();
        break;
      }
      fs::FS* sdFs = storageManager_->filesystem();
      if (!sdFs) {
        setAdvancedNotice("CARTAO SD INDISPONIVEL");
        refreshActivePage();
        break;
      }
      uint16_t moved = 0;
      uint16_t failed = 0;
      const bool ok = flightRecorder_->moveTracklogsTo(*sdFs, kAdvancedIgcSdDir, moved, failed);
      storageManager_->refresh();
      flightRecorder_->attachArchiveStorage(storageManager_->filesystem(), kAdvancedIgcSdDir);
      if (moved == 0 && failed == 0) {
        setAdvancedNotice("NENHUM IGC INTERNO PARA MOVER");
      } else if (ok) {
        char text[80];
        snprintf(text, sizeof(text), "IGC MOVIDOS PARA SD: %u", static_cast<unsigned>(moved));
        setAdvancedNotice(text);
      } else {
        char text[80];
        snprintf(text, sizeof(text), "MOVIDOS %u  FALHAS %u", static_cast<unsigned>(moved), static_cast<unsigned>(failed));
        setAdvancedNotice(text);
      }
      refreshActivePage();
      break;
    }
    case TouchAction::AdvancedClearWifi:
      if (wifiManager_ && wifiManager_->clearCredentials()) {
        wifiSelectedSsid_ = "";
        wifiPassword_ = "";
        setAdvancedNotice("WIFI LIMPO");
      } else {
        setAdvancedNotice("FALHA AO LIMPAR WIFI");
      }
      refreshActivePage();
      break;
    case TouchAction::AdvancedResetSettings:
      setAdvancedNotice(resetSavedRuntimeSettings(false) ? "CONFIGURAÇOES PADRAO RESTAURADAS" : "PADRAO PARCIAL: REVISE NVS");
      refreshActivePage();
      break;
    case TouchAction::AdvancedClearWeatherCache:
      setAdvancedNotice(clearPrefsNamespace("weatherWind") ? "CACHE METEO LIMPO" : "FALHA AO LIMPAR METEO");
      refreshActivePage();
      break;
    case TouchAction::AdvancedFormatSystem: {
      if (flightRecorder_ && flightRecorder_->recording()) {
        setAdvancedNotice("BLOQUEADO: VOO EM GRAVAÇAO");
        refreshActivePage();
        break;
      }
      if (tracklogBle_ && tracklogBle_->activeTransfer()) {
        setAdvancedNotice("BLOQUEADO: TRANSFERENCIA BLE ATIVA");
        refreshActivePage();
        break;
      }
      if (tracklogBle_) {
        tracklogBle_->end();
      }
      resetSavedRuntimeSettings(true);
      LittleFS.end();
      const bool formatted = LittleFS.format();
      const bool mounted = formatted && LittleFS.begin(false);
      if (mounted && flightRecorder_ && pilotProfile_) {
        flightRecorder_->begin(LittleFS, *pilotProfile_);
      }
      selectedTracklogPath_[0] = '\0';
      tracklogNotice_[0] = '\0';
      tracklogDeleteConfirm_ = false;
      setAdvancedNotice(formatted && mounted ? "FORMATAÇAO GERAL OK - IGC INTERNO APAGADO" : "FALHA AO FORMATAR MEMORIA INTERNA");
      refreshActivePage();
      break;
    }
    default:
      refreshActivePage();
      break;
  }
}

void MainScreen::addTouchZone(const Rect_t& bounds, TouchAction action) {
  if (touchZoneCount_ >= kMaxTouchZones) return;
  touchZones_[touchZoneCount_++] = {bounds, action, true};
}

bool MainScreen::dispatchTouchAction(TouchAction action) {
  lastTouchAction_ = action;
  powerPressStartedMs_ = 0;
  powerLastTouchMs_ = 0;
  switch (action) {
    case TouchAction::Home:
      tracklogNotice_[0] = '\0';
      tracklogDeleteConfirm_ = false;
      storageClearConfirm_ = false;
      advancedConfirmAction_ = TouchAction::None;
      advancedNotice_[0] = '\0';
      openDashboard();
      break;
    case TouchAction::Back:
      if (activePage_ == Page::TracklogDetails) {
        tracklogNotice_[0] = '\0';
        tracklogDeleteConfirm_ = false;
        openPage(Page::Tracklog);
      } else if (activePage_ == Page::ManualLogic || activePage_ == Page::ManualUser) {
        manualScrollOffset_ = 0;
        openPage(Page::Manual);
      } else if (activePage_ == Page::WeatherLocation) {
        if (weatherInfoPopupVisible_) {
          weatherInfoPopupVisible_ = false;
          refreshActivePage();
        } else if (weatherLocationView_ == WeatherLocationView::CatalogSearch ||
                   weatherLocationView_ == WeatherLocationView::CatalogStates) {
          beginWeatherCatalogMatchRebuild();
          refreshActivePage();
        } else if (weatherLocationView_ == WeatherLocationView::CatalogLoading) {
          weatherLocationView_ = WeatherLocationView::Menu;
          weatherCatalogFilterDirty_ = true;
          weatherLocationNotice_[0] = '\0';
          refreshActivePage();
        } else if (weatherLocationView_ != WeatherLocationView::Menu) {
          weatherLocationView_ = WeatherLocationView::Menu;
          weatherLocationNotice_[0] = '\0';
          refreshActivePage();
        } else if (weatherLocationSelectionRequired_) {
          weatherLocationSelectionRequired_ = false;
          openPage(Page::Settings);
        } else {
          openPage(Page::WeatherStation);
        }
      } else if (activePage_ == Page::ThermalCycleBeta && thermalCycleInfoPopupVisible_) {
        thermalCycleInfoPopupVisible_ = false;
        refreshActivePage();
      } else if (activePage_ == Page::AudioEditor || activePage_ == Page::DashboardLayout || activePage_ == Page::WifiSettings ||
          activePage_ == Page::FirmwareUpdate || activePage_ == Page::WeatherStation || activePage_ == Page::PilotProfile ||
          activePage_ == Page::ThermalCycleBeta || activePage_ == Page::ThermalAssistSettings || activePage_ == Page::DeviceInfo ||
          activePage_ == Page::SystemStatus || activePage_ == Page::Manual || activePage_ == Page::Storage ||
          activePage_ == Page::AdvancedSystem || activePage_ == Page::MapDownload) {
        storageClearConfirm_ = false;
        advancedConfirmAction_ = TouchAction::None;
        advancedNotice_[0] = '\0';
        if (activePage_ == Page::MapDownload) {
          openPage(Page::Storage);
        } else {
          openPage(Page::Settings);
        }
      } else {
        openDashboard();
      }
      break;
    case TouchAction::OpenSettings:
      openPage(Page::Settings);
      break;
    case TouchAction::OpenAudioEditor:
      audioSavedNotice_ = false;
      openPage(Page::AudioEditor);
      break;
    case TouchAction::OpenWifiSettings:
      if (wifiManager_ && wifiSelectedSsid_.length() == 0) {
        wifiSelectedSsid_ = wifiManager_->currentSsid();
      }
      wifiLastRefreshMs_ = 0;
      wifiLastState_ = 255;
      openPage(Page::WifiSettings);
      break;
    case TouchAction::OpenFirmwareUpdate:
      if (firmwareUpdater_) {
        firmwareUpdater_->resetStatus();
      }
      openPage(Page::FirmwareUpdate);
      break;
    case TouchAction::OpenWeatherStation:
      weatherInfoPopupVisible_ = false;
      weatherLocationSelectionRequired_ = true;
      weatherLocationView_ = WeatherLocationView::Menu;
      weatherLocationNotice_[0] = '\0';
      openPage(Page::WeatherLocation);
      break;
    case TouchAction::OpenWeatherLocation:
      weatherInfoPopupVisible_ = false;
      weatherLocationSelectionRequired_ = false;
      weatherLocationView_ = WeatherLocationView::Menu;
      weatherLocationNotice_[0] = '\0';
      openPage(Page::WeatherLocation);
      break;
    case TouchAction::OpenPilotProfile:
      profileSavedNotice_ = false;
      openPage(Page::PilotProfile);
      break;
    case TouchAction::OpenThermalCycleBeta:
      openPage(Page::ThermalCycleBeta);
      break;
    case TouchAction::OpenDashboardLayout:
      dashboardLayoutSavedNotice_ = false;
      dashboardLayoutDragActive_ = false;
      openPage(Page::DashboardLayout);
      break;
    case TouchAction::OpenThermalAssistSettings:
      openPage(Page::ThermalAssistSettings);
      break;
    case TouchAction::OpenDeviceInfo:
      openPage(Page::DeviceInfo);
      break;
    case TouchAction::OpenSystemStatus:
      openPage(Page::SystemStatus);
      break;
    case TouchAction::OpenManual:
      manualPage_ = 0;
      manualScrollOffset_ = 0;
      openPage(Page::Manual);
      break;
    case TouchAction::OpenManualLogic:
      manualPage_ = 0;
      manualScrollOffset_ = 0;
      openPage(Page::ManualLogic);
      break;
    case TouchAction::OpenManualUser:
      manualPage_ = 0;
      manualScrollOffset_ = 0;
      openPage(Page::ManualUser);
      break;
    case TouchAction::OpenStorageManager:
      storageClearConfirm_ = false;
      if (storageManager_) {
        storageManager_->refresh();
      }
      openPage(Page::Storage);
      break;
    case TouchAction::OpenAdvancedSystem:
      advancedConfirmAction_ = TouchAction::None;
      advancedNotice_[0] = '\0';
      if (storageManager_) {
        storageManager_->refresh();
      }
      openPage(Page::AdvancedSystem);
      break;
    case TouchAction::OpenMapDownload:
      if (mapDownloadManager_) {
        mapDownloadManager_->resetStatus();
      }
      openPage(Page::MapDownload);
      break;
    case TouchAction::NextPage:
      openPage(Page::Map);
      break;
    case TouchAction::ToggleTracklog:
      tracklogPage_ = 0;
      tracklogNotice_[0] = '\0';
      tracklogDeleteConfirm_ = false;
      if (storageManager_) {
        storageManager_->refresh();
      }
      if (flightRecorder_) {
        flightRecorder_->attachArchiveStorage(storageManager_ ? storageManager_->filesystem() : nullptr, kAdvancedIgcSdDir);
      }
      openPage(Page::Tracklog);
      break;
    case TouchAction::ToggleAudio:
      lastData_.audioEnabled = !lastData_.audioEnabled;
      if (activePage_ == Page::Dashboard) {
        renderFooterDynamic(lastData_);
        Rect_t area = footerDynamicBounds();
        display_.updateAreas(&area, 1);
        markFooterDisplayed(Page::Dashboard);
      } else if (activePage_ == Page::Map) {
        refreshMapPage(true);
        registerPageTouchZones();
      } else {
        renderPage(activePage_);
        display_.saveBaseBuffer();
        refreshPageTransition();
        registerPageTouchZones();
      }
      break;
    case TouchAction::PowerHold:
      break;
    case TouchAction::PowerRequest: {
      const Rect_t area = powerConfirmPopupBounds();
      powerConfirmVisible_ = true;
      refreshDashboardOverlayArea(area);
      break;
    }
    case TouchAction::PowerConfirmYes:
      powerConfirmVisible_ = false;
      lastTouchAction_ = TouchAction::PowerOff;
      break;
    case TouchAction::PowerConfirmNo: {
      const Rect_t area = powerConfirmPopupBounds();
      powerConfirmVisible_ = false;
      refreshDashboardOverlayArea(area);
      break;
    }
    case TouchAction::AudioResponseDown:
      if (audioBuzzer_) {
        audioBuzzer_->adjustSimpleLevels(-1, 0);
        audioProfileDirty_ = true;
        audioSavedNotice_ = false;
      } else {
        audioSavedNotice_ = false;
      }
      refreshAudioEditorControls();
      break;
    case TouchAction::AudioResponseUp:
      if (audioBuzzer_) {
        audioBuzzer_->adjustSimpleLevels(1, 0);
        audioProfileDirty_ = true;
        audioSavedNotice_ = false;
      } else {
        audioSavedNotice_ = false;
      }
      refreshAudioEditorControls();
      break;
    case TouchAction::AudioPitchDown:
      if (audioBuzzer_) {
        audioBuzzer_->adjustSimpleLevels(0, -1);
        audioProfileDirty_ = true;
        audioSavedNotice_ = false;
        audioBuzzer_->playPitchPreview();
      } else {
        audioSavedNotice_ = false;
      }
      refreshAudioEditorControls();
      break;
    case TouchAction::AudioPitchUp:
      if (audioBuzzer_) {
        audioBuzzer_->adjustSimpleLevels(0, 1);
        audioProfileDirty_ = true;
        audioSavedNotice_ = false;
        audioBuzzer_->playPitchPreview();
      } else {
        audioSavedNotice_ = false;
      }
      refreshAudioEditorControls();
      break;
    case TouchAction::AudioVoiceToggle:
      if (audioBuzzer_) {
        audioBuzzer_->toggleVoiceEnabled();
        audioProfileDirty_ = true;
        audioSavedNotice_ = false;
        audioBuzzer_->playGpsConnectedSound();
      } else {
        audioSavedNotice_ = false;
      }
      refreshAudioEditorControls();
      break;
    case TouchAction::AudioReset:
      if (audioBuzzer_) {
        audioBuzzer_->resetDefaultProfile();
        audioProfileDirty_ = true;
        audioSavedNotice_ = false;
      } else {
        audioSavedNotice_ = false;
      }
      refreshAudioEditorControls();
      break;
    case TouchAction::ThermalModePilot:
      if (thermalAssistConfig_) {
        thermalAssistConfig_->setVisualMode(ThermalAssistVisualMode::PilotCentered);
        thermalAssistConfig_->save();
      }
      refreshActivePage();
      break;
    case TouchAction::ThermalModeThermal:
      if (thermalAssistConfig_) {
        thermalAssistConfig_->setVisualMode(ThermalAssistVisualMode::ThermalCentered);
        thermalAssistConfig_->save();
      }
      refreshActivePage();
      break;
    case TouchAction::ThermalInfo:
      if (activePage_ == Page::ThermalAssistSettings) {
        thermalInfoPopupVisible_ = !thermalInfoPopupVisible_;
        if (thermalInfoPopupVisible_) {
          thermalInfoScrollPage_ = 0;
        }
        refreshActivePage();
      }
      break;
    case TouchAction::ThermalInfoUp:
      if (activePage_ == Page::ThermalAssistSettings && thermalInfoPopupVisible_ && thermalInfoScrollPage_ > 0) {
        --thermalInfoScrollPage_;
        refreshActivePage();
      }
      break;
    case TouchAction::ThermalInfoDown:
      if (activePage_ == Page::ThermalAssistSettings && thermalInfoPopupVisible_ && thermalInfoScrollPage_ + 1 < kThermalInfoPageCount) {
        ++thermalInfoScrollPage_;
        refreshActivePage();
      }
      break;
    case TouchAction::ThermalCycleInfo:
      if (activePage_ == Page::ThermalCycleBeta) {
        thermalCycleInfoPopupVisible_ = !thermalCycleInfoPopupVisible_;
        if (thermalCycleInfoPopupVisible_) {
          thermalCycleInfoScrollPage_ = 0;
        }
        refreshActivePage();
      }
      break;
    case TouchAction::ThermalCycleInfoUp:
      if (activePage_ == Page::ThermalCycleBeta && thermalCycleInfoPopupVisible_ && thermalCycleInfoScrollPage_ > 0) {
        --thermalCycleInfoScrollPage_;
        refreshActivePage();
      }
      break;
    case TouchAction::ThermalCycleInfoDown:
      if (activePage_ == Page::ThermalCycleBeta && thermalCycleInfoPopupVisible_ &&
          thermalCycleInfoScrollPage_ + 1 < kThermalCycleInfoPageCount) {
        ++thermalCycleInfoScrollPage_;
        refreshActivePage();
      }
      break;
    case TouchAction::StorageRefresh:
      storageClearConfirm_ = false;
      if (storageManager_) {
        storageManager_->refresh();
      }
      refreshActivePage();
      break;
    case TouchAction::StorageClearMaps:
      if (storageManager_) {
        if (!storageClearConfirm_) {
          storageClearConfirm_ = true;
        } else {
          storageManager_->clearMaps();
          OfflineMapPackage::invalidateCache();
          mapPage_.invalidatePreparedBaseCache();
          storageClearConfirm_ = false;
        }
      }
      refreshActivePage();
      break;
    case TouchAction::AdvancedRecoverDisplay:
      executeAdvancedAction(action);
      break;
    case TouchAction::AdvancedMoveIgcToSd:
      requestAdvancedConfirmation(action, "COPIAR IGC PARA SD E APAGAR ORIGINAIS?");
      break;
    case TouchAction::AdvancedClearWifi:
      requestAdvancedConfirmation(action, "APAGAR SSID E SENHA SALVOS?");
      break;
    case TouchAction::AdvancedResetSettings:
      requestAdvancedConfirmation(action, "RESTAURAR CONFIGURAÇOES PADRAO?");
      break;
    case TouchAction::AdvancedClearWeatherCache:
      requestAdvancedConfirmation(action, "LIMPAR CACHE METEO/VENTO?");
      break;
    case TouchAction::AdvancedFormatSystem:
      requestAdvancedConfirmation(action, "APAGAR CONFIGS E IGC? IRREVERSIVEL.");
      break;
    case TouchAction::AdvancedConfirmYes:
      if (advancedConfirmAction_ != TouchAction::None) {
        const TouchAction pending = advancedConfirmAction_;
        executeAdvancedAction(pending);
      } else {
        refreshActivePage();
      }
      break;
    case TouchAction::AdvancedConfirmNo:
      advancedConfirmAction_ = TouchAction::None;
      setAdvancedNotice("AÇAO CANCELADA");
      refreshActivePage();
      break;
    case TouchAction::MapDownloadSelect0:
    case TouchAction::MapDownloadSelect1:
    case TouchAction::MapDownloadSelect2:
    case TouchAction::MapDownloadSelect3:
    case TouchAction::MapDownloadSelect4:
    case TouchAction::MapDownloadSelect5:
      if (mapDownloadManager_) {
        uint8_t index = 0;
        if (action == TouchAction::MapDownloadSelect1) {
          index = 1;
        } else if (action == TouchAction::MapDownloadSelect2) {
          index = 2;
        } else if (action == TouchAction::MapDownloadSelect3) {
          index = 3;
        } else if (action == TouchAction::MapDownloadSelect4) {
          index = 4;
        } else if (action == TouchAction::MapDownloadSelect5) {
          index = 5;
        }
        mapDownloadManager_->selectTarget(index);
      }
      refreshActivePage();
      break;
    case TouchAction::MapDownloadStart:
      if (mapDownloadManager_) {
        mapDownloadManager_->beginDownloadSelected(lastData_.latitudeDeg, lastData_.longitudeDeg, lastData_.gpsFix);
      }
      refreshActivePage();
      break;
    case TouchAction::MapDownloadCancel:
      if (mapDownloadManager_) {
        mapDownloadManager_->cancel();
      }
      refreshActivePage();
      break;
    case TouchAction::MapDownloadBack:
      if (mapDownloadManager_) {
        mapDownloadManager_->goBack();
      }
      refreshActivePage();
      break;
    case TouchAction::MapZoomIn:
      if (mapPage_.showMapControls()) {
        mapPage_.zoomIn();
        refreshActivePage();
      }
      break;
    case TouchAction::MapZoomOut:
      if (mapPage_.showMapControls()) {
        mapPage_.zoomOut();
        refreshActivePage();
      }
      break;
    case TouchAction::MapPanUp:
      if (mapPage_.showPanControls()) {
        mapPage_.panByScreenStep(0, 1);
        refreshActivePage();
      }
      break;
    case TouchAction::MapPanDown:
      if (mapPage_.showPanControls()) {
        mapPage_.panByScreenStep(0, -1);
        refreshActivePage();
      }
      break;
    case TouchAction::MapPanLeft:
      if (mapPage_.showPanControls()) {
        mapPage_.panByScreenStep(-1, 0);
        refreshActivePage();
      }
      break;
    case TouchAction::MapPanRight:
      if (mapPage_.showPanControls()) {
        mapPage_.panByScreenStep(1, 0);
        refreshActivePage();
      }
      break;
    case TouchAction::WeatherPrevDay:
      if (activePage_ == Page::WeatherStation && weatherDayIndex_ > 0) {
        weatherInfoPopupVisible_ = false;
        --weatherDayIndex_;
        refreshActivePage();
      }
      break;
    case TouchAction::WeatherNextDay:
      if (activePage_ == Page::WeatherStation && weatherClient_ && weatherDayIndex_ + 1 < weatherClient_->forecastDayCount()) {
        weatherInfoPopupVisible_ = false;
        ++weatherDayIndex_;
        refreshActivePage();
      }
      break;
    case TouchAction::WeatherInfo:
      if (activePage_ == Page::WeatherStation || activePage_ == Page::WeatherLocation) {
        weatherInfoPopupVisible_ = !weatherInfoPopupVisible_;
        if (weatherInfoPopupVisible_) {
          weatherInfoScrollPage_ = 0;
        }
        refreshActivePage();
      }
      break;
    case TouchAction::WeatherInfoUp:
      if ((activePage_ == Page::WeatherStation || activePage_ == Page::WeatherLocation) &&
          weatherInfoPopupVisible_ && weatherInfoScrollPage_ > 0) {
        --weatherInfoScrollPage_;
        refreshActivePage();
      }
      break;
    case TouchAction::WeatherInfoDown:
      if ((activePage_ == Page::WeatherStation || activePage_ == Page::WeatherLocation) &&
          weatherInfoPopupVisible_ && weatherInfoScrollPage_ + 1 < kWeatherInfoPageCount) {
        ++weatherInfoScrollPage_;
        refreshActivePage();
      }
      break;
    case TouchAction::ManualPageUp:
      if ((activePage_ == Page::ManualLogic || activePage_ == Page::ManualUser) && manualScrollOffset_ > 0) {
        manualScrollOffset_ = manualScrollOffset_ > kManualScrollStepLines ? manualScrollOffset_ - kManualScrollStepLines : 0;
        refreshActivePage();
      }
      break;
    case TouchAction::ManualPageDown:
      if (activePage_ == Page::ManualLogic || activePage_ == Page::ManualUser) {
        const uint8_t lineTotal = manualActiveLineCount();
        const uint8_t maxOffset = lineTotal > kManualVisibleLineCount ? lineTotal - kManualVisibleLineCount : 0;
        if (manualScrollOffset_ < maxOffset) {
          uint8_t nextOffset = manualScrollOffset_ + kManualScrollStepLines;
          if (nextOffset > maxOffset) {
            nextOffset = maxOffset;
          }
          manualScrollOffset_ = nextOffset;
          refreshActivePage();
        }
      }
      break;
    case TouchAction::ManualTab0:
    case TouchAction::ManualTab1:
    case TouchAction::ManualTab2:
    case TouchAction::ManualTab3:
    case TouchAction::ManualTab4:
      if (activePage_ == Page::ManualLogic || activePage_ == Page::ManualUser) {
        const uint8_t target = static_cast<uint8_t>(action) - static_cast<uint8_t>(TouchAction::ManualTab0);
        if (target < kManualPageCount && manualPage_ != target) {
          manualPage_ = target;
          manualScrollOffset_ = 0;
          refreshActivePage();
        }
      }
      break;
    default:
      break;
  }
  return true;
}

TouchAction MainScreen::actionAt(int32_t x, int32_t y) const {
  if (x < 0 || x >= EPD_WIDTH || y < 0 || y >= EPD_HEIGHT) {
    return TouchAction::None;
  }

  for (size_t i = 0; i < touchZoneCount_; ++i) {
    if (touchZones_[i].enabled && pointInRect(touchZones_[i].bounds, x, y)) {
      return touchZones_[i].action;
    }
  }
  return TouchAction::None;
}

TouchAction MainScreen::footerActionAt(int32_t x, int32_t y) const {
  if (y < layout_.trend.y || y >= EPD_HEIGHT || x < layout_.footerButtons.x || x >= layout_.footerButtons.x + layout_.footerButtons.width) {
    return TouchAction::None;
  }

  const int32_t buttonW = layout_.footerButtons.width / 5;
  int32_t slot = (x - layout_.footerButtons.x) / buttonW;
  if (slot < 0) slot = 0;
  if (slot > 4) slot = 4;

  if (slot == 0) {
    return activePage_ == Page::Dashboard || pageUsesFooterSettings(activePage_) ? TouchAction::OpenSettings : TouchAction::Back;
  }
  if (slot == 1) return TouchAction::ToggleAudio;
  if (slot == 2) return TouchAction::ToggleTracklog;
  if (slot == 3) return TouchAction::NextPage;
  return activePage_ == Page::Dashboard ? TouchAction::PowerRequest : TouchAction::Home;
}
