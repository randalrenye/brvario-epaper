#!/usr/bin/env python3
"""Build BRVARIO regional map packages for Brazilian states.

This script prepares a GitHub Pages-friendly map repository layout:

  build/maps-brasil/
    catalog.json          # generated packages only
    catalog.plan.json     # all planned regions, useful before long builds
    regions/
      mg_001.brmap
      sp_001.brmap
      ...

The firmware should download preprocessed files from GitHub. The ESP32 must
not generate DEM, hillshade, or contours in the field.

Data sources:
- State boundaries: IBGE geographic mesh API.
- Relief DEM: AWS Terrain Tiles SRTM/HGT, reused by make_relief_brmap.py.
"""

from __future__ import annotations

import argparse
import gzip
import json
import math
import sys
import urllib.request
from dataclasses import dataclass
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
if str(SCRIPT_DIR) not in sys.path:
    sys.path.insert(0, str(SCRIPT_DIR))

from make_relief_brmap import (  # noqa: E402
    VERSION,
    GeoBounds,
    WaypointFeature,
    TYPE_WAYPOINT,
    contour_segments,
    downsample_grid,
    hillshade_4bpp,
    load_dem_source,
    raster_tone_counts,
    sample_dem,
    smooth_grid,
    write_brmap,
    write_raster_preview_bmp,
)


@dataclass(frozen=True)
class StateSpec:
    uf: str
    ibge_id: int
    name: str


@dataclass(frozen=True)
class RegionPackage:
    region_id: str
    state: StateSpec
    name: str
    center_lat: float
    center_lon: float
    bounds: GeoBounds
    radius_km: int


STATE_SPECS: dict[str, StateSpec] = {
    "AC": StateSpec("AC", 12, "Acre"),
    "AL": StateSpec("AL", 27, "Alagoas"),
    "AP": StateSpec("AP", 16, "Amapa"),
    "BA": StateSpec("BA", 29, "Bahia"),
    "CE": StateSpec("CE", 23, "Ceara"),
    "DF": StateSpec("DF", 53, "Distrito Federal"),
    "GO": StateSpec("GO", 52, "Goias"),
    "MA": StateSpec("MA", 21, "Maranhao"),
    "MG": StateSpec("MG", 31, "Minas Gerais"),
    "MS": StateSpec("MS", 50, "Mato Grosso do Sul"),
    "MT": StateSpec("MT", 51, "Mato Grosso"),
    "PA": StateSpec("PA", 15, "Para"),
    "PB": StateSpec("PB", 25, "Paraiba"),
    "PE": StateSpec("PE", 26, "Pernambuco"),
    "PI": StateSpec("PI", 22, "Piaui"),
    "PR": StateSpec("PR", 41, "Parana"),
    "ES": StateSpec("ES", 32, "Espirito Santo"),
    "RJ": StateSpec("RJ", 33, "Rio de Janeiro"),
    "RN": StateSpec("RN", 24, "Rio Grande do Norte"),
    "RO": StateSpec("RO", 11, "Rondonia"),
    "RR": StateSpec("RR", 14, "Roraima"),
    "RS": StateSpec("RS", 43, "Rio Grande do Sul"),
    "SC": StateSpec("SC", 42, "Santa Catarina"),
    "SE": StateSpec("SE", 28, "Sergipe"),
    "SP": StateSpec("SP", 35, "Sao Paulo"),
    "TO": StateSpec("TO", 17, "Tocantins"),
}

# Default publishing set based on states listed with free-flight ramps on
# Guia 4 Ventos. Other states can still be generated explicitly with --states.
DEFAULT_STATE_UFS = [
    "AL",
    "BA",
    "CE",
    "ES",
    "GO",
    "MA",
    "MG",
    "MS",
    "MT",
    "PB",
    "PE",
    "PR",
    "RJ",
    "RN",
    "RO",
    "RR",
    "RS",
    "SC",
    "SE",
    "SP",
    "TO",
]

IBGE_MESH_URL = (
    "https://servicodados.ibge.gov.br/api/v3/malhas/estados/"
    "{ibge_id}?formato=application/vnd.geo+json&qualidade=minima"
)


