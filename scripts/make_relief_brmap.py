#!/usr/bin/env python3
"""Build a DEM-first topographic BRVARIO .brmap package.

Pipeline used by this script:

DEM HGT tiles -> DEM crop/resample -> DEM smoothing -> slope/aspect
-> physical hillshade -> light ambient occlusion -> e-paper quantization
-> contour vectors -> firmware-ready .brmap v2 package.

Contour lines are never used to create shadows. They are generated only after
the raster relief exists and are stored as a separate vector layer.
"""

from __future__ import annotations

import argparse
import gzip
import json
import math
import struct
import urllib.request
from dataclasses import dataclass
from pathlib import Path


MAGIC = 0x50414D42
VERSION = 2
HEADER_STRUCT = "<IHHiiiiII32s16s"
RASTER_STRUCT = "<HHiiiiIB7s"
LINE_STRUCT = "<BBHiiii"
POINT_STRUCT = "<ii"
WAYPOINT_STRUCT = "<Bii24s"

TYPE_CONTOUR = 1
TYPE_CONTOUR_INDEX = 8
TYPE_RAMP = 6
TYPE_WAYPOINT = 7
MAX_LINE_POINTS = 96


# ---------------------------------------------------------------------------
# Binary output records
# ---------------------------------------------------------------------------


@dataclass
class DemTile:
    lat_floor: int
    lon_floor: int
    size: int
    elevations: list[int]


@dataclass
class LineFeature:
    feature_type: int
    points: list[tuple[float, float]]


@dataclass
class WaypointFeature:
    feature_type: int
    lat: float
    lon: float
    name: str


@dataclass(frozen=True)
class GeoBounds:
    lat_min: float
    lat_max: float
    lon_min: float
    lon_max: float


@dataclass
class DemGrid:
    values: list[list[float]]
    bounds: GeoBounds

    @property
    def width(self) -> int:
        return len(self.values[0])

    @property
    def height(self) -> int:
        return len(self.values)


@dataclass
class DemSource:
    tiles: dict[tuple[int, int], DemTile]

    def sample(self, lat: float, lon: float) -> float:
        lat_floor = math.floor(lat)
        lon_floor = math.floor(lon)
        tile = self.tiles.get((lat_floor, lon_floor))
        if tile is None:
            return 0.0
        return tile_elevation(tile, lat, lon)


def coord_to_e7(value: float) -> int:
    return int(round(value * 10_000_000))


def fixed_ascii(text: str, size: int) -> bytes:
    data = text.encode("ascii", "ignore")[: size - 1]
    return data + bytes(size - len(data))


def tile_name(lat_floor: int, lon_floor: int) -> tuple[str, str]:
    lat_prefix = "N" if lat_floor >= 0 else "S"
    lon_prefix = "E" if lon_floor >= 0 else "W"
    lat_abs = abs(lat_floor)
    lon_abs = abs(lon_floor)
    folder = f"{lat_prefix}{lat_abs:02d}"
    name = f"{lat_prefix}{lat_abs:02d}{lon_prefix}{lon_abs:03d}"
    return folder, name


def download_hgt(lat_floor: int, lon_floor: int, cache_dir: Path) -> Path:
    folder, name = tile_name(lat_floor, lon_floor)
    cache_dir.mkdir(parents=True, exist_ok=True)
    hgt_path = cache_dir / f"{name}.hgt"
    if hgt_path.exists():
        return hgt_path

    url = f"https://s3.amazonaws.com/elevation-tiles-prod/skadi/{folder}/{name}.hgt.gz"
    gz_path = cache_dir / f"{name}.hgt.gz"
    print(f"Downloading {url}")
    urllib.request.urlretrieve(url, gz_path)
    with gzip.open(gz_path, "rb") as src:
        hgt_path.write_bytes(src.read())
    return hgt_path


