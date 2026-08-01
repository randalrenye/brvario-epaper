# BRVARIO E-PAPER

[![Validar dados públicos](https://github.com/randalrenye/brvario-epaper/actions/workflows/validate-public-data.yml/badge.svg)](https://github.com/randalrenye/brvario-epaper/actions/workflows/validate-public-data.yml)

O **BRVARIO E-PAPER** é um instrumento de voo livre baseado em ESP32-S3 e na
LilyGo T5 EPD47 S3. O projeto reúne variômetro, navegação GPS, mapa topográfico
offline, informações meteorológicas, tracklog IGC, BLE, Wi-Fi e atualização de
firmware OTA em uma tela e-paper de baixo consumo.

Este repositório é a origem pública dos dados e artefatos distribuídos ao
dispositivo. O código-fonte completo do firmware está em desenvolvimento e não
é espelhado integralmente aqui neste momento.

## Recursos atuais

- Dashboard horizontal otimizado para e-paper de 4,7 polegadas.
- Variômetro sonoro e visual, GPS, altitude, velocidade, vento e dados de voo.
- Mapas vetoriais e topográficos offline armazenados no microSD.
- Download de mapas por estado, posição GPS ou rampa de voo.
- Catálogo público de rampas para pesquisa e meteorologia.
- Registro de voo IGC e integração BLE.
- Atualização do firmware por Wi-Fi com validação de tamanho e SHA-256.

## Serviços públicos

| Serviço | Endpoint estável | Uso |
| --- | --- | --- |
| Mapas | [`regions/catalog.json`](https://randalrenye.github.io/brvario-epaper/regions/catalog.json) | Catálogo consumido pelo firmware atual |
| Mapas legado | [`catalog.json`](https://randalrenye.github.io/brvario-epaper/catalog.json) | Compatibilidade com versões anteriores |
| Rampas | [`weather/catalog.json`](https://randalrenye.github.io/brvario-epaper/weather/catalog.json) | Pesquisa de rampas e seleção meteorológica |
| Firmware OTA | [`firmware/manifest.json`](https://randalrenye.github.io/brvario-epaper/firmware/manifest.json) | Versão estável e binário de atualização |

Esses caminhos fazem parte do contrato do dispositivo. Eles não devem ser
renomeados ou movidos sem uma migração compatível no firmware.

## Estrutura

```text
.
├── firmware/    # manifesto e binário OTA estável
├── regions/     # catálogo e pacotes regionais .brmap
├── scripts/     # geradores e validadores de dados públicos
├── weather/     # catálogo, esquemas e detalhes das rampas
├── docs/        # arquitetura e procedimentos de publicação
└── catalog.json # catálogo legado de mapas
```

O GitHub Pages publica diretamente a branch `main` em
[`randalrenye.github.io/brvario-epaper`](https://randalrenye.github.io/brvario-epaper/).

## Documentação

- [Arquitetura pública](docs/architecture.md)
- [Mapas offline](docs/maps.md)
- [Rampas e meteorologia](docs/weather.md)
- [Atualização OTA](docs/firmware-update.md)
- [Como contribuir](CONTRIBUTING.md)
- [Política de segurança](SECURITY.md)

## Segurança de voo

O BRVARIO é um instrumento auxiliar. Mapas, previsões e sensores podem conter
imprecisões ou ficar indisponíveis. O piloto continua responsável pela tomada de
decisão, pelo planejamento e pelo cumprimento das regras de voo aplicáveis.

## Direitos e fontes

Não há licença de código aberto concedida por este repositório. Os dados de
terceiros permanecem sujeitos aos termos de suas fontes. Consulte
[`NOTICE.md`](NOTICE.md) para atribuições e avisos.
