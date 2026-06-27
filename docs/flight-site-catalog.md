# Catálogo de rampas BRVARIO

O catálogo de rampas é próprio do BRVARIO e é publicado como JSON no GitHub
Pages. O firmware não faz scraping de sites.

## Organização

```text
weather/
  catalog.json
  catalog.schema.json
  site.schema.json
  sites/
    00001.json
    00002.json
```

Cada arquivo em `weather/sites/` é a fonte de verdade de uma rampa. Ele pode
armazenar:

- identidade, cidade, UF e país;
- coordenadas E7;
- altitude e desnível;
- quadrantes;
- tipos de voo e melhores meses;
- resumo, acesso, decolagem e pouso;
- clube responsável;
- vantagens, riscos e alertas;
- recordes, contatos, mídia e informações gerais;
- autoria, fonte, licença e data de verificação.

O arquivo `weather/catalog.json` é um índice compacto gerado automaticamente.
Cada rampa deve ficar em uma única linha nesse índice porque o ESP32 cria um
índice de offsets e carrega apenas as cinco rampas visíveis na tela.

## Gerar e validar

```powershell
py scripts\build_flight_site_catalog.py `
  --catalog-version 1 `
  --updated-at 2026-06-19
```

O script bloqueia:

- IDs repetidos ou fora de `1..65535`;
- nomes maiores que os buffers do firmware;
- coordenadas inválidas;
- UFs inválidas;
- quadrantes desconhecidos;
- fichas sem proveniência e licença;
- mais de 512 rampas.

## Publicação

Publique a pasta `weather/` na raiz do repositório servido pelo GitHub Pages.
O firmware está configurado para:

```text
https://randalrenye.github.io/brvario-epaper/weather/catalog.json
```

Ao atualizar:

1. o ESP32 baixa para `/weather/catalog.tmp`;
2. valida schema, versão, limites e cada rampa;
3. preserva o catálogo anterior como backup;
4. instala o novo arquivo;
5. recarrega o índice;
6. restaura o backup se a recarga falhar.

Sem arquivo válido, Pedra Grande permanece disponível no catálogo interno.

## Proveniência

O Guia 4 Ventos é usado como referência factual para a importação automatizada.
O importador não copia descrições, acesso, decolagem, pouso, prós, contras,
recordes, contatos, fotos, vídeos ou outros conteúdos editoriais.

Cada ficha importada contém somente:

- nome;
- cidade e UF;
- coordenadas E7;
- altitude;
- desnível, quando disponível;
- quadrantes;
- URL pública de referência e data de verificação.

## Inventário do Guia 4 Ventos

O inventário de páginas públicas pode ser atualizado sem copiar os dados:

```powershell
py scripts\import_guia4ventos_catalog.py --insecure
```

Ele grava apenas IDs, títulos, URLs, slugs e datas em:

```text
weather/sources/guia4ventos.inventory.json
```

Para atualizar as fichas factuais:

```powershell
py scripts\import_guia4ventos_catalog.py `
  --insecure `
  --import-facts
```

O intervalo padrão entre páginas é 500 ms e pode ser aumentado com
`--delay-ms`. Se uma ficha não tiver os campos mínimos ou usar um formato
desconhecido, o processo termina com erro e grava um relatório para revisão.
