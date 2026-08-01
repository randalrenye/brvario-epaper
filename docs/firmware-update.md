# Atualização de firmware OTA

## Manifesto

O dispositivo consulta `firmware/manifest.json`. O arquivo informa canal, placa,
versão, número de build, URL, tamanho e SHA-256 do binário.

```json
{
  "schema": "brvario.firmware",
  "schemaVersion": 1,
  "channel": "stable",
  "board": "T5-ePaper-S3",
  "version": "0.1.1",
  "build": 2,
  "url": "https://randalrenye.github.io/brvario-epaper/firmware/arquivo.bin",
  "size": 2138688,
  "sha256": "...",
  "mandatory": false
}
```

O número `build` é a referência de comparação e deve sempre aumentar. O nome da
placa deve continuar exatamente igual ao esperado pelo firmware.

## Fluxo no dispositivo

1. O usuário abre a página de atualização.
2. O Wi-Fi conecta usando as credenciais salvas.
3. O ESP32 valida o manifesto e verifica se há build mais novo.
4. O binário é gravado na partição OTA inativa em blocos.
5. Tamanho e SHA-256 são conferidos antes da ativação.
6. O aparelho reinicia somente após a validação completa.

Mapas, preferências e tracklogs do microSD não são alterados.

## Publicação

1. Compile o ambiente oficial de release.
2. Gere um binário com versão e build atualizados.
3. Calcule tamanho e SHA-256 do arquivo final.
4. Atualize o manifesto apontando para a URL definitiva do GitHub Pages.
5. Execute `py -3 scripts/validate_public_repository.py`.
6. Publique binário e manifesto na mesma pull request.

Somente o mantenedor deve publicar firmware OTA. Nunca envie chaves de API,
credenciais Wi-Fi ou artefatos de depuração junto ao binário.