def km_to_lat_deg(km: float) -> float:
    return km / 111.32


def km_to_lon_deg(km: float, lat: float) -> float:
    return km / (111.32 * max(0.15, math.cos(math.radians(lat))))


def download_boundary(state: StateSpec, cache_dir: Path) -> Path:
    cache_dir.mkdir(parents=True, exist_ok=True)
    path = cache_dir / f"{state.uf.lower()}_ibge.geojson"
    if path.exists():
        return path

    url = IBGE_MESH_URL.format(ibge_id=state.ibge_id)
    print(f"Downloading IBGE boundary {state.uf}: {url}")
    with urllib.request.urlopen(url, timeout=60) as response:
        data = response.read()
    if data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    path.write_bytes(data)
    return path


def extract_polygons(geojson: dict) -> list[list[list[tuple[float, float]]]]:
    """Return polygons as [rings], where each ring is [(lon, lat), ...]."""
    polygons: list[list[list[tuple[float, float]]]] = []

    def add_geometry(geometry: dict) -> None:
        if not geometry:
            return
        geom_type = geometry.get("type")
        coords = geometry.get("coordinates")
        if geom_type == "Polygon":
            polygons.append([[tuple(map(float, point[:2])) for point in ring] for ring in coords])
        elif geom_type == "MultiPolygon":
            for polygon in coords:
                polygons.append([[tuple(map(float, point[:2])) for point in ring] for ring in polygon])
        elif geom_type == "GeometryCollection":
            for child in geometry.get("geometries", []):
                add_geometry(child)

    if geojson.get("type") == "FeatureCollection":
        for feature in geojson.get("features", []):
            add_geometry(feature.get("geometry") or {})
    elif geojson.get("type") == "Feature":
        add_geometry(geojson.get("geometry") or {})
    else:
        add_geometry(geojson)

    if not polygons:
        raise ValueError("IBGE GeoJSON did not contain Polygon/MultiPolygon geometry")
    return polygons


def ring_contains(lon: float, lat: float, ring: list[tuple[float, float]]) -> bool:
    inside = False
    if len(ring) < 3:
        return False
    j = len(ring) - 1
    for i, (xi, yi) in enumerate(ring):
        xj, yj = ring[j]
        crosses = (yi > lat) != (yj > lat)
        if crosses:
            x_at_lat = (xj - xi) * (lat - yi) / max(1e-12, yj - yi) + xi
            if lon < x_at_lat:
                inside = not inside
        j = i
    return inside


def polygon_contains(lon: float, lat: float, polygon: list[list[tuple[float, float]]]) -> bool:
    if not polygon or not ring_contains(lon, lat, polygon[0]):
        return False
    for hole in polygon[1:]:
        if ring_contains(lon, lat, hole):
            return False
    return True


def state_contains(polygons: list[list[list[tuple[float, float]]]], lon: float, lat: float) -> bool:
    return any(polygon_contains(lon, lat, polygon) for polygon in polygons)


def state_bbox(polygons: list[list[list[tuple[float, float]]]]) -> GeoBounds:
    lats: list[float] = []
    lons: list[float] = []
    for polygon in polygons:
        for ring in polygon:
            for lon, lat in ring:
                lats.append(lat)
                lons.append(lon)
    return GeoBounds(min(lats), max(lats), min(lons), max(lons))


def bbox_touches_state(bounds: GeoBounds, polygons: list[list[list[tuple[float, float]]]]) -> bool:
    center_lat = (bounds.lat_min + bounds.lat_max) * 0.5
    center_lon = (bounds.lon_min + bounds.lon_max) * 0.5
    test_points = [
        (center_lon, center_lat),
        (bounds.lon_min, bounds.lat_min),
        (bounds.lon_min, bounds.lat_max),
        (bounds.lon_max, bounds.lat_min),
        (bounds.lon_max, bounds.lat_max),
    ]
    if any(state_contains(polygons, lon, lat) for lon, lat in test_points):
        return True

    for polygon in polygons:
        for ring in polygon:
            for lon, lat in ring:
                if bounds.lon_min <= lon <= bounds.lon_max and bounds.lat_min <= lat <= bounds.lat_max:
                    return True
    return False


