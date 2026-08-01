# Rampas e meteorologia

## Catálogo compacto

`weather/catalog.json` contém os campos necessários para pesquisa rápida no
ESP32: nome, cidade, estado, coordenadas, altitude, quadrantes de vento e caminho
do detalhe. O campo `siteCount` deve corresponder ao número de entradas.

Cada rampa possui um arquivo em `weather/sites/NNNNN.json` com procedência e
informações adicionais. Os esquemas públicos estão em:

- `weather/catalog.schema.json`
- `weather/site.schema.json`

## Funcionamento no dispositivo

O catálogo permite ao piloto pesquisar e favoritar rampas por região e estado.
A rampa escolhida fornece coordenadas para a previsão remota e para a seleção dos
pacotes de mapa dentro de um raio de 50, 100 ou 200 km. O mapa não é duplicado por
rampa: a mesma malha de `regions/` atende todos os modos de download.

Quando nenhuma rampa é escolhida, a estação meteorológica continua usando a
posição GPS atual.

## Atualização do catálogo

Após alterar os detalhes, gere novamente o arquivo compacto:

```powershell
py -3 scripts/build_flight_site_catalog.py --catalog-version 3
```

O importador mantém apenas fatos objetivos das páginas de referência. Conteúdo
editorial e mídia não devem ser copiados.
