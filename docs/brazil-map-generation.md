# BRVARIO Brazil regional maps

The BRVARIO map repository should not use one giant file per state. For the
4.7 inch e-paper screen, a state-sized raster becomes too coarse and loses
terrain depth when the pilot view is zoomed in.

The first scalable approach is to split each state into overlapping regional
packages. The firmware downloads only the region needed for the pilot.

## Generator

Script:

```text
scripts/make_brazil_region_maps.py
```

It uses:

- IBGE state meshes for all Brazilian state boundaries.
- AWS Terrain Tiles HGT DEM for relief.
- The existing DEM-first hillshade pipeline from `make_relief_brmap.py`.
- Guia 4 Ventos as the first practical filter for states that have registered
  free-flight ramps.

## Plan regions

Create the regional plan without generating all binary maps:

```powershell
py scripts\make_brazil_region_maps.py `
  --output-root build\maps-brasil
```

Current default plan generates only states with ramps listed on Guia 4 Ventos.
States outside this first flight set remain visible in the firmware as
`EM BREVE`. Amazonas is not listed in the firmware for now because the user
asked to remove it.

```text
AL: 18   BA: 323  CE: 67   ES: 27   GO: 197
MA: 227  MG: 338  MS: 148  MT: 437  PB: 36
PE: 60   PR: 92   RJ: 35   RN: 23   RO: 133
RR: 135  RS: 155  SC: 49   SE: 14   SP: 167
TO: 155
Total flight-state plan: 2836 regions
```

Output:

```text
build/maps-brasil/catalog.plan.json
```

Then regenerate the compact firmware catalog:

```powershell
py scripts\make_brazil_catalog_header.py `
  --plan build\maps-brasil\catalog.plan.json `
  --output src\network\BrazilMapCatalog.h
```

## Build packages

Generate the first package as a smoke test:

```powershell
py scripts\make_brazil_region_maps.py `
  --states MG `
  --output-root build\maps-brasil `
  --build `
  --limit 1 `
  --preview
```

Generate all packages for GitHub publishing:

```powershell
py scripts\make_brazil_region_maps.py `
  --output-root build\maps-brasil `
  --build
```

This can take a long time. The script is resumable: if a `.brmap` already
exists, it is skipped unless `--force` is used.

Output layout:

```text
build/maps-brasil/
  catalog.json
  catalog.plan.json
  regions/
    mg_001.brmap
    mg_002.brmap
    ...
```

After building one or more states, recreate the publish catalog from the files
that actually exist:

```powershell
py scripts\make_brazil_publish_catalog.py `
  --plan build\maps-brasil\catalog.plan.json `
  --output-root build\maps-brasil
```

To publish only one state at a time, keep the full `catalog.plan.json` and run:

```powershell
py scripts\make_brazil_region_maps.py `
  --states MG `
  --output-root build\maps-brasil `
  --build

py scripts\make_brazil_publish_catalog.py `
  --plan build\maps-brasil\catalog.plan.json `
  --output-root build\maps-brasil
```

Copy the contents of `build/maps-brasil/` to the GitHub Pages repository root.

The firmware build currently uses:

```text
-DMAP_DOWNLOAD_BASE_URL="https://randalrenye.github.io/brvario-epaper/regions"
```

Keep all `.brmap` files inside the GitHub Pages `regions/` folder. With this
base URL, the firmware downloads files like:

```text
https://randalrenye.github.io/brvario-epaper/regions/mg_001.brmap
```

## Important firmware note

The firmware UI does not ask the pilot to download technical packages one by
one. The internal catalog is split into regional `.brmap` files, but the user
flow is:

```text
Brazil region -> state -> download full state
Brazil region -> state -> download current GPS area
```

Full-state downloads are queued package by package. The ESP32 downloads one
file at a time and keeps processing the main loop between chunks, avoiding a
long blocking operation. Bordering packages from neighbouring generated states
are added to the queue when they touch the selected state coverage, so flights
near state borders have terrain beyond the official boundary.