def read_hgt(path: Path, lat_floor: int, lon_floor: int) -> DemTile:
    data = path.read_bytes()
    samples = int(math.sqrt(len(data) // 2))
    if samples * samples * 2 != len(data):
      raise ValueError(f"Unexpected HGT size for {path}: {len(data)} bytes")
    values = list(struct.unpack(f">{samples * samples}h", data))
    return DemTile(lat_floor, lon_floor, samples, values)


def load_dem_source(bounds: GeoBounds, cache_dir: Path) -> DemSource:
    """Download/read every HGT tile touched by the requested bounds."""
    tiles: dict[tuple[int, int], DemTile] = {}
    for lat_floor in range(math.floor(bounds.lat_min), math.floor(bounds.lat_max) + 1):
        for lon_floor in range(math.floor(bounds.lon_min), math.floor(bounds.lon_max) + 1):
            hgt_path = download_hgt(lat_floor, lon_floor, cache_dir)
            tiles[(lat_floor, lon_floor)] = read_hgt(hgt_path, lat_floor, lon_floor)
    return DemSource(tiles)


def tile_elevation(tile: DemTile, lat: float, lon: float) -> float:
    """Bilinear HGT sampling avoids stair-step artifacts in slope/aspect."""
    row_f = (tile.lat_floor + 1.0 - lat) * (tile.size - 1)
    col_f = (lon - tile.lon_floor) * (tile.size - 1)
    row_f = max(0.0, min(float(tile.size - 1), row_f))
    col_f = max(0.0, min(float(tile.size - 1), col_f))
    row0 = max(0, min(tile.size - 1, int(math.floor(row_f))))
    col0 = max(0, min(tile.size - 1, int(math.floor(col_f))))
    row1 = min(tile.size - 1, row0 + 1)
    col1 = min(tile.size - 1, col0 + 1)
    fy = row_f - float(row0)
    fx = col_f - float(col0)

    weighted_total = 0.0
    weight_total = 0.0
    for row, wy in ((row0, 1.0 - fy), (row1, fy)):
        for col, wx in ((col0, 1.0 - fx), (col1, fx)):
            weight = wx * wy
            value = tile.elevations[row * tile.size + col]
            if value > -32768 and weight > 0.0:
                weighted_total += float(value) * weight
                weight_total += weight
    return weighted_total / weight_total if weight_total > 0.0 else 0.0


def sample_dem(source: DemSource, bounds: GeoBounds, width: int, height: int) -> DemGrid:
    """Resample the DEM to the exact raster size used by the ESP32 package."""
    grid: list[list[float]] = []
    for y in range(height):
        lat = bounds.lat_max - (bounds.lat_max - bounds.lat_min) * y / max(1, height - 1)
        row: list[float] = []
        for x in range(width):
            lon = bounds.lon_min + (bounds.lon_max - bounds.lon_min) * x / max(1, width - 1)
            row.append(source.sample(lat, lon))
        grid.append(row)
    return DemGrid(grid, bounds)


def smooth_grid(grid: list[list[float]], passes: int) -> list[list[float]]:
    height = len(grid)
    width = len(grid[0])
    current = grid
    for _ in range(passes):
        nxt = [[0.0] * width for _ in range(height)]
        for y in range(height):
            for x in range(width):
                total = 0.0
                count = 0
                for yy in range(max(0, y - 1), min(height, y + 2)):
                    for xx in range(max(0, x - 1), min(width, x + 2)):
                        weight = 2.0 if xx == x and yy == y else 1.0
                        total += current[yy][xx] * weight
                        count += int(weight)
                nxt[y][x] = total / count
        current = nxt
    return current


def percentile(values: list[float], pct: float) -> float:
    if not values:
        return 0.0
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, int(round((len(ordered) - 1) * pct / 100.0))))
    return ordered[index]


def stable_noise01(x: int, y: int) -> float:
    value = (x * 374761393 + y * 668265263) & 0xFFFFFFFF
    value = (value ^ (value >> 13)) * 1274126177
    value = value ^ (value >> 16)
    return (value & 0xFFFF) / 65535.0


BAYER_8X8 = (
    0, 48, 12, 60, 3, 51, 15, 63,
    32, 16, 44, 28, 35, 19, 47, 31,
    8, 56, 4, 52, 11, 59, 7, 55,
    40, 24, 36, 20, 43, 27, 39, 23,
    2, 50, 14, 62, 1, 49, 13, 61,
    34, 18, 46, 30, 33, 17, 45, 29,
    10, 58, 6, 54, 9, 57, 5, 53,
    42, 26, 38, 22, 41, 25, 37, 21,
)


