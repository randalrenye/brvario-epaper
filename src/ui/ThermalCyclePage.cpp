#include "ui/ThermalCyclePage.h"

#include <stdio.h>
#include <string.h>

#include "config/AppConfig.h"

namespace {

void drawMetric(EpdDisplay& display, int32_t labelX, int32_t valueX, int32_t y, const char* label, const char* value, uint8_t scale = 3) {
  display.drawSmallTextBold(label, labelX, y, scale, AppConfig::kBlack);
  display.drawSmallTextBoldAligned(value, valueX, y, scale, AppConfig::kBlack, EpdDisplay::Align::Right);
}

}  // namespace

void ThermalCyclePage::render(EpdDisplay& display,
                              const Rect_t& screen,
                              int32_t contentBottomY,
                              const ThermalCycleBeta::Snapshot& state) {
  display.clearBuffer(AppConfig::kWhite);
  display.drawRect(screen, AppConfig::kBlack);
  display.drawRect({screen.x + 1, screen.y + 1, screen.width - 2, screen.height - 2}, AppConfig::kBlack);
  display.drawLine(screen.x, screen.y + 48, screen.x + screen.width - 1, screen.y + 48, AppConfig::kBlack);

  display.drawSmallTextBoldAligned("CICLO TERMAL BETA", screen.x + screen.width / 2, screen.y + 12, 3, AppConfig::kBlack,
                                   EpdDisplay::Align::Center);
  display.drawSmallTextBoldAligned("Experimental - somente pressao BMP280", screen.x + screen.width / 2, screen.y + 58, 2,
                                   AppConfig::kBlack, EpdDisplay::Align::Center);

  if (!state.pressureValid) {
    display.drawSmallTextBoldAligned("BMP280 SEM PRESSAO VALIDA", screen.x + screen.width / 2, screen.y + 156, 3, AppConfig::kBlack,
                                     EpdDisplay::Align::Center);
    display.drawSmallTextBoldAligned("Aguardando nova amostra do barometro.", screen.x + screen.width / 2, screen.y + 210, 3,
                                     AppConfig::kBlack, EpdDisplay::Align::Center);
    display.drawSmallTextBoldAligned("Temperatura, GPS, vento e IA nao sao usados.", screen.x + screen.width / 2, screen.y + 258, 3,
                                     AppConfig::kBlack, EpdDisplay::Align::Center);
    return;
  }

  if (state.status == ThermalCycleBeta::Status::CollectingBase) {
    char elapsed[16];
    char total[16];
    formatClock(state.elapsedMs, elapsed, sizeof(elapsed));
    formatClock(ThermalCycleBeta::THERMAL_WARMUP_MS, total, sizeof(total));

    display.drawSmallTextBoldAligned("Coletando base...", screen.x + screen.width / 2, screen.y + 132, 4, AppConfig::kBlack,
                                     EpdDisplay::Align::Center);
    char timeLine[40];
    snprintf(timeLine, sizeof(timeLine), "Tempo: %s / %s", elapsed, total);
    display.drawSmallTextBoldAligned(timeLine, screen.x + screen.width / 2, screen.y + 196, 3, AppConfig::kBlack,
                                     EpdDisplay::Align::Center);

    const int32_t barX = screen.x + 180;
    const int32_t barY = screen.y + 250;
    const int32_t barW = screen.width - 360;
    const int32_t barH = 28;
    display.drawRect({barX, barY, barW, barH}, AppConfig::kBlack);
    const uint32_t elapsedForBarMs =
        state.elapsedMs > ThermalCycleBeta::THERMAL_WARMUP_MS ? ThermalCycleBeta::THERMAL_WARMUP_MS : state.elapsedMs;
    const int32_t fillW =
        static_cast<int32_t>((static_cast<uint64_t>(barW - 4) * elapsedForBarMs) / ThermalCycleBeta::THERMAL_WARMUP_MS);
    if (fillW > 0) {
      display.fillRect({barX + 2, barY + 2, fillW, barH - 4}, AppConfig::kBlack);
    }

    display.drawSmallTextBoldAligned("Mantenha o BRVARIO parado na rampa/decolagem.", screen.x + screen.width / 2, screen.y + 316, 3,
                                     AppConfig::kBlack, EpdDisplay::Align::Center);
    display.drawSmallTextBoldAligned("A analise comeca apos a base inicial.", screen.x + screen.width / 2, screen.y + 358, 3,
                                     AppConfig::kBlack, EpdDisplay::Align::Center);
  } else {
    char confidence[28];
    snprintf(confidence, sizeof(confidence), "%u%% %s", static_cast<unsigned>(state.confidencePercent),
             ThermalCycleBeta::confidenceLabel(state.confidencePercent));

    const char* statusText = ThermalCycleBeta::statusLabel(state.status);
    const uint8_t statusScale = strlen(statusText) > 17 ? 2 : 3;
    display.drawSmallTextBold("Status:", screen.x + 56, screen.y + 96, 3, AppConfig::kBlack);
    display.drawSmallTextBold(statusText, screen.x + 210, screen.y + (statusScale == 3 ? 96 : 102), statusScale, AppConfig::kBlack);
    display.drawSmallTextBold("Confianca:", screen.x + 56, screen.y + 136, 3, AppConfig::kBlack);
    display.drawSmallTextBold(confidence, screen.x + 272, screen.y + 136, 3, AppConfig::kBlack);

    display.drawLine(screen.x + 46, screen.y + 178, screen.x + screen.width - 46, screen.y + 178, AppConfig::kBlack);

    char lastPulse[24];
    char detectedCycle[24];
    char patternPeriod[16];
    char patternCycle[28];
    char nextCycle[28];
    formatMinutes(state.lastEventAgeMs, lastPulse, sizeof(lastPulse));
    formatMinutes(state.detectedCycleMs != 0 ? state.detectedCycleMs : state.medianCycleMs, detectedCycle, sizeof(detectedCycle));
    formatMinutes(state.bestPeriodMs, patternPeriod, sizeof(patternPeriod));
    formatSignedMinutes(state.nextCycleRemainingMs, nextCycle, sizeof(nextCycle));
    if (state.bestPeriodMs != 0) {
      snprintf(patternCycle, sizeof(patternCycle), "%s %u%%", patternPeriod, static_cast<unsigned>(state.periodScorePercent));
    } else {
      snprintf(patternCycle, sizeof(patternCycle), "--");
    }

    drawMetric(display, screen.x + 72, screen.x + screen.width - 72, screen.y + 204, "Ultimo pulso", state.eventCount > 0 ? lastPulse : "--");
    const bool showCycleWindow = state.hasCycle && state.eventCount > 0 && state.detectedCycleMs >= ThermalCycleBeta::MIN_CYCLE_MS;
    drawMetric(display, screen.x + 72, screen.x + screen.width - 72, screen.y + 248, "Ciclo estim.", state.hasCycle ? detectedCycle : "--");
    drawMetric(display, screen.x + 72, screen.x + screen.width - 72, screen.y + 292, "Padrao baro", patternCycle);
    drawMetric(display, screen.x + 72, screen.x + screen.width - 72, screen.y + 336, "Prox. janela",
               showCycleWindow ? nextCycle : "sem previsao confiavel");

    char events[20];
    char quality[16];
    char noise[24];
    snprintf(events, sizeof(events), "%u", static_cast<unsigned>(state.eventCount));
    snprintf(quality, sizeof(quality), "%u%%", static_cast<unsigned>(state.qualityPercent));
    snprintf(noise, sizeof(noise), "%s %.3f", ThermalCycleBeta::noiseLabel(state.noiseHpa), static_cast<double>(state.noiseHpa));
    drawMetric(display, screen.x + 72, screen.x + 310, screen.y + 386, "Eventos", events, 2);
    drawMetric(display, screen.x + 365, screen.x + 610, screen.y + 386, "Qualid.", quality, 2);
    drawMetric(display, screen.x + 650, screen.x + screen.width - 72, screen.y + 386, "Ruido", noise, 2);

    const char* phaseHint = nullptr;
    switch (state.status) {
      case ThermalCycleBeta::Status::PossiblePulse:
        phaseHint = "Possivel pulso: analisando o sinal barometrico.";
        break;
      case ThermalCycleBeta::Status::BetweenCycles:
        phaseHint = "Entre ciclos: proxima janela ainda distante.";
        break;
      case ThermalCycleBeta::Status::ProbableWindow:
        phaseHint = "Janela provavel: observe rampa, vento e nuvens.";
        break;
      case ThermalCycleBeta::Status::AwaitingNewPulse:
        phaseHint = "Ciclo passou: aguardando novo pulso.";
        break;
      case ThermalCycleBeta::Status::IrregularCycle:
        phaseHint = "Ciclo irregular: padrao com baixa confianca.";
        break;
      default:
        break;
    }

    if (state.movementLikely) {
      display.drawSmallTextBoldAligned("Movimento provavel: coleta mascarada por seguranca.", screen.x + screen.width / 2, screen.y + 414, 2,
                                       AppConfig::kBlack, EpdDisplay::Align::Center);
    } else if (state.artifactMasked) {
      display.drawSmallTextBoldAligned("Aguardando estabilizar apos toque/refresh.", screen.x + screen.width / 2, screen.y + 414, 2,
                                       AppConfig::kBlack, EpdDisplay::Align::Center);
    } else if (phaseHint) {
      display.drawSmallTextBoldAligned(phaseHint, screen.x + screen.width / 2, screen.y + 414, 2,
                                       AppConfig::kBlack, EpdDisplay::Align::Center);
    } else if (state.status == ThermalCycleBeta::Status::IrregularCycle || state.status == ThermalCycleBeta::Status::InsufficientSignal) {
      display.drawSmallTextBoldAligned("Ainda sem previsao confiavel. Continue observando.", screen.x + screen.width / 2, screen.y + 414, 2,
                                       AppConfig::kBlack, EpdDisplay::Align::Center);
    }
  }

  const int32_t debugY = contentBottomY - 34;
  display.drawLine(screen.x + 24, debugY - 10, screen.x + screen.width - 24, debugY - 10, AppConfig::kBlack);
  char debug[112];
  snprintf(debug,
           sizeof(debug),
           "P%.2f  C%.2f  L%.2f  R%+.4f  D%+.4f  N%u",
           static_cast<double>(state.pressureHpa),
           static_cast<double>(state.shortEmaHpa),
           static_cast<double>(state.longEmaHpa),
           static_cast<double>(state.residualHpa),
           static_cast<double>(state.dPdtHpaPerSec),
           static_cast<unsigned>(state.sampleCount));
  display.drawSmallTextBold(debug, screen.x + 34, debugY, 2, AppConfig::kBlack);
}

