#!/usr/bin/env python3
"""Build a BRVARIO .brmap package from a small GeoJSON file.

This first generator is intentionally dependency-free. It accepts a
FeatureCollection with LineString/MultiLineString and Point geometries, then
writes the binary format consumed by the ESP32 firmware.
"""

from __future__ import annotations

import argparse
import json
import math
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


MAGIC = 0x50414D42  # "BMAP" in the little-endian file.
VERSION = 1
MAX_LINE_POINTS = 96

FEATURE_TYPES = {
    "contour": 1,
    "river": 2,
    "road": 3,
    "trail": 4,
    "city": 5,
    "ramp": 6,
    "waypoint": 7,
}

LINE_TYPES = {1, 2, 3, 4}
POINT_TYPES = {5, 6, 7}


@dataclass
class LineFeature:
    feature_type: int
    points: list[tuple[float, float]]  # lat, lon


@dataclass
class WaypointFeature:
    feature_type: int
    lat: float
    lon: float
    name: str


def coord_to_e7(value: float) -> int:
    return int(round(value * 10_000_000))


def fixed_ascii(text: str, size: int) -> bytes:
    data = text.encode("ascii", "ignore")[: size - 1]
    return data + bytes(size - len(data))


def feature_type(properties: dict) -> int:
    raw = str(properties.get("type") or properties.get("brvario:type") or "").strip().lower()
    if raw not in FEATURE_TYPES:
        raise ValueError(f"Unknown feature type: {raw!r}")
    return FEATURE_TYPES[raw]


def point_name(properties: dict) -> str:
    return str(properties.get("name") or properties.get("label") or "")[:23]


def simplify_line(points: list[tuple[float, float]], tolerance_m: float) -> list[tuple[float, float]]:
    if len(points) <= 2 or tolerance_m <= 0:
        return points

    lat0 = sum(lat for lat, _ in points) / len(points)
    meters_per_lat = 111_320.0
    meters_per_lon = max(0.15, math.cos(math.radians(lat0))) * meters_per_lat

    def to_xy(point: tuple[float, float]) -> tuple[float, float]:
        lat, lon = point
        return lon * meters_per_lon, lat * meters_per_lat

    def distance_to_segment(point: tuple[float, float], start: tuple[float, float], end: tuple[float, float]) -> float:
        px, py = to_xy(point)
        ax, ay = to_xy(start)
        bx, by = to_xy(end)
        dx = bx - ax
        dy = by - ay
        if dx == 0 and dy == 0:
            return math.hypot(px - ax, py - ay)
        t = max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
        cx = ax + t * dx
        cy = ay + t * dy
        return math.hypot(px - cx, py - cy)

    def rdp(slice_points: list[tuple[float, float]]) -> list[tuple[float, float]]:
        if len(slice_points) <= 2:
            return slice_points
        start = slice_points[0]
        end = slice_points[-1]
        max_distance = -1.0
        index = 0
        for i, point in enumerate(slice_points[1:-1], start=1):
            distance = distance_to_segment(point, start, end)
            if distance > max_distance:
                max_distance = distance
                index = i
        if max_distance <= tolerance_m:
            return [start, end]
        left = rdp(slice_points[: index + 1])
        right = rdp(slice_points[index:])
        return left[:-1] + right

    return rdp(points)


def chunk_line(points: list[tuple[float, float]]) -> Iterable[list[tuple[float, float]]]:
    if len(points) <= MAX_LINE_POINTS:
        yield points
        return
    start = 0
    while start < len(points) - 1:
        end = min(start + MAX_LINE_POINTS, len(points))
        yield points[start:end]
        start = end - 1


