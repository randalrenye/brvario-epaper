# Scripts de manutenção

| Script | Finalidade |
| --- | --- |
| `make_brazil_region_maps.py` | Planeja e gera pacotes regionais `.brmap` |
| `make_relief_brmap.py` | Gera relevo e curvas a partir do DEM |
| `make_brazil_publish_catalog.py` | Publica somente mapas realmente gerados |
| `make_brazil_catalog_header.py` | Gera o catálogo compacto do firmware |
| `make_brmap.py` | Converte GeoJSON simples para `.brmap` |
| `import_guia4ventos_catalog.py` | Importa fatos objetivos das rampas |
| `build_flight_site_catalog.py` | Valida detalhes e cria o catálogo compacto |
| `validate_public_repository.py` | Valida toda a árvore antes da publicação |
| `patch_lilygo_epd.py` | Ajuste de build do driver LilyGo EPD47 |

Arquivos gerados, caches DEM e planos intermediários devem ficar em `build/`,
que não é versionado.
