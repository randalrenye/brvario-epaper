#!/usr/bin/env python3
"""Validate files published to BRVARIO devices through GitHub Pages."""

from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path, PurePosixPath
from typing import Any
from urllib.parse import urlparse


ROOT = Path(__file__).resolve().parents[1]
MAP_MAGIC = b"BMAP"
MAP_REQUIRED_FIELDS = {
    "id",
    "name",
    "state",
    "file",
    "size",
    "version",
    "radiusKm",
    "centerLat",
    "centerLon",
    "latMin",
    "latMax",
    "lonMin",
    "lonMax",
}
SITE_REQUIRED_FIELDS = {
    "id",
    "name",
    "city",
    "state",
    "latitudeE7",
    "longitudeE7",
    "altitudeM",
    "windQuadrants",
    "detail",
    "source",
}


class Validation:
    def __init__(self) -> None:
        self.errors: list[str] = []

    def require(self, condition: bool, message: str) -> None:
        if not condition:
            self.errors.append(message)


def load_json(path: Path, validation: Validation) -> Any:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as exc:
        validation.errors.append(f"{path.relative_to(ROOT)}: JSON inválido: {exc}")
        return None


def safe_relative_path(value: str) -> bool:
    path = PurePosixPath(value)
    return not path.is_absolute() and ".." not in path.parts and "\\" not in value


def validate_maps(validation: Validation) -> int:
    canonical = load_json(ROOT / "regions" / "catalog.json", validation)
    legacy = load_json(ROOT / "catalog.json", validation)
    if not isinstance(canonical, list) or not isinstance(legacy, list):
        return 0

    validation.require(len(canonical) > 0, "regions/catalog.json está vazio")
    validation.require(
        len(canonical) == len(legacy),
        "catalog.json legado possui quantidade diferente do catálogo canônico",
    )

    ids: set[str] = set()
    files: set[str] = set()
    for index, entry in enumerate(canonical):
        label = f"regions/catalog.json[{index}]"
        if not isinstance(entry, dict):
            validation.errors.append(f"{label}: entrada deve ser um objeto")
            continue

        missing = MAP_REQUIRED_FIELDS - entry.keys()
        validation.require(not missing, f"{label}: campos ausentes: {sorted(missing)}")
        if missing:
            continue

        region_id = entry["id"]
        file_name = entry["file"]
        validation.require(
            isinstance(region_id, str) and re.fullmatch(r"[a-z]{2}_[0-9]{3,5}", region_id) is not None,
            f"{label}: id inválido: {region_id!r}",
        )
        validation.require(region_id not in ids, f"{label}: id duplicado: {region_id}")
        ids.add(region_id)

        validation.require(
            isinstance(file_name, str)
            and re.fullmatch(r"[a-z]{2}_[0-9]{3,5}\.brmap", file_name) is not None,
            f"{label}: nome de arquivo inválido: {file_name!r}",
        )
        validation.require(file_name not in files, f"{label}: arquivo duplicado: {file_name}")
        files.add(file_name)
        validation.require(file_name == f"{region_id}.brmap", f"{label}: id e arquivo divergem")

        state = entry["state"]
        validation.require(
            isinstance(state, str) and re.fullmatch(r"[A-Z]{2}", state) is not None,
            f"{label}: UF inválida: {state!r}",
        )
        validation.require(entry["latMin"] < entry["latMax"], f"{label}: latitude inválida")
        validation.require(entry["lonMin"] < entry["lonMax"], f"{label}: longitude inválida")

        map_path = ROOT / "regions" / file_name
        if not map_path.is_file():
            validation.errors.append(f"{label}: mapa ausente: regions/{file_name}")
        else:
            actual_size = map_path.stat().st_size
            validation.require(actual_size == entry["size"], f"{label}: tamanho incorreto")
            with map_path.open("rb") as stream:
                validation.require(stream.read(4) == MAP_MAGIC, f"{label}: cabeçalho BMAP inválido")

        if index < len(legacy) and isinstance(legacy[index], dict):
            expected_legacy = dict(entry)
            expected_legacy["file"] = f"regions/{file_name}"
            validation.require(
                legacy[index] == expected_legacy,
                f"catalog.json[{index}]: catálogo legado diverge de {region_id}",
            )

    disk_files = {path.name for path in (ROOT / "regions").glob("*.brmap")}
    for orphan in sorted(disk_files - files):
        validation.errors.append(f"regions/{orphan}: mapa não listado no catálogo")
    return len(canonical)


