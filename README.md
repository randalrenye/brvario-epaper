# BRVARIO E-PAPER

Firmware PlatformIO/Arduino para variometro de voo livre usando ESP32-S3 e LilyGo T5 EPD47 S3.

Recursos principais:

- Dashboard horizontal 960x540 para e-paper.
- Refresh parcial otimizado para reduzir sombras e consumo.
- GPS, barometro BMP280, vario sonoro, tracklog IGC e BLE para sincronizacao.
- Estacao meteorologica com Wi-Fi/OpenWeather e dados locais.
- Mapa offline preparado para microSD.
- Paginas de configuracao, audio, personalizacao, tracklog e sistema.

## Build

Use o ambiente `T5-ePaper-S3` no PlatformIO.

```bash
platformio run --environment T5-ePaper-S3
```

## Segredos locais

Nao coloque chaves reais diretamente no Git. Para habilitar OpenWeather localmente, defina a macro `OPENWEATHER_API_KEY` somente no seu ambiente de build local.
