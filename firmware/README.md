# BRVARIO firmware OTA

Arquivos publicados para atualizacao remota da LilyGo T5 EPD47 S3.

- `manifest.json`: versao ativa, build, placa, tamanho, SHA-256 e URL do firmware.
- `brvario-epaper-T5-ePaper-S3-v*.bin`: imagem compilada para a particao OTA.

O dispositivo aceita somente builds maiores, destinados a `T5-ePaper-S3`, e valida
o tamanho e o SHA-256 antes de reiniciar.
