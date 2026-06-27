#!/usr/bin/env python3
"""Validate BRVARIO flight-site records and build the compact ESP32 catalog."""

from __future__ import annotations

import argparse
import datetime as dt
import json
from pathlib import Path
from typing import Any


VALID_QUADRANTS = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"}
MAX_SITES = 512


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build the BRVARIO flight-site catalog")
    parser.add_argument("--sites-dir", type=Path, default=Path("weather/sites"))
    parser.add_argument("--output", type=Path, default=Path("weather/catalog.json"))
    parser.add_argument("--catalog-version", type=int, default=1)
    parser.add_argument("--updated-at", default=dt.date.today().isoformat())
    return parser.parse_args()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def load_site(path: Path) -> dict[str, Any]:
    site = json.loads(path.read_text(encoding="utf-8"))
    require(site.get("schema") == "brvario.flight-site", f"{path}: invalid schema")
    require(site.get("schemaVersion") == 1, f"{path}: unsupported schemaVersion")

    site_id = site.get("id")
    require(isinstance(site_id, int) and 1 <= site_id <= 65535, f"{path}: invalid id")
    require(path.stem == f"{site_id:05d}", f"{path}: filename must match id as five digits")

    name = site.get("name")
    city = site.get("city")
    state = site.get("state")
    require(isinstance(name, str) and 0 < len(name) <= 39, f"{path}: invalid name")
    require(isinstance(city, str) and 0 < len(city) <= 27, f"{path}: invalid city")
    require(isinstance(state, str) and len(state) == 2 and state.isupper(), f"{path}: invalid state")

    coordinates = site.get("coordinates", {})
    latitude_e7 = coordinates.get("latitudeE7")
    longitude_e7 = coordinates.get("longitudeE7")
    require(isinstance(latitude_e7, int) and -900000000 <= latitude_e7 <= 900000000, f"{path}: invalid latitudeE7")
    require(
        isinstance(longitude_e7, int) and -1800000000 <= longitude_e7 <= 1800000000,
        f"{path}: invalid longitudeE7",
    )
    require(latitude_e7 != 0 or longitude_e7 != 0, f"{path}: zero coordinate is not accepted")

    altitude_m = site.get("terrain", {}).get("altitudeM")
    require(isinstance(altitude_m, int) and -500 <= altitude_m <= 9000, f"{path}: invalid altitudeM")

    quadrants = site.get("windQuadrants")
    require(isinstance(quadrants, list), f"{path}: windQuadrants must be an array")
    require(len(quadrants) == len(set(quadrants)), f"{path}: duplicate wind quadrant")
    require(set(quadrants).issubset(VALID_QUADRANTS), f"{path}: invalid wind quadrant")

    provenance = site.get("provenance", {})
    require(bool(provenance.get("dataOwner")), f"{path}: provenance.dataOwner is required")
    require(bool(provenance.get("license")), f"{path}: provenance.license is required")
    require(bool(provenance.get("lastVerified")), f"{path}: provenance.lastVerified is required")
    return site


def compact_entry(site: dict[str, Any], detail_path: str) -> dict[str, Any]:
    provenance = site["provenance"]
    entry = {
        "id": site["id"],
        "name": site["name"],
        "city": site["city"],
        "state": site["state"],
        "latitudeE7": site["coordinates"]["latitudeE7"],
        "longitudeE7": site["coordinates"]["longitudeE7"],
        "altitudeM": site["terrain"]["altitudeM"],
        "windQuadrants": site["windQuadrants"],
        "detail": detail_path,
        "source": provenance["dataOwner"],
    }
    vertical_drop_m = site["terrain"].get("verticalDropM")
    if isinstance(vertical_drop_m, int):
        entry["verticalDropM"] = vertical_drop_m
    return entry


def write_catalog(path: Path, version: int, updated_at: str, entries: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as output:
        output.write("{\n")
        output.write('  "schema": "brvario.flight-sites.catalog",\n')
        output.write('  "schemaVersion": 1,\n')
        output.write(f'  "catalogVersion": {version},\n')
        output.write(f'  "updatedAt": {json.dumps(updated_at)},\n')
        output.write(f'  "siteCount": {len(entries)},\n')
        output.write('  "sites": [\n')
        for index, entry in enumerate(entries):
            suffix = "," if index + 1 < len(entries) else ""
            output.write("    " + json.dumps(entry, ensure_ascii=True, separators=(",", ":")) + suffix + "\n")
        output.write("  ]\n")
        output.write("}\n")


def main() -> None:
    args = parse_args()
    require(args.catalog_version > 0, "catalog version must be positive")
    site_paths = sorted(args.sites_dir.glob("*.json"))
    require(site_paths, f"no site files found in {args.sites_dir}")
    require(len(site_paths) <= MAX_SITES, f"catalog exceeds {MAX_SITES} sites")

    entries: list[dict[str, Any]] = []
    seen_ids: set[int] = set()
    seen_slugs: set[str] = set()
    for path in site_paths:
        site = load_site(path)
        require(site["id"] not in seen_ids, f"{path}: duplicate id {site['id']}")
        require(site["slug"] not in seen_slugs, f"{path}: duplicate slug {site['slug']}")
        seen_ids.add(site["id"])
        seen_slugs.add(site["slug"])
        detail_path = f"sites/{path.name}"
        entries.append(compact_entry(site, detail_path))

    entries.sort(key=lambda item: (item["state"], item["city"], item["name"], item["id"]))
    write_catalog(args.output, args.catalog_version, args.updated_at, entries)
    print(f"Wrote {args.output} with {len(entries)} validated flight sites")


if __name__ == "__main__":
    main()