def bayer_matrix(size: int) -> list[list[int]]:
    if size <= 2:
        return [[0, 2], [3, 1]]
    half = bayer_matrix(size // 2)
    result = [[0] * size for _ in range(size)]
    for y in range(size // 2):
        for x in range(size // 2):
            base = half[y][x] * 4
            result[y][x] = base
            result[y][x + size // 2] = base + 2
            result[y + size // 2][x] = base + 3
            result[y + size // 2][x + size // 2] = base + 1
    return result


BAYER_CACHE: dict[int, list[list[int]]] = {}


def ordered_dither01(x: int, y: int, cell_size: int) -> float:
    """Ordered dithering tuned for the final e-paper raster size.

    Larger cells avoid tiny random speckles that disappear on the physical
    display and turn into dirty-looking patches.
    """
    size = max(2, min(32, cell_size))
    if size & (size - 1):
        size = 16
    matrix = BAYER_CACHE.get(size)
    if matrix is None:
        matrix = bayer_matrix(size)
        BAYER_CACHE[size] = matrix
    return (matrix[y % size][x % size] + 0.5) / float(size * size)


# ---------------------------------------------------------------------------
# DEM-first hillshade
# ---------------------------------------------------------------------------


def grid_spacing_m(lat_min: float, lat_max: float, lon_min: float, lon_max: float, width: int, height: int) -> tuple[float, float]:
    lat_mid = (lat_min + lat_max) * 0.5
    dx = abs(lon_max - lon_min) * 111_320.0 * max(0.15, math.cos(math.radians(lat_mid))) / max(1, width - 1)
    dy = abs(lat_max - lat_min) * 111_320.0 / max(1, height - 1)
    return dx, dy


def horn_gradient(grid: list[list[float]], x: int, y: int, dx: float, dy: float) -> tuple[float, float]:
    """Horn 3x3 DEM gradient in east/north meters.

    This is the same family of derivative used by common GIS hillshade tools.
    It reads only DEM elevations; contour density is not involved.
    """
    height = len(grid)
    width = len(grid[0])
    xm = max(0, x - 1)
    xp = min(width - 1, x + 1)
    ym = max(0, y - 1)
    yp = min(height - 1, y + 1)

    z1 = grid[ym][xm]
    z2 = grid[ym][x]
    z3 = grid[ym][xp]
    z4 = grid[y][xm]
    z6 = grid[y][xp]
    z7 = grid[yp][xm]
    z8 = grid[yp][x]
    z9 = grid[yp][xp]

    east = ((z3 + 2.0 * z6 + z9) - (z1 + 2.0 * z4 + z7)) / max(8.0 * dx, 1.0)
    north = ((z1 + 2.0 * z2 + z3) - (z7 + 2.0 * z8 + z9)) / max(8.0 * dy, 1.0)
    return east, north


def hillshade_layer(
    grid: list[list[float]],
    lat_min: float,
    lat_max: float,
    lon_min: float,
    lon_max: float,
    sun_azimuth_deg: float,
    sun_altitude_deg: float,
    vertical_scale: float,
) -> list[list[float]]:
    """Compute one physical hillshade layer from DEM slope/aspect."""
    height = len(grid)
    width = len(grid[0])
    dx, dy = grid_spacing_m(lat_min, lat_max, lon_min, lon_max, width, height)
    sun_azimuth = math.radians(sun_azimuth_deg)
    sun_altitude = math.radians(sun_altitude_deg)
    sin_altitude = math.sin(sun_altitude)
    cos_altitude = math.cos(sun_altitude)

    shades: list[list[float]] = []
    for y in range(height):
        row: list[float] = []
        for x in range(width):
            dz_east, dz_north = horn_gradient(grid, x, y, dx, dy)
            dz_east *= vertical_scale
            dz_north *= vertical_scale
            slope = math.atan(math.hypot(dz_east, dz_north))
            aspect = math.atan2(-dz_east, -dz_north)
            # Classic GIS hillshade: DEM -> slope/aspect -> light angle.
            shade = sin_altitude * math.cos(slope) + cos_altitude * math.sin(slope) * math.cos(sun_azimuth - aspect)
            shade = max(0.0, min(1.0, shade))
            row.append(shade)
        shades.append(row)
    return shades


def slope_layer(grid: list[list[float]], lat_min: float, lat_max: float, lon_min: float, lon_max: float) -> list[list[float]]:
    height = len(grid)
    width = len(grid[0])
    dx, dy = grid_spacing_m(lat_min, lat_max, lon_min, lon_max, width, height)

    slopes: list[list[float]] = []
    for y in range(height):
        row: list[float] = []
        for x in range(width):
            dz_east, dz_north = horn_gradient(grid, x, y, dx, dy)
            row.append(math.hypot(dz_east, dz_north))
        slopes.append(row)
    return slopes


def normalize_positive(grid: list[list[float]], high_pct: float) -> list[list[float]]:
    values = [max(0.0, value) for row in grid for value in row]
    hi = max(0.001, percentile(values, high_pct))
    return [[max(0.0, min(1.0, value / hi)) for value in row] for row in grid]


def ambient_occlusion_layer(
    grid: list[list[float]],
    occlusion_smooth: int,
) -> list[list[float]]:
    """Light DEM-based occlusion for closed valleys and reentrants.

    The signal is topographic position: pixels lower than their broader
    surrounding terrain receive a gentle shadow. This is intentionally subtle
    so it does not create dark disconnected blobs.
    """
    broad = smooth_grid(grid, occlusion_smooth)
    mid = smooth_grid(grid, max(2, occlusion_smooth // 3))
    height = len(grid)
    width = len(grid[0])

    occlusion_raw: list[list[float]] = []
    for y in range(height):
        occlusion_row: list[float] = []
        for x in range(width):
            occlusion_row.append(max(0.0, broad[y][x] - mid[y][x]))
        occlusion_raw.append(occlusion_row)

    return smooth_grid(normalize_positive(occlusion_raw, 98.5), max(2, occlusion_smooth // 7))


def slope_intensity_layer(
    grid: list[list[float]],
    lat_min: float,
    lat_max: float,
    lon_min: float,
    lon_max: float,
    smooth_passes: int,
) -> list[list[float]]:
    """Normalized DEM slope used only for a small contrast boost."""
    slope_source = smooth_grid(grid, smooth_passes)
    return smooth_grid(normalize_positive(slope_layer(slope_source, lat_min, lat_max, lon_min, lon_max), 97.5), 2)


def elevation_intensity_layer(grid: list[list[float]], smooth_passes: int, low_pct: float, high_pct: float) -> list[list[float]]:
    """Normalize relative elevation so higher terrain can receive subtle tone."""
    source = smooth_grid(grid, smooth_passes) if smooth_passes > 0 else grid
    flat = [value for row in source for value in row]
    lo = percentile(flat, low_pct)
    hi = percentile(flat, high_pct)
    span = max(0.001, hi - lo)
    return [[max(0.0, min(1.0, (value - lo) / span)) for value in row] for row in source]


def multiscale_hillshade(
    grid: list[list[float]],
    lat_min: float,
    lat_max: float,
    lon_min: float,
    lon_max: float,
    sun_azimuth_deg: float,
    sun_altitude_deg: float,
    low_smooth: int,
    mid_smooth: int,
    fine_smooth: int,
    low_weight: float,
    mid_weight: float,
    fine_weight: float,
) -> list[list[float]]:
    """Blend macro/mid/fine physical hillshade layers.

    Macro has the highest weight so the eye reads mountains and valleys before
    seeing any fine texture.
    """
    low = hillshade_layer(smooth_grid(grid, low_smooth), lat_min, lat_max, lon_min, lon_max, sun_azimuth_deg, sun_altitude_deg, 2.20)
    mid = hillshade_layer(smooth_grid(grid, mid_smooth), lat_min, lat_max, lon_min, lon_max, sun_azimuth_deg, sun_altitude_deg, 1.55)
    fine = hillshade_layer(smooth_grid(grid, fine_smooth), lat_min, lat_max, lon_min, lon_max, sun_azimuth_deg, sun_altitude_deg, 0.80)
    total_weight = max(0.001, low_weight + mid_weight + fine_weight)

    height = len(grid)
    width = len(grid[0])
    blended: list[list[float]] = []
    for y in range(height):
        row: list[float] = []
        for x in range(width):
            row.append((low[y][x] * low_weight + mid[y][x] * mid_weight + fine[y][x] * fine_weight) / total_weight)
        blended.append(row)
    return blended


def hillshade_4bpp(
    grid: list[list[float]],
    lat_min: float,
    lat_max: float,
    lon_min: float,
    lon_max: float,
    shade_min: int,
    shade_max: int,
    contrast: float,
    white_bias: float,
    shade_depth: float,
    shadow_gamma: float,
    dither_strength: float,
    dither_cell: int,
    sun_azimuth_deg: float,
    sun_altitude_deg: float,
    low_smooth: int,
    mid_smooth: int,
    fine_smooth: int,
    low_weight: float,
    mid_weight: float,
    fine_weight: float,
    stretch_low_pct: float,
    stretch_high_pct: float,
    ridge_strength: float,
    valley_strength: float,
    slope_boost: float,
    curvature_smooth: int,
    display_smooth: int,
    elevation_shade_strength: float,
    elevation_shade_gamma: float,
    elevation_smooth: int,
) -> bytes:
    height = len(grid)
    width = len(grid[0])

    blended = multiscale_hillshade(grid,
                                   lat_min,
                                   lat_max,
                                   lon_min,
                                   lon_max,
                                   sun_azimuth_deg,
                                   sun_altitude_deg,
                                   low_smooth,
                                   mid_smooth,
                                   fine_smooth,
                                   low_weight,
                                   mid_weight,
                                   fine_weight)
    if display_smooth > 0:
        # Final-size smoothing: suppress detail smaller than the useful e-paper
        # pixel footprint while keeping the macro light/shadow model intact.
        blended = smooth_grid(blended, display_smooth)
    flat = [shade for row in blended for shade in row]

    lo = percentile(flat, stretch_low_pct)
    hi = percentile(flat, stretch_high_pct)
    span = max(0.001, hi - lo)
    occlusion = ambient_occlusion_layer(grid, curvature_smooth)
    slopes = slope_intensity_layer(grid, lat_min, lat_max, lon_min, lon_max, max(1, curvature_smooth // 4))
    elevation = elevation_intensity_layer(grid, elevation_smooth, 3.0, 98.0)

    pixels: list[int] = []
    for y in range(height):
        for x in range(width):
            shade = (blended[y][x] - lo) / span
            shade = max(0.0, min(1.0, shade))
            shade = 0.5 + (shade - 0.5) * contrast
            shade += white_bias
            shade = max(0.0, min(1.0, shade))
            shade = 0.5 + (shade - 0.5) * (1.0 + slopes[y][x] * slope_boost)
            shade += ridge_strength * max(0.0, shade - 0.62)
            shade -= occlusion[y][x] * valley_strength * (0.35 + (1.0 - shade) * 0.65)
            if elevation_shade_strength > 0.0:
                topo = elevation[y][x]
                if elevation_shade_gamma > 0.0 and elevation_shade_gamma != 1.0:
                    topo = pow(topo, elevation_shade_gamma)
                shade -= topo * elevation_shade_strength
            shade = max(0.0, min(1.0, shade))

            shadow = 1.0 - shade
            if shadow_gamma > 0.0 and shadow_gamma != 1.0:
                shadow = pow(shadow, shadow_gamma)

            value_f = shade_max - shadow * shade_depth
            value_f = max(float(shade_min), min(float(shade_max), value_f))

            if dither_strength > 0.0:
                shadow_mix = max(0.0, min(1.0, shadow * (1.0 - shadow) * 4.0))
                value_f += (ordered_dither01(x, y, dither_cell) - 0.5) * dither_strength * shadow_mix
            value = int(round(value_f))
            pixels.append(max(3, min(15, value)))

    packed = bytearray((width * height + 1) // 2)
    for i, value in enumerate(pixels):
        if i & 1:
            packed[i // 2] |= value << 4
        else:
            packed[i // 2] |= value
    return bytes(packed)


def white_raster_4bpp(width: int, height: int) -> bytes:
    return bytes([0xFF] * ((width * height + 1) // 2))


def contour_segments(grid: list[list[float]],
                     lat_min: float,
                     lat_max: float,
                     lon_min: float,
                     lon_max: float,
                     interval_m: int,
                     index_interval_m: int,
                     min_step_m: float,
                     min_chain_points: int,
                     smooth_passes: int) -> list[LineFeature]:
    height = len(grid)
    width = len(grid[0])
    z_min = min(min(row) for row in grid)
    z_max = max(max(row) for row in grid)
    first = int(math.floor(z_min / interval_m) * interval_m)
    last = int(math.ceil(z_max / interval_m) * interval_m)
    segments_by_level: dict[int, list[tuple[tuple[float, float], tuple[float, float]]]] = {}

    def point_for(x: int, y: int, edge: int, level: float) -> tuple[float, float]:
        z00 = grid[y][x]
        z10 = grid[y][x + 1]
        z11 = grid[y + 1][x + 1]
        z01 = grid[y + 1][x]
        if edge == 0:
            t = 0.5 if z10 == z00 else (level - z00) / (z10 - z00)
            fx, fy = x + t, y
        elif edge == 1:
            t = 0.5 if z11 == z10 else (level - z10) / (z11 - z10)
            fx, fy = x + 1, y + t
        elif edge == 2:
            t = 0.5 if z11 == z01 else (level - z01) / (z11 - z01)
            fx, fy = x + t, y + 1
        else:
            t = 0.5 if z01 == z00 else (level - z00) / (z01 - z00)
            fx, fy = x, y + t
        lat = lat_max - (lat_max - lat_min) * fy / max(1, height - 1)
        lon = lon_min + (lon_max - lon_min) * fx / max(1, width - 1)
        return lat, lon

    cases = {
        1: [(3, 0)], 2: [(0, 1)], 3: [(3, 1)],
        4: [(1, 2)], 5: [(3, 0), (1, 2)], 6: [(0, 2)], 7: [(3, 2)],
        8: [(2, 3)], 9: [(0, 2)], 10: [(0, 1), (2, 3)], 11: [(1, 2)],
        12: [(1, 3)], 13: [(0, 1)], 14: [(3, 0)],
    }

    for level in range(first, last + interval_m, interval_m):
        if level <= z_min or level >= z_max:
            continue
        for y in range(height - 1):
            for x in range(width - 1):
                z00 = grid[y][x]
                z10 = grid[y][x + 1]
                z11 = grid[y + 1][x + 1]
                z01 = grid[y + 1][x]
                case = 0
                if z00 >= level: case |= 1
                if z10 >= level: case |= 2
                if z11 >= level: case |= 4
                if z01 >= level: case |= 8
                for edge_a, edge_b in cases.get(case, []):
                    a = point_for(x, y, edge_a, float(level))
                    b = point_for(x, y, edge_b, float(level))
                    segments_by_level.setdefault(level, []).append((a, b))
    return join_contour_segments(segments_by_level, index_interval_m, min_step_m, min_chain_points, smooth_passes)


def contour_key(point: tuple[float, float]) -> tuple[int, int]:
    return (int(round(point[0] * 1_000_000)), int(round(point[1] * 1_000_000)))


def reverse_line(line: list[tuple[float, float]]) -> list[tuple[float, float]]:
    return list(reversed(line))


def smooth_polyline(points: list[tuple[float, float]], passes: int) -> list[tuple[float, float]]:
    if passes <= 0 or len(points) < 3:
        return points

    result = points
    for _ in range(passes):
        smoothed: list[tuple[float, float]] = [result[0]]
        for a, b in zip(result, result[1:]):
            smoothed.append((a[0] * 0.75 + b[0] * 0.25, a[1] * 0.75 + b[1] * 0.25))
            smoothed.append((a[0] * 0.25 + b[0] * 0.75, a[1] * 0.25 + b[1] * 0.75))
        smoothed.append(result[-1])
        result = smoothed
    return result


def simplify_polyline(points: list[tuple[float, float]], min_step_m: float) -> list[tuple[float, float]]:
    if len(points) <= 2:
        return points
    result = [points[0]]
    lat0 = sum(lat for lat, _ in points) / len(points)
    meters_per_lon = 111_320.0 * max(0.15, math.cos(math.radians(lat0)))
    for point in points[1:-1]:
        prev = result[-1]
        north = (point[0] - prev[0]) * 111_320.0
        east = (point[1] - prev[1]) * meters_per_lon
        if math.hypot(north, east) >= min_step_m:
            result.append(point)
    result.append(points[-1])
    return result


def split_polyline(points: list[tuple[float, float]], feature_type: int, min_step_m: float, smooth_passes: int) -> list[LineFeature]:
    points = simplify_polyline(points, min_step_m)
    points = smooth_polyline(points, smooth_passes)
    points = simplify_polyline(points, max(1.0, min_step_m * 0.5))
    if len(points) < 2:
        return []
    features: list[LineFeature] = []
    start = 0
    while start < len(points) - 1:
        end = min(start + MAX_LINE_POINTS, len(points))
        chunk = points[start:end]
        if len(chunk) >= 2:
            features.append(LineFeature(feature_type, chunk))
        start = end - 1
    return features


def join_contour_segments(segments_by_level: dict[int, list[tuple[tuple[float, float], tuple[float, float]]]],
                          index_interval_m: int,
                          min_step_m: float,
                          min_chain_points: int,
                          smooth_passes: int) -> list[LineFeature]:
    features: list[LineFeature] = []
    for level, segments in segments_by_level.items():
        chains: list[list[tuple[float, float]]] = []
        starts: dict[tuple[int, int], int] = {}
        ends: dict[tuple[int, int], int] = {}

        def rebuild_indexes() -> None:
            starts.clear()
            ends.clear()
            for idx, chain in enumerate(chains):
                starts[contour_key(chain[0])] = idx
                ends[contour_key(chain[-1])] = idx

        for a, b in segments:
            ka = contour_key(a)
            kb = contour_key(b)
            end_idx = ends.get(ka)
            start_idx = starts.get(kb)
            start_a_idx = starts.get(ka)
            end_b_idx = ends.get(kb)

            if end_idx is not None and start_idx is not None and end_idx != start_idx:
                chains[end_idx].extend(chains[start_idx][1:])
                chains.pop(start_idx)
                rebuild_indexes()
            elif end_idx is not None:
                chains[end_idx].append(b)
                rebuild_indexes()
            elif start_idx is not None:
                chains[start_idx].insert(0, a)
                rebuild_indexes()
            elif start_a_idx is not None:
                chains[start_a_idx].insert(0, b)
                rebuild_indexes()
            elif end_b_idx is not None:
                chains[end_b_idx].append(a)
                rebuild_indexes()
            else:
                chains.append([a, b])
                starts[ka] = len(chains) - 1
                ends[kb] = len(chains) - 1

        feature_type = TYPE_CONTOUR_INDEX if index_interval_m > 0 and level % index_interval_m == 0 else TYPE_CONTOUR
        for chain in chains:
            if len(chain) >= min_chain_points:
                features.extend(split_polyline(chain, feature_type, min_step_m, smooth_passes))
    return features


def downsample_grid(grid: list[list[float]], factor: int) -> list[list[float]]:
    if factor <= 1:
        return grid
    height = len(grid)
    width = len(grid[0])
    out_height = max(2, (height + factor - 1) // factor)
    out_width = max(2, (width + factor - 1) // factor)
    result: list[list[float]] = []
    for oy in range(out_height):
        row: list[float] = []
        y0 = min(height - 1, oy * factor)
        y1 = min(height, y0 + factor)
        for ox in range(out_width):
            x0 = min(width - 1, ox * factor)
            x1 = min(width, x0 + factor)
            total = 0.0
            count = 0
            for y in range(y0, y1):
                for x in range(x0, x1):
                    total += grid[y][x]
                    count += 1
            row.append(total / max(1, count))
        result.append(row)
    return result


def write_brmap(path: Path, name: str, lat_min: float, lat_max: float, lon_min: float, lon_max: float, raster_width: int, raster_height: int, raster: bytes, lines: list[LineFeature], waypoints: list[WaypointFeature]) -> None:
    header_size = struct.calcsize(HEADER_STRUCT) + struct.calcsize(RASTER_STRUCT)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as output:
        output.write(struct.pack(
            HEADER_STRUCT,
            MAGIC, VERSION, header_size,
            coord_to_e7(lat_min), coord_to_e7(lat_max), coord_to_e7(lon_min), coord_to_e7(lon_max),
            len(lines), len(waypoints), fixed_ascii(name, 32), bytes(16)
        ))
        output.write(struct.pack(
            RASTER_STRUCT,
            raster_width, raster_height,
            coord_to_e7(lat_min), coord_to_e7(lat_max), coord_to_e7(lon_min), coord_to_e7(lon_max),
            len(raster), 1, bytes(7)
        ))
        output.write(raster)

        for line in lines:
            lat_e7 = [coord_to_e7(lat) for lat, _ in line.points]
            lon_e7 = [coord_to_e7(lon) for _, lon in line.points]
            output.write(struct.pack(LINE_STRUCT, line.feature_type, 0, len(line.points), min(lat_e7), max(lat_e7), min(lon_e7), max(lon_e7)))
            for lat, lon in line.points:
                output.write(struct.pack(POINT_STRUCT, coord_to_e7(lat), coord_to_e7(lon)))

        for waypoint in waypoints:
            output.write(struct.pack(WAYPOINT_STRUCT, waypoint.feature_type, coord_to_e7(waypoint.lat), coord_to_e7(waypoint.lon), fixed_ascii(waypoint.name, 24)))


def write_catalog(path: Path, package_path: Path, region_id: str, display_name: str, lat_min: float, lat_max: float, lon_min: float, lon_max: float) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    entry = {
        "id": region_id,
        "name": display_name,
        "file": f"regions/{package_path.name}",
        "size": package_path.stat().st_size,
        "version": VERSION,
        "latMin": lat_min,
        "latMax": lat_max,
        "lonMin": lon_min,
        "lonMax": lon_max,
    }
    path.write_text(json.dumps([entry], indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def write_raster_preview_bmp(path: Path, width: int, height: int, raster: bytes) -> None:
    """Write a desktop preview of the 4-bit relief raster."""
    path.parent.mkdir(parents=True, exist_ok=True)
    pixels: list[int] = []
    for packed in raster:
        pixels.append(packed & 0x0F)
        if len(pixels) < width * height:
            pixels.append((packed >> 4) & 0x0F)
    pixels = pixels[: width * height]

    row_stride = ((width * 3 + 3) // 4) * 4
    file_size = 54 + row_stride * height
    bmp = bytearray()
    bmp += b"BM"
    bmp += struct.pack("<IHHI", file_size, 0, 0, 54)
    bmp += struct.pack("<IIIHHIIIIII", 40, width, height, 1, 24, 0, row_stride * height, 2835, 2835, 0, 0)
    for y in range(height - 1, -1, -1):
        row = bytearray()
        base = y * width
        for x in range(width):
            gray = int(round(pixels[base + x] * 255 / 15))
            row += bytes((gray, gray, gray))
        row += bytes(row_stride - len(row))
        bmp += row
    path.write_bytes(bmp)


def raster_tone_counts(width: int, height: int, raster: bytes) -> list[int]:
    counts = [0] * 16
    pixel_count = width * height
    seen = 0
    for packed in raster:
        counts[packed & 0x0F] += 1
        seen += 1
        if seen >= pixel_count:
            break
        counts[(packed >> 4) & 0x0F] += 1
        seen += 1
    return counts


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build relief .brmap from AWS Terrain Tiles HGT")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--name", required=True)
    parser.add_argument("--region-id", required=True)
    parser.add_argument("--display-name", required=True)
    parser.add_argument("--lat-min", type=float, required=True)
    parser.add_argument("--lat-max", type=float, required=True)
    parser.add_argument("--lon-min", type=float, required=True)
    parser.add_argument("--lon-max", type=float, required=True)
    parser.add_argument("--width", type=int, default=640)
    parser.add_argument("--height", type=int, default=360)
    parser.add_argument("--contour-interval", type=int, default=50)
    parser.add_argument("--contour-downsample", type=int, default=3)
    parser.add_argument("--index-contour-interval", type=int, default=100)
    parser.add_argument("--contour-min-step", type=float, default=5.0)
    parser.add_argument("--contour-min-points", type=int, default=3)
    parser.add_argument("--contour-smooth", type=int, default=1)
    parser.add_argument("--shade-min", type=int, default=11)
    parser.add_argument("--shade-max", type=int, default=15)
    parser.add_argument("--shade-contrast", type=float, default=1.0)
    parser.add_argument("--shade-white-bias", type=float, default=0.08)
    parser.add_argument("--shade-depth", type=float, default=4.7)
    parser.add_argument("--shade-shadow-gamma", type=float, default=1.0)
    parser.add_argument("--shade-dither-strength", type=float, default=0.16)
    parser.add_argument("--shade-dither-cell", type=int, default=16)
    parser.add_argument("--shade-display-smooth", type=int, default=1)
    parser.add_argument("--shade-sun-azimuth", type=float, default=315.0)
    parser.add_argument("--shade-sun-altitude", type=float, default=40.0)
    parser.add_argument("--shade-low-smooth", type=int, default=26)
    parser.add_argument("--shade-mid-smooth", type=int, default=10)
    parser.add_argument("--shade-fine-smooth", type=int, default=4)
    parser.add_argument("--shade-low-weight", type=float, default=0.78)
    parser.add_argument("--shade-mid-weight", type=float, default=0.18)
    parser.add_argument("--shade-fine-weight", type=float, default=0.04)
    parser.add_argument("--shade-stretch-low", type=float, default=1.5)
    parser.add_argument("--shade-stretch-high", type=float, default=98.5)
    parser.add_argument("--shade-ridge-strength", type=float, default=0.0)
    parser.add_argument("--shade-valley-strength", type=float, default=0.08)
    parser.add_argument("--shade-slope-boost", type=float, default=0.04)
    parser.add_argument("--shade-curvature-smooth", type=int, default=26)
    parser.add_argument("--elevation-shade-strength", type=float, default=0.0)
    parser.add_argument("--elevation-shade-gamma", type=float, default=1.6)
    parser.add_argument("--elevation-shade-smooth", type=int, default=12)
    parser.add_argument("--white-background", action="store_true", help="Generate a pure white raster and use contours as the terrain layer")
    parser.add_argument("--cache-dir", type=Path, default=Path("build/dem-cache"))
    parser.add_argument("--catalog", type=Path)
    parser.add_argument("--preview", type=Path)
    parser.add_argument("--ramp-lat", type=float)
    parser.add_argument("--ramp-lon", type=float)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    bounds = GeoBounds(args.lat_min, args.lat_max, args.lon_min, args.lon_max)
    dem_source = load_dem_source(bounds, args.cache_dir)
    raw_dem_grid = sample_dem(dem_source, bounds, args.width, args.height)
    raw_dem = raw_dem_grid.values
    dem = smooth_grid(raw_dem, 1)
    if args.white_background:
        raster = white_raster_4bpp(args.width, args.height)
    else:
        raster = hillshade_4bpp(raw_dem,
                                args.lat_min,
                                args.lat_max,
                                args.lon_min,
                                args.lon_max,
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
                                args.elevation_shade_smooth)
    contour_grid = downsample_grid(dem, args.contour_downsample)
    contours = contour_segments(contour_grid,
                                args.lat_min,
                                args.lat_max,
                                args.lon_min,
                                args.lon_max,
                                args.contour_interval,
                                args.index_contour_interval,
                                args.contour_min_step,
                                args.contour_min_points,
                                args.contour_smooth)
    waypoints: list[WaypointFeature] = []
    if args.ramp_lat is not None and args.ramp_lon is not None:
        waypoints.append(WaypointFeature(TYPE_RAMP, args.ramp_lat, args.ramp_lon, "Rampa"))
    waypoints.append(WaypointFeature(TYPE_WAYPOINT, (args.lat_min + args.lat_max) * 0.5, (args.lon_min + args.lon_max) * 0.5, "Centro"))
    write_brmap(args.output, args.name, args.lat_min, args.lat_max, args.lon_min, args.lon_max, args.width, args.height, raster, contours, waypoints)
    if args.catalog:
        write_catalog(args.catalog, args.output, args.region_id, args.display_name, args.lat_min, args.lat_max, args.lon_min, args.lon_max)
    if args.preview:
        write_raster_preview_bmp(args.preview, args.width, args.height, raster)

    print(f"Wrote {args.output} ({args.output.stat().st_size} bytes, {len(contours)} contour segments, raster {args.width}x{args.height})")
    counts = raster_tone_counts(args.width, args.height, raster)
    total = max(1, args.width * args.height)
    print("Tone distribution:")
    for tone, count in enumerate(counts):
        if count:
            print(f"  {tone}: {count * 100.0 / total:.1f}%")


if __name__ == "__main__":
    main()