def validate_weather(validation: Validation) -> int:
    catalog = load_json(ROOT / "weather" / "catalog.json", validation)
    if not isinstance(catalog, dict):
        return 0

    sites = catalog.get("sites")
    validation.require(catalog.get("schema") == "brvario.flight-sites.catalog", "schema de rampas inválido")
    validation.require(catalog.get("schemaVersion") == 1, "schemaVersion de rampas inválido")
    validation.require(isinstance(sites, list), "weather/catalog.json: sites deve ser uma lista")
    if not isinstance(sites, list):
        return 0
    validation.require(catalog.get("siteCount") == len(sites), "siteCount não corresponde à lista de rampas")

    ids: set[int] = set()
    detail_files: set[str] = set()
    for index, site in enumerate(sites):
        label = f"weather/catalog.json.sites[{index}]"
        if not isinstance(site, dict):
            validation.errors.append(f"{label}: entrada deve ser um objeto")
            continue
        missing = SITE_REQUIRED_FIELDS - site.keys()
        validation.require(not missing, f"{label}: campos ausentes: {sorted(missing)}")
        if missing:
            continue

        site_id = site["id"]
        detail_name = site["detail"]
        validation.require(isinstance(site_id, int) and 1 <= site_id <= 65535, f"{label}: id inválido")
        validation.require(site_id not in ids, f"{label}: id duplicado: {site_id}")
        ids.add(site_id)
        validation.require(
            isinstance(detail_name, str) and safe_relative_path(detail_name),
            f"{label}: caminho de detalhe inválido",
        )
        detail_files.add(detail_name)

        detail_path = ROOT / "weather" / detail_name
        detail = load_json(detail_path, validation) if detail_path.is_file() else None
        if detail is None:
            validation.require(detail_path.is_file(), f"{label}: detalhe ausente: {detail_name}")
            continue
        validation.require(detail.get("schema") == "brvario.flight-site", f"{detail_name}: schema inválido")
        validation.require(detail.get("id") == site_id, f"{detail_name}: id diverge do catálogo")
        validation.require(detail.get("name") == site["name"], f"{detail_name}: nome diverge do catálogo")
        validation.require(detail.get("city") == site["city"], f"{detail_name}: cidade diverge do catálogo")
        validation.require(detail.get("state") == site["state"], f"{detail_name}: UF diverge do catálogo")
        coordinates = detail.get("coordinates", {})
        validation.require(coordinates.get("latitudeE7") == site["latitudeE7"], f"{detail_name}: latitude diverge")
        validation.require(coordinates.get("longitudeE7") == site["longitudeE7"], f"{detail_name}: longitude diverge")

    disk_details = {
        path.relative_to(ROOT / "weather").as_posix()
        for path in (ROOT / "weather" / "sites").glob("*.json")
    }
    for orphan in sorted(disk_details - detail_files):
        validation.errors.append(f"weather/{orphan}: detalhe não listado no catálogo")
    return len(sites)


def validate_firmware(validation: Validation) -> tuple[str, int]:
    manifest = load_json(ROOT / "firmware" / "manifest.json", validation)
    if not isinstance(manifest, dict):
        return "?", 0

    validation.require(manifest.get("schema") == "brvario.firmware", "schema OTA inválido")
    validation.require(manifest.get("schemaVersion") == 1, "schemaVersion OTA inválido")
    validation.require(manifest.get("board") == "T5-ePaper-S3", "placa OTA inválida")
    validation.require(isinstance(manifest.get("build"), int) and manifest["build"] > 0, "build OTA inválido")
    validation.require(isinstance(manifest.get("mandatory"), bool), "mandatory OTA deve ser booleano")

    parsed_url = urlparse(str(manifest.get("url", "")))
    validation.require(parsed_url.scheme == "https", "URL OTA deve usar HTTPS")
    validation.require(parsed_url.netloc == "randalrenye.github.io", "host OTA inesperado")
    validation.require(
        parsed_url.path.startswith("/brvario-epaper/firmware/"),
        "URL OTA deve apontar para firmware/ no GitHub Pages",
    )
    file_name = PurePosixPath(parsed_url.path).name
    validation.require(file_name.endswith(".bin"), "URL OTA não aponta para um arquivo .bin")

    binary_path = ROOT / "firmware" / file_name
    if not binary_path.is_file():
        validation.errors.append(f"firmware/{file_name}: binário OTA ausente")
    else:
        actual_size = binary_path.stat().st_size
        validation.require(actual_size == manifest.get("size"), "tamanho do binário OTA incorreto")
        actual_hash = hashlib.sha256(binary_path.read_bytes()).hexdigest()
        validation.require(actual_hash == manifest.get("sha256"), "SHA-256 do binário OTA incorreto")

    return str(manifest.get("version", "?")), int(manifest.get("build", 0) or 0)


def main() -> int:
    validation = Validation()
    map_count = validate_maps(validation)
    site_count = validate_weather(validation)
    version, build = validate_firmware(validation)

    if validation.errors:
        print(f"FALHA: {len(validation.errors)} erro(s) encontrado(s):")
        for error in validation.errors:
            print(f"- {error}")
        return 1

    print(
        "OK: "
        f"{map_count} mapas, {site_count} rampas e firmware {version} "
        f"build {build} validados."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
