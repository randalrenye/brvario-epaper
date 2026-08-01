# Arquitetura pública

## Objetivo

O repositório funciona como uma origem estática para arquivos baixados pelo
ESP32-S3. O GitHub Pages publica a branch `main` sem processamento Jekyll.

```text
BRVARIO
  ├─ consulta regions/catalog.json
  │    └─ baixa somente os pacotes .brmap necessários para o microSD
  ├─ consulta weather/catalog.json
  │    └─ baixa o detalhe da rampa selecionada quando necessário
  └─ consulta firmware/manifest.json
       └─ baixa e valida o binário OTA indicado
```

## Contratos estáveis

| Diretório | Conteúdo | Pode mudar internamente? | Pode ser renomeado? |
| --- | --- | --- | --- |
| `regions/` | Mapas e catálogo canônico | Sim, com nova versão de catálogo | Não |
| `weather/` | Rampas, esquemas e catálogo | Sim, preservando o esquema | Não |
| `firmware/` | Manifesto e binário OTA | Sim, em uma nova publicação | Não |
| `catalog.json` | Catálogo legado de mapas | Deve acompanhar `regions/catalog.json` | Não |

O diretório `build/` e o arquivo `catalog.plan.json` são intermediários locais.
Eles não fazem parte da publicação.

## Memória e downloads

Os mapas são armazenados no microSD. O firmware seleciona pacotes por estado,
posição GPS ou raio em torno de uma rampa. Todos os modos usam a mesma malha de
arquivos, evitando duplicação no GitHub e no cartão.

## Publicação segura

Toda mudança deve passar por `scripts/validate_public_repository.py`. A validação
confere referências, tamanhos e hashes antes que o GitHub Pages publique a nova
versão da branch `main`.
