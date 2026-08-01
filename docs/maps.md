# Mapas offline

## Estrutura

O catálogo atual está em `regions/catalog.json`. Cada entrada identifica um
pacote `.brmap`, seu estado, área geográfica, versão e tamanho em bytes.

```json
{
  "id": "mg_243",
  "name": "Minas Gerais 243",
  "state": "MG",
  "file": "mg_243.brmap",
  "size": 1234567,
  "version": 2,
  "radiusKm": 35,
  "centerLat": -16.73,
  "centerLon": -40.42,
  "latMin": -16.99,
  "latMax": -16.47,
  "lonMin": -40.90,
  "lonMax": -39.94
}
```

O `catalog.json` da raiz contém as mesmas regiões, mas usa caminhos iniciados
por `regions/`. Ele é mantido para compatibilidade com versões antigas.

## Geração

O planejamento e os arquivos temporários ficam fora do repositório, por exemplo
em `build/maps-brasil/`.

Gerar o plano e construir os pacotes de Minas Gerais:

```powershell
py -3 scripts/make_brazil_region_maps.py --states MG --output-root build/maps-brasil --build
```

Gerar somente um pacote para teste:

```powershell
py -3 scripts/make_brazil_region_maps.py --states MG --output-root build/maps-brasil --build --only-id mg_243
```

Gerar o catálogo apenas com arquivos realmente concluídos:

```powershell
py -3 scripts/make_brazil_publish_catalog.py --plan build/maps-brasil/catalog.plan.json --output-root build/maps-brasil
```

Copie para `regions/` somente os `.brmap` finalizados e o catálogo validado. O
ESP32 nunca gera DEM, curvas de nível ou relevo durante o voo.

## Fonte e processamento

O gerador usa limites estaduais do IBGE e relevo DEM SRTM. O fluxo é DEM-first:
recorte, reamostragem, curvas de nível, composição monocromática e exportação no
formato compacto consumido pelo firmware.

## Compatibilidade

- Não reutilize um `id` para outra área.
- Atualize `size` sempre que o arquivo mudar.
- Incremente `version` quando o formato ou conteúdo exigir nova invalidação.
- Nunca publique no catálogo um arquivo que ainda não exista.
