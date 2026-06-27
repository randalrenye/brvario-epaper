# Refresh reference

Esta nota registra a versao que resolveu o acumulo de sombras das informacoes antigas.

Modelo usado:

- `framebuffer_`: novo estado desejado.
- `displayedFramebuffer_`: ultimo estado confirmado como mostrado na tela.
- `baseFramebuffer_`: layout estatico.
- Para cada area dinamica, o codigo calcula o menor retangulo alterado.
- A transicao correta usa duas mascaras:
  - mascara dos pixels antigos alterados para apagar em branco;
  - mascara dos pixels novos alterados para desenhar em preto.

O problema visual restante desta versao era a transicao: quando implementada com `epd_draw_image()`, cada mascara passava pelo render grayscale completo da LilyGo, deixando a troca mais perceptivel.

Tentativa descartada: a versao com pulsos diretos via `ed047tc1.h` compilou, mas no painel real borrou a tela inteira e gerou riscos/manchas. Nao usar esse caminho como base sem portar corretamente a engine high-level/MODE_DU do projeto original.

Versao bem-sucedida:

- Atualizacao parcial em lote, com os dados mudando juntos.
- `lastUiUpdateMs` reiniciado somente depois do refresh terminar; a cadencia atual usa `kUiUpdateIntervalMs = 750` para reduzir o tempo entre mudancas no painel.
- `kWhiteErasePasses = 1` para reduzir o tempo em branco.
- As areas dinamicas sao atualizadas separadamente dentro de um unico `epd_poweron()`, evitando que topo, centro e rodape virem um retangulo quase do tamanho da tela inteira.
- Patch local da LilyGo em `scripts/patch_lilygo_epd.py`, usando `custom_lilygo_epd_frame_count` no `platformio.ini`.
- `custom_lilygo_epd_frame_count = 8` e o ponto de equilibrio atual: abaixo disso a transicao fica mais rapida, mas o preto pode ficar fraco demais.
- Resultado validado no painel real pelo usuario: transicao de dados e atualizacao da tela ficaram boas.

Manutencao de contraste inferior:

- A faixa inferior do painel pode perder contraste visual aos poucos mesmo quando o framebuffer nao muda.
- Para evitar que rodape, `ALT AGL` e `VELOCIDADE VENTO` fiquem claros, `EpdDisplay::reinforceArea()` redesenha periodicamente apenas os pixels atuais dessa faixa, sem apagar em branco antes.
- O reforco roda a cada `kLowerBandContrastEveryCycles = 4` somente quando algum dado da faixa inferior mudou, cobrindo a parte inferior a partir dos blocos inferiores das colunas.

Filtro por estado visual:

- `MainScreen` mantem um resumo dos valores que realmente aparecem na tela, como altitude arredondada, velocidade arredondada, minuto do relogio e vario em decimos.
- Se um valor visual nao mudou, a area dele nao entra na lista de refresh normal.
- O filtro evita atualizar dados iguais, mas o envio ao display acontece em um unico lote por ciclo para evitar que as areas mudem visualmente em sequencia.
- As colunas laterais sao agrupadas visualmente: se qualquer dado da coluna esquerda muda, a coluna esquerda entra no lote; se qualquer dado da coluna direita muda, a coluna direita entra no lote.
- O reforco de contraste inferior, quando necessario, e combinado na mesma chamada de atualizacao para nao gerar uma segunda piscada.
