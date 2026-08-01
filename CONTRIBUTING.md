# Como contribuir

Este repositório publica arquivos consumidos diretamente por dispositivos
BRVARIO. Uma alteração incorreta pode interromper downloads de mapas,
meteorologia ou atualização OTA.

## Antes de alterar

1. Abra uma issue descrevendo a mudança e a região ou recurso afetado.
2. Não renomeie `regions/`, `weather/`, `firmware/` nem os catálogos públicos.
3. Não inclua chaves de API, senhas Wi-Fi, tracklogs pessoais ou coordenadas
   privadas.
4. Gere catálogos pelos scripts; não altere milhares de entradas manualmente.

## Validação local

No Windows:

```powershell
py -3 scripts/validate_public_repository.py
```

Em Linux ou macOS:

```bash
python3 scripts/validate_public_repository.py
```

O comando confere a presença e o tamanho dos mapas, a compatibilidade do
catálogo legado, os detalhes das rampas e o tamanho/SHA-256 do firmware OTA.

## Pull requests

- Mantenha cada alteração focada em um único assunto.
- Explique quais endpoints públicos foram afetados.
- Informe o comando usado para gerar os arquivos.
- Aguarde a validação automática antes de publicar na `main`.

A publicação de binários OTA é reservada ao mantenedor do projeto.
