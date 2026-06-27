# BRVARIO offline map package

Initial firmware format: `.brmap` (`.brvario` is also accepted by the reader).

The package is a little-endian binary stream optimized for sequential reads from microSD. The ESP32 does not load the full map into RAM; it scans records and renders only features whose bounding box intersects the current screen window.

## Header

Magic: `BMAP`

Version: `1`

Version `2` adds an optional 4-bit relief raster before the vector records. This is the preferred style for free-flight maps: shaded terrain first, simplified contours and waypoints above it.

All coordinates are signed `int32` degrees multiplied by `1e7`.

```text
uint32 magic
uint16 version
uint16 headerSize
int32  latMinE7
int32  latMaxE7
int32  lonMinE7
int32  lonMaxE7
uint32 lineCount
uint32 waypointCount
char   name[32]
uint8  reserved[16]
```

## Relief raster extension, version 2

When `version == 2`, the header is followed by:

```text
uint16 width
uint16 height
int32  latMinE7
int32  latMaxE7
int32  lonMinE7
int32  lonMaxE7
uint32 dataSize
uint8  format     // 1 = packed 4bpp grayscale, two pixels per byte
uint8  reserved[7]
```

The raster bytes come immediately after this extension. Vector line records begin after `headerSize + dataSize`.

## Line records

Each line record is followed immediately by `pointCount` point records.

```text
uint8  type       // 1 contour, 2 river, 3 road, 4 trail
uint8  flags      // reserved
uint16 pointCount // firmware currently keeps up to 96 points per line segment
int32  latMinE7
int32  latMaxE7
int32  lonMinE7
int32  lonMaxE7

repeat pointCount:
  int32 latE7
  int32 lonE7
```

## Waypoint records

```text
uint8 type        // 5 city, 6 ramp, 7 waypoint
int32 latE7
int32 lonE7
char  name[24]
```

## Data sources

Recommended generation pipeline:

- OpenStreetMap extracts from Geofabrik for roads, trails, rivers, cities, ramps, and named places.
- SRTM, TOPODATA/INPE, Copernicus DEM, or OpenTopography DEM for contours.
- OpenTopoMap only as a visual reference, not as a direct tile source.

Server-side simplification should happen before packaging: remove minor details, simplify polylines per zoom/use case, split long lines into small records, and keep labels sparse for e-paper.

## Experimental generator

The first dependency-free generator is available at:

```text
scripts/make_brmap.py
```

Example:

```powershell
py scripts\make_brmap.py `
  --input docs\maps\palmopolis_experimental.geojson `
  --output build\maps\regions\palmopolis.brmap `
  --name PALMOPOLIS `
  --region-id palmopolis `
  --display-name "Palmopolis Experimental" `
  --catalog build\maps\catalog.json `
  --simplify-m 4
```

Generated publishable layout:

```text
build/maps/
  catalog.json
  regions/
    palmopolis.brmap
```

For a GitHub Pages map repository, copy the contents of `build/maps/` to the root of the published repository so the firmware can later fetch:

```text
https://usuario.github.io/brvario-maps/catalog.json
https://usuario.github.io/brvario-maps/regions/palmopolis.brmap
```

## Relief generator

For terrain maps like the XTracer/topographic references, use:

```text
scripts/make_relief_brmap.py
```

It downloads public HGT DEM tiles from AWS Terrain Tiles, creates a grayscale hillshade layer, extracts simplified contour segments, and writes a `.brmap` version 2 package.

Example used for the Palmopolis local e-paper relief test:

```powershell
py scripts\make_relief_brmap.py `
  --output build\maps\regions\palmopolis.brmap `
  --name PALMO-REL-EINK-LOCAL `
  --region-id palmopolis `
  --display-name "Palmopolis Relief E-Ink Local" `
  --lat-min -16.765 `
  --lat-max -16.710 `
  --lon-min -40.480 `
  --lon-max -40.390 `
  --width 720 `
  --height 420 `
  --contour-interval 30 `
  --contour-downsample 3 `
  --index-contour-interval 120 `
  --contour-min-step 5 `
  --contour-min-points 3 `
  --contour-smooth 1 `
  --shade-min 10 `
  --shade-max 15 `
  --shade-contrast 1.18 `
  --shade-white-bias 0.04 `
  --shade-depth 5.0 `
  --shade-shadow-gamma 1.00 `
  --shade-dither-strength 0.08 `
  --shade-dither-cell 8 `
  --shade-display-smooth 1 `
  --shade-sun-azimuth 315 `
  --shade-sun-altitude 40 `
  --shade-low-smooth 28 `
  --shade-mid-smooth 10 `
  --shade-fine-smooth 3 `
  --shade-low-weight 0.70 `
  --shade-mid-weight 0.23 `
  --shade-fine-weight 0.07 `
  --shade-stretch-low 2.0 `
  --shade-stretch-high 98.0 `
  --shade-ridge-strength 0.03 `
  --shade-valley-strength 0.08 `
  --shade-slope-boost 0.08 `
  --shade-curvature-smooth 28 `
  --catalog build\maps\catalog.json `
  --preview build\maps\palmopolis_rel_eink_local_preview.bmp `
  --ramp-lat -16.7360 `
  --ramp-lon -40.4290
```