def load_geojson(path: Path, simplify_m: float) -> tuple[list[LineFeature], list[WaypointFeature]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("type") != "FeatureCollection":
        raise ValueError("Input must be a GeoJSON FeatureCollection")

    lines: list[LineFeature] = []
    waypoints: list[WaypointFeature] = []

    for feature in data.get("features", []):
        properties = feature.get("properties") or {}
        geometry = feature.get("geometry") or {}
        kind = feature_type(properties)
        geom_type = geometry.get("type")
        coordinates = geometry.get("coordinates")

        if kind in LINE_TYPES:
            if geom_type == "LineString":
                line_strings = [coordinates]
            elif geom_type == "MultiLineString":
                line_strings = coordinates
            else:
                raise ValueError(f"Line feature requires LineString/MultiLineString, got {geom_type}")

            for line_string in line_strings:
                points = [(float(lat), float(lon)) for lon, lat in line_string]
                points = simplify_line(points, simplify_m)
                if len(points) >= 2:
                    for chunk in chunk_line(points):
                        if len(chunk) >= 2:
                            lines.append(LineFeature(kind, chunk))
        elif kind in POINT_TYPES:
            if geom_type != "Point":
                raise ValueError(f"Point feature requires Point geometry, got {geom_type}")
            lon, lat = coordinates
            waypoints.append(WaypointFeature(kind, float(lat), float(lon), point_name(properties)))

    return lines, waypoints


def all_points(lines: list[LineFeature], waypoints: list[WaypointFeature]) -> list[tuple[float, float]]:
    points: list[tuple[float, float]] = []
    for line in lines:
        points.extend(line.points)
    points.extend((point.lat, point.lon) for point in waypoints)
    return points


def write_brmap(path: Path, name: str, lines: list[LineFeature], waypoints: list[WaypointFeature]) -> None:
    points = all_points(lines, waypoints)
    if not points:
        raise ValueError("No map features found")

    lat_values = [lat for lat, _ in points]
    lon_values = [lon for _, lon in points]
    header_size = struct.calcsize("<IHHiiiiII32s16s")

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as output:
        output.write(
            struct.pack(
                "<IHHiiiiII32s16s",
                MAGIC,
                VERSION,
                header_size,
                coord_to_e7(min(lat_values)),
                coord_to_e7(max(lat_values)),
                coord_to_e7(min(lon_values)),
                coord_to_e7(max(lon_values)),
                len(lines),
                len(waypoints),
                fixed_ascii(name, 32),
                bytes(16),
            )
        )

        for line in lines:
            lat_e7 = [coord_to_e7(lat) for lat, _ in line.points]
            lon_e7 = [coord_to_e7(lon) for _, lon in line.points]
            output.write(
                struct.pack(
                    "<BBHiiii",
                    line.feature_type,
                    0,
                    len(line.points),
                    min(lat_e7),
                    max(lat_e7),
                    min(lon_e7),
                    max(lon_e7),
                )
            )
            for lat, lon in line.points:
                output.write(struct.pack("<ii", coord_to_e7(lat), coord_to_e7(lon)))

        for waypoint in waypoints:
            output.write(
                struct.pack(
                    "<Bii24s",
                    waypoint.feature_type,
                    coord_to_e7(waypoint.lat),
                    coord_to_e7(waypoint.lon),
                    fixed_ascii(waypoint.name, 24),
                )
            )


def write_catalog(path: Path, package_path: Path, package_name: str, region_id: str, display_name: str, lines: list[LineFeature], waypoints: list[WaypointFeature]) -> None:
    points = all_points(lines, waypoints)
    lat_values = [lat for lat, _ in points]
    lon_values = [lon for _, lon in points]
    entry = {
        "id": region_id,
        "name": display_name,
        "file": f"regions/{package_path.name}",
        "size": package_path.stat().st_size,
        "version": VERSION,
        "latMin": min(lat_values),
        "latMax": max(lat_values),
        "lonMin": min(lon_values),
        "lonMax": max(lon_values),
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps([entry], indent=2, ensure_ascii=True) + "\n", encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build a BRVARIO .brmap package from GeoJSON")
    parser.add_argument("--input", required=True, type=Path, help="Input GeoJSON FeatureCollection")
    parser.add_argument("--output", required=True, type=Path, help="Output .brmap file")
    parser.add_argument("--name", required=True, help="Package name stored in the header")
    parser.add_argument("--region-id", default=None, help="Region id for optional catalog.json")
    parser.add_argument("--display-name", default=None, help="Display name for optional catalog.json")
    parser.add_argument("--catalog", type=Path, help="Optional catalog.json output")
    parser.add_argument("--simplify-m", type=float, default=8.0, help="Ramer-Douglas-Peucker tolerance in meters")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    lines, waypoints = load_geojson(args.input, args.simplify_m)
    write_brmap(args.output, args.name, lines, waypoints)
    if args.catalog:
        region_id = args.region_id or args.output.stem
        display_name = args.display_name or args.name
        write_catalog(args.catalog, args.output, args.name, region_id, display_name, lines, waypoints)

    size = args.output.stat().st_size
    print(f"Wrote {args.output} ({size} bytes, {len(lines)} lines, {len(waypoints)} waypoints)")


if __name__ == "__main__":
    main()
