# Auditoria de estabilidade - 2026-06-19

## Sintoma investigado

Congelamento do ESP32-S3 depois de alguns minutos, sem evidencia de falha de
alimentacao. As mudancas recentes mais relevantes eram GPS em UART, audio em
task FreeRTOS e varias alteracoes no refresh do e-paper.

O repositorio ainda nao possui commits Git. Por isso, a comparacao foi feita
com os arquivos atuais e com `docs/refresh-reference.md`.

## Causas encontradas

### 1. Concorrencia no barramento I2C

GT911, BMP280/BME280 e PCF8563 compartilham `Wire` nos pinos SDA 18 e SCL 17.
O barometro faz leituras em uma task FreeRTOS, enquanto o touch era lido no
loop principal sem mutex.

O touch tambem executava uma recuperacao preventiva a cada 120 segundos. Essa
rotina chamava `Wire.end()`, pulsava SCL manualmente e iniciava `Wire` de novo,
mesmo que a task do barometro estivesse no meio de uma transacao.

Correcao:

- criado `system/I2CBusLock`;
- touch, barometro e RTC agora serializam todas as transacoes;
- a recuperacao completa do GT911 continua disponivel apenas para falha real;
- removida a recuperacao preventiva fixa de 120 segundos.

### 2. Audio concorrente nos dois nucleos

A task `vario_audio` e os sons de evento executados no loop principal podiam
alterar simultaneamente LEDC, frequencia, duty e estado interno do buzzer.

Correcao:

- criado mutex recursivo exclusivo para a saida de audio;
- cadencia do vario e sons de evento nao acessam mais LEDC ao mesmo tempo;
- mantida a task dedicada para o som nao depender do refresh do display.

### 3. Varredura pesada do cartao SD

Quando nao havia voo em gravacao, `updateFlightRecorder()` chamava
`StorageManager::refresh()` a cada 10 segundos. Essa funcao calcula estatisticas
e percorre `/maps` recursivamente duas vezes.

Correcao:

- criado `StorageManager::ensureMounted()`, que apenas verifica ou monta o SD;
- o monitoramento de insercao passou para 30 segundos;
- a varredura completa permanece somente nas telas que realmente precisam das
  estatisticas de armazenamento.

### 4. Refresh do e-paper

O codigo atual unia todas as areas alteradas em um unico retangulo. Mudancas no
topo e no rodape podiam produzir uma atualizacao quase de tela inteira. Tambem
havia full refresh automatico aproximadamente a cada 99 segundos.

Correcao:

- areas distantes voltaram a ser processadas separadamente no mesmo
  `epd_poweron()`, como na versao documentada e validada;
- full refresh do dashboard passou de 180 para 900 ciclos;
- paginas paradas fazem full refresh preventivo somente a cada 15 minutos;
- operacoes de display acima de 1200 ms sao informadas no Serial.

### 5. Conflito do GPS com a porta de diagnostico

O GPS usa GPIO44 RX e GPIO43 TX. Com `ARDUINO_USB_CDC_ON_BOOT=0`, `Serial`
tambem usa UART0 nesses pinos.

Correcao:

- `ARDUINO_USB_CDC_ON_BOOT=1`;
- logs passam pelo USB Hardware CDC;
- GPIO43/44 ficam exclusivos para o GPS.

## Diagnostico de bancada

A cada 30 segundos o monitor Serial imprime:

```text
HEALTH: up=... cpu=... page=... heap=... min=... maior=... psram=...
stackLoop=... stackAudio=... stackBaro=... i2cTimeout=...
```

Observar:

- `min` caindo continuamente: possivel vazamento/fragmentacao de heap;
- `maior` muito menor que `heap`: fragmentacao;
- `stackAudio`, `stackBaro` ou `stackLoop` proximos de zero: risco de overflow;
- `i2cTimeout` crescendo rapidamente: dispositivo ou fiação mantendo o I2C;
- `EPD: refresh parcial lento`: pausa causada pelo painel, nao travamento da CPU;
- novo boot com `PANIC`, `TASK_WDT`, `INT_WDT` ou `BROWNOUT`: causa do reset.

## Risco residual

O BLE usa callbacks executados pela pilha Bluetooth e compartilha diversos
campos com o loop principal. O fluxo atual e incremental e compila, mas uma
auditoria especifica de sincronizacao continua recomendada se o problema
aparecer exclusivamente durante exportacao ou sincronizacao de IGC.