void ThermalCyclePage::formatMinutes(uint32_t valueMs, char* out, size_t outSize) {
  if (!out || outSize == 0) {
    return;
  }
  if (valueMs == 0) {
    snprintf(out, outSize, "--");
    return;
  }
  const uint32_t minutes = (valueMs + 30000UL) / 60000UL;
  if (minutes == 0) {
    snprintf(out, outSize, "<1 min");
  } else {
    snprintf(out, outSize, "%lu min", static_cast<unsigned long>(minutes));
  }
}

void ThermalCyclePage::formatSignedMinutes(int32_t valueMs, char* out, size_t outSize) {
  if (!out || outSize == 0) {
    return;
  }
  if (valueMs <= -60000) {
    snprintf(out, outSize, "janela passou");
    return;
  }
  if (valueMs <= 0) {
    snprintf(out, outSize, "agora");
    return;
  }
  char minutes[16];
  formatMinutes(static_cast<uint32_t>(valueMs), minutes, sizeof(minutes));
  snprintf(out, outSize, "~%s", minutes);
}

void ThermalCyclePage::formatClock(uint32_t valueMs, char* out, size_t outSize) {
  if (!out || outSize == 0) {
    return;
  }
  const uint32_t totalSeconds = valueMs / 1000UL;
  const uint32_t minutes = totalSeconds / 60UL;
  const uint32_t seconds = totalSeconds % 60UL;
  snprintf(out, outSize, "%02lu:%02lu", static_cast<unsigned long>(minutes), static_cast<unsigned long>(seconds));
}