def bounds_from_center(center_lat: float, center_lon: float, width_km: float, height_km: float) -> GeoBounds:
    half_h = km_to_lat_deg(height_km * 0.5)
    half_w = km_to_lon_deg(width_km * 0.5, center_lat)
    return GeoBounds(center_lat - half_h, center_lat + half_h, center_lon - half_w, center_lon + half_w)


def planned_regions_for_state(
    state: StateSpec,
    polygons: list[list[list[tuple[float, float]]]],
    package_width_km: float,
    package_height_km: float,
    spacing_factor: float,
    radius_km: int,
) -> list[RegionPackage]:
    bbox = state_bbox(polygons)
    lat_step = km_to_lat_deg(package_height_km * spacing_factor)
    regions: list[RegionPackage] = []
    row = 0
    lat = bbox.lat_min + km_to_lat_deg(package_height_km * 0.5)
    while lat <= bbox.lat_max + 1e-9:
        lon_step = km_to_lon_deg(package_width_km * spacing_factor, lat)
        lon = bbox.lon_min + km_to_lon_deg(package_width_km * 0.5, lat)
        col = 0
        while lon <= bbox.lon_max + 1e-9:
            bounds = bounds_from_center(lat, lon, package_width_km, package_height_km)
            if bbox_touches_state(bounds, polygons):
                index = len(regions) + 1
                region_id = f"{state.uf.lower()}_{index:03d}"
                regions.append(
                    RegionPackage(
                        region_id=region_id,
                        state=state,
                        name=f"{state.name} {index:03d}",
                        center_lat=lat,
                        center_lon=lon,
                        bounds=bounds,
                        radius_km=radius_km,
                    )
                )
            lon += lon_step
            col += 1
        lat += lat_step
        row += 1
    return regions


def centered_region(
    state: StateSpec,
    region_id: str,
    center_lat: float,
    center_lon: float,
    package_width_km: float,
    package_height_km: float,
    radius_km: int,
) -> RegionPackage:
    return RegionPackage(
        region_id=region_id,
        state=state,
        name=f"{state.name} {region_id.upper()}",
        center_lat=center_lat,
        center_lon=center_lon,
        bounds=bounds_from_center(center_lat, center_lon, package_width_km, package_height_km),
        radius_km=radius_km,
    )


def catalog_entry(region: RegionPackage, package_path: Path | None, output_root: Path) -> dict:
    rel_file = f"regions/{region.region_id}.brmap"
    entry = {
        "id": region.region_id,
        "name": region.name,
        "state": region.state.uf,
        "file": rel_file,
        "size": package_path.stat().st_size if package_path and package_path.exists() else 0,
        "version": VERSION,
        "radiusKm": region.radius_km,
        "centerLat": round(region.center_lat, 6),
        "centerLon": round(region.center_lon, 6),
        "latMin": round(region.bounds.lat_min, 6),
        "latMax": round(region.bounds.lat_max, 6),
        "lonMin": round(region.bounds.lon_min, 6),
        "lonMax": round(region.bounds.lon_max, 6),
    }
    if package_path and package_path.exists():
        entry["file"] = package_path.relative_to(output_root).as_posix()
    return entry


