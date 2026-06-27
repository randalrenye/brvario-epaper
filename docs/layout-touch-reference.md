# Layout e touch de referencia

Registro da versao atual do dashboard antes de novas mudancas grandes.

- Layout horizontal 960x540.
- Barra superior fina: `Layout::header = {8, 8, 944, 42}`.
- Coluna esquerda com altitudes e assistente de termica: `Layout::info = {8, 50, 270, 482}`.
- Centro com vario, ganho, planeio e duracao de voo: `Layout::vario = {278, 50, 400, 422}`.
- Coluna direita com velocidade do vento, rosa dos ventos e velocidade solo: `Layout::speed = {678, 50, 274, 482}`.
- Blocos superiores das colunas laterais alinhados: `ALT GPS` e `VELOCIDADE SOLO`.
- Blocos inferiores das colunas laterais alinhados: `ALT AGL` e `VELOCIDADE VENTO`.
- Blocos centrais pareados: `ASSISTENTE TERMICA` e `RUMO / DIR. VENTO`, com titulos centralizados em negrito, circulos no mesmo raio de 114 px e centro vertical alinhado 8 px mais baixo para dar respiro aos titulos.
- Widget central do vario tem titulo `VARIO` no topo da area.
- Rodape central para botoes touch: `Layout::footerButtons = {292, 472, 376, 60}`.
- O refresh bem-sucedido continua sendo o parcial por diferenca registrado em `docs/refresh-reference.md`.
- As transicoes por botoes usam `updateAreas()` na area da tela em vez de `fullRefresh()`, evitando o `epd_clear()` que causava piscadas fortes.

Base de touch validada pelo projeto de referencia:

- Driver `TouchDrvGT911`.
- `Wire.begin(BOARD_SDA, BOARD_SCL)`.
- Detecta GT911 em `0x14`, depois `0x5D`.
- `touch.setMaxCoordinates(EPD_WIDTH, EPD_HEIGHT)`.
- `touch.setSwapXY(true)`.
- `touch.setMirrorXY(false, true)`.
- Leitura em modo responsivo a cada 35 ms, com trava de 160 ms para nao repetir comando enquanto o dedo fica apoiado.
- Coordenadas usadas diretamente nas zonas do rodape, sem tentativa de remapeamento extra.
- Zonas touch do rodape sao um pouco maiores que os icones para compensar imprecisao natural do painel.

Mapa atual dos botoes:

- Botao 1: configuracao; nas paginas secundarias vira voltar.
- Botao 2: personalizacao.
- Botao 3: tracklog.
- Botao 4: audio.