def build_package(region: RegionPackage, args: argparse.Namespace) -> tuple[Path, list[int], int]:
    output_path = args.output_root / "regions" / f"{region.region_id}.brmap"
    preview_path = args.output_root / "preview" / f"{region.region_id}.bmp" if args.preview else None
    if output_path.exists() and not args.force:
        print(f"Skipping existing {output_path}")
        counts = [0] * 16
        return output_path, counts, 0

    dem_source = load_dem_source(region.bounds, args.dem_cache)
    raw_grid = sample_dem(dem_source, region.bounds, args.width, args.height)
    raw_dem = raw_grid.values
    contour_dem = smooth_grid(raw_dem, args.contour_dem_smooth)

    raster = hillshade_4bpp(
        raw_dem,
        region.bounds.lat_min,
        region.bounds.lat_max,
        region.bounds.lon_min,
        region.bounds.lon_max,
        args.shade_min,
        args.shade_max,
        args.shade_contrast,
        args.shade_white_bias,
        args.shade_depth,
        args.shade_shadow_gamma,
        args.shade_dither_strength,
        args.shade_dither_cell,
        args.shade_sun_azimuth,
        args.shade_sun_altitude,
        args.shade_low_smooth,
        args.shade_mid_smooth,
        args.shade_fine_smooth,
        args.shade_low_weight,
        args.shade_mid_weight,
        args.shade_fine_weight,
        args.shade_stretch_low,
        args.shade_stretch_high,
        args.shade_ridge_strength,
        args.shade_valley_strength,
        args.shade_slope_boost,
        args.shade_curvature_smooth,
        args.shade_display_smooth,
        args.elevation_shade_strength,
        args.elevation_shade_gamma,
        args.elevation_shade_smooth,
    )

    contour_grid = downsample_grid(contour_dem, args.contour_downsample)
    contours = contour_segments(
        contour_grid,
        region.bounds.lat_min,
        region.bounds.lat_max,
        region.bounds.lon_min,
        region.bounds.lon_max,
        args.contour_interval,
        args.index_contour_interval,
        args.contour_min_step,
        args.contour_min_points,
        args.contour_smooth,
    )

    waypoints = [WaypointFeature(TYPE_WAYPOINT, region.center_lat, region.center_lon, "Centro")]
    write_brmap(
        output_path,
        region.region_id.upper(),
        region.bounds.lat_min,
        region.bounds.lat_max,
        region.bounds.lon_min,
        region.bounds.lon_max,
        args.width,
        args.height,
        raster,
        contours,
        waypoints,
    )
    if preview_path:
        write_raster_preview_bmp(preview_path, args.width, args.height, raster)

    return output_path, raster_tone_counts(args.width, args.height, raster), len(contours)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate BRVARIO regional maps for Brazilian free-flight states")
    parser.add_argument("--states", nargs="+", default=DEFAULT_STATE_UFS, choices=sorted(STATE_SPECS))
    parser.add_argument("--output-root", type=Path, default=Path("build/maps-brasil"))
    parser.add_argument("--boundary-cache", type=Path, default=Path("build/boundary-cache"))
    parser.add_argument("--dem-cache", type=Path, default=Path("build/dem-cache"))
    parser.add_argument("--build", action="store_true", help="Generate .brmap files. Without this, only catalog.plan.json is written.")
    parser.add_argument("--force", action="store_true", help="Rebuild packages that already exist.")
    parser.add_argument("--limit", type=int, default=0, help="Build only the first N planned packages; 0 means no limit.")
    parser.add_argument("--preview", action="store_true", help="Write BMP raster previews beside generated packages.")
    parser.add_argument("--only-id", help="Build only one planned package id, for example mg_243.")
    parser.add_argument("--center-lat", type=float, help="Generate one custom package centered at this latitude.")
    parser.add_argument("--center-lon", type=float, help="Generate one custom package centered at this longitude.")
    parser.add_argument("--region-id", help="Custom package id used with --center-lat/--center-lon.")

    parser.add_argument("--region-radius-km", type=int, default=35)
    parser.add_argument("--package-width-km", type=float, default=35.0)
    parser.add_argument("--package-height-km", type=float, default=20.5)
    parser.add_argument("--spacing-factor", type=float, default=0.70)
    parser.add_argument("--width", type=int, default=720)
    parser.add_argument("--height", type=int, default=420)

    parser.add_argument("--contour-interval", type=int, default=10)
    parser.add_argument("--index-contour-interval", type=int, default=50)
    parser.add_argument("--contour-downsample", type=int, default=4)
    parser.add_argument("--contour-min-step", type=float, default=12.0)
    parser.add_argument("--contour-min-points", type=int, default=4)
    parser.add_argument("--contour-smooth", type=int, default=1)
    parser.add_argument("--contour-dem-smooth", type=int, default=1)

    parser.add_argument("--shade-min", type=int, default=13)
    parser.add_argument("--shade-max", type=int, default=15)
    parser.add_argument("--shade-contrast", type=float, default=0.0)
    parser.add_argument("--shade-white-bias", type=float, default=0.5)
    parser.add_argument("--shade-depth", type=float, default=3.4)
    parser.add_argument("--shade-shadow-gamma", type=float, default=1.0)
    parser.add_argument("--shade-dither-strength", type=float, default=0.0)
    parser.add_argument("--shade-dither-cell", type=int, default=16)
    parser.add_argument("--shade-display-smooth", type=int, default=2)
    parser.add_argument("--shade-sun-azimuth", type=float, default=315.0)
    parser.add_argument("--shade-sun-altitude", type=float, default=40.0)
    parser.add_argument("--shade-low-smooth", type=int, default=34)
    parser.add_argument("--shade-mid-smooth", type=int, default=14)
    parser.add_argument("--shade-fine-smooth", type=int, default=5)
    parser.add_argument("--shade-low-weight", type=float, default=0.80)
    parser.add_argument("--shade-mid-weight", type=float, default=0.17)
    parser.add_argument("--shade-fine-weight", type=float, default=0.03)
    parser.add_argument("--shade-stretch-low", type=float, default=4.0)
    parser.add_argument("--shade-stretch-high", type=float, default=99.2)
    parser.add_argument("--shade-ridge-strength", type=float, default=0.0)
    parser.add_argument("--shade-valley-strength", type=float, default=0.0)
    parser.add_argument("--shade-slope-boost", type=float, default=0.0)
    parser.add_argument("--shade-curvature-smooth", type=int, default=34)
    parser.add_argument("--elevation-shade-strength", type=float, default=0.56)
    parser.add_argument("--elevation-shade-gamma", type=float, default=3.0)
    parser.add_argument("--elevation-shade-smooth", type=int, default=18)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    args.output_root.mkdir(parents=True, exist_ok=True)
    (args.output_root / "regions").mkdir(parents=True, exist_ok=True)

    planned: list[RegionPackage] = []
    if args.center_lat is not None or args.center_lon is not None:
        if args.center_lat is None or args.center_lon is None:
            raise SystemExit("--center-lat and --center-lon must be used together")
        state = STATE_SPECS[args.states[0]]
        region_id = args.region_id or f"{state.uf.lower()}_custom"
        planned.append(centered_region(state, region_id, args.center_lat, args.center_lon, args.package_width_km, args.package_height_km, args.region_radius_km))
        print(f"{state.uf}: 1 centered package {region_id}")
    else:
        for uf in args.states:
            state = STATE_SPECS[uf]
            boundary_path = download_boundary(state, args.boundary_cache)
            polygons = extract_polygons(json.loads(boundary_path.read_text(encoding="utf-8")))
            regions = planned_regions_for_state(
                state,
                polygons,
                args.package_width_km,
                args.package_height_km,
                args.spacing_factor,
                args.region_radius_km,
            )
            planned.extend(regions)
            print(f"{state.uf}: {len(regions)} planned regional packages")

    if args.only_id:
        planned = [region for region in planned if region.region_id == args.only_id]
        if not planned:
            raise SystemExit(f"Package id not found in plan: {args.only_id}")

    plan_entries = [catalog_entry(region, None, args.output_root) for region in planned]
    plan_path = args.output_root / "catalog.plan.json"
    plan_path.write_text(json.dumps(plan_entries, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(f"Wrote plan {plan_path} ({len(plan_entries)} regions)")

    if not args.build:
        print("Catalog plan only. Re-run with --build to generate .brmap files.")
        return

    built_entries: list[dict] = []
    build_count = 0
    for region in planned:
        if args.limit and build_count >= args.limit:
            break
        output_path, tone_counts, contour_count = build_package(region, args)
        built_entries.append(catalog_entry(region, output_path, args.output_root))
        build_count += 1
        total = max(1, args.width * args.height)
        tones = ", ".join(f"{tone}:{count * 100.0 / total:.1f}%" for tone, count in enumerate(tone_counts) if count)
        print(f"{region.region_id}: {output_path.stat().st_size} bytes, {contour_count} contours, tones [{tones}]")

    catalog_path = args.output_root / "catalog.json"
    catalog_path.write_text(json.dumps(built_entries, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(f"Wrote catalog {catalog_path} ({len(built_entries)} generated packages)")


if __name__ == "__main__":
    main()
