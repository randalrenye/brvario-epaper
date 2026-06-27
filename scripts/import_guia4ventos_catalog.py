#!/usr/bin/env python3
"""Inventory Guia 4 Ventos ramp pages and import objective flight-site facts.

The importer deliberately ignores editorial descriptions, access instructions,
photos, videos, contacts, advantages, risks and other authored content. It
keeps only identity/location facts, altitude, vertical drop and wind quadrants,
plus a link to the public source page for verification.
"""

from __future__ import annotations

import argparse
import datetime as dt
import html
import json
import re
import ssl
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path
from typing import Any


BASE_URL = "https://guia4ventos.com.br"
RAMP_CATEGORY_ID = 3
USER_AGENT = "BRVARIO-Flight-Site-Catalog/1.0"
STATE_SLUGS = {
    "al", "ba", "ce", "es", "go", "ma", "mt", "ms", "mg", "pr", "pb",
    "pe", "rj", "rn", "rs", "ro", "rr", "sc", "se", "sp", "to",
}
QUADRANT_SLUGS = {
    "n-norte": "N",
    "ne-nordeste": "NE",
    "e-leste": "E",
    "se-sudeste": "SE",
    "s-sul": "S",
    "sw-sudoeste": "SW",
    "w-oeste": "W",
    "nw-noroeste": "NW",
}
def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Inventory/import Guia 4 Ventos ramp records")
    parser.add_argument(
        "--inventory",
        type=Path,
        default=Path("weather/sources/guia4ventos.inventory.json"),
    )
    parser.add_argument("--sites-dir", type=Path, default=Path("weather/sites"))
    parser.add_argument("--import-facts", action="store_true")
    parser.add_argument(
        "--delay-ms",
        type=int,
        default=500,
        help="Delay between source page requests (default: 500 ms)",
    )
    parser.add_argument(
        "--ids",
        help="Comma-separated WordPress post IDs for a targeted retry",
    )
    parser.add_argument("--insecure", action="store_true", help="Ignore TLS certificate validation")
    return parser.parse_args()


def request_bytes(path: str, context: ssl.SSLContext) -> tuple[bytes, dict[str, str]]:
    url = path if path.startswith("https://") else BASE_URL + path
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": USER_AGENT,
            "Accept": "application/json,text/html;q=0.9,*/*;q=0.8",
            "Accept-Language": "pt-BR,pt;q=0.9,en;q=0.5",
            "Connection": "close",
        },
    )
    last_error: Exception | None = None
    for attempt in range(4):
        try:
            with urllib.request.urlopen(request, context=context, timeout=45) as response:
                headers = {key.lower(): value for key, value in response.headers.items()}
                return response.read(), headers
        except (OSError, urllib.error.URLError) as error:
            last_error = error
            if attempt < 3:
                time.sleep(2 ** attempt)
    raise RuntimeError(f"{url}: request failed after 4 attempts: {last_error}")


def request_json(path: str, context: ssl.SSLContext) -> tuple[Any, dict[str, str]]:
    body, headers = request_bytes(path, context)
    return json.loads(body.decode("utf-8")), headers


def request_html(path: str, context: ssl.SSLContext) -> str:
    body, headers = request_bytes(path, context)
    content_type = headers.get("content-type", "")
    charset_match = re.search(r"charset=([a-zA-Z0-9_-]+)", content_type)
    charset = charset_match.group(1) if charset_match else "utf-8"
    return body.decode(charset, errors="replace")


def fetch_all_ramp_posts(context: ssl.SSLContext) -> list[dict[str, Any]]:
    posts: list[dict[str, Any]] = []
    page = 1
    while True:
        query = urllib.parse.urlencode(
            {
                "categories": RAMP_CATEGORY_ID,
                "per_page": 100,
                "page": page,
                "_embed": "wp:term",
                "orderby": "id",
                "order": "asc",
            }
        )
        batch, headers = request_json(f"/wp-json/wp/v2/posts?{query}", context)
        posts.extend(batch)
        total_pages = int(headers.get("x-wp-totalpages", page))
        if page >= total_pages:
            break
        page += 1
    return posts


def clean_text(value: str) -> str:
    value = html.unescape(re.sub(r"<[^>]+>", " ", value or ""))
    value = re.sub(r"[\ue000-\uf8ff]", " ", value)
    return re.sub(r"\s+", " ", value).strip()


def embedded_terms(post: dict[str, Any]) -> list[dict[str, Any]]:
    output: list[dict[str, Any]] = []
    for group in post.get("_embedded", {}).get("wp:term", []):
        output.extend(group)
    return output


def inventory_entry(post: dict[str, Any]) -> dict[str, Any]:
    return {
        "sourceId": post["id"],
        "title": clean_text(post["title"]["rendered"]),
        "slug": post["slug"],
        "url": post["link"],
        "modified": post["modified"],
    }


def write_inventory(path: Path, posts: list[dict[str, Any]]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    data = {
        "schema": "brvario.source-inventory",
        "schemaVersion": 1,
        "source": "Guia 4 Ventos",
        "sourceUrl": f"{BASE_URL}/rampas-de-voo-livre/",
        "retrievedAt": dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat(),
        "recordCount": len(posts),
        "redistributionAuthorized": False,
        "factualImportOnly": True,
        "editorialContentIncluded": False,
        "records": [inventory_entry(post) for post in posts],
    }
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def terms_by_taxonomy(post: dict[str, Any], taxonomy: str) -> list[dict[str, Any]]:
    return [term for term in embedded_terms(post) if term.get("taxonomy") == taxonomy]


def local_field(page_html: str) -> str:
    text = clean_text(html.unescape(page_html))
    match = re.search(
        r"\bLocal\s*:\s*(.+?)(?=\s+(?:WayPoint|Acesso|Decolagem|Pouso)\s*:)",
        text,
        re.IGNORECASE,
    )
    return match.group(1).strip() if match else ""


def state_from_post(post: dict[str, Any], page_html: str) -> str:
    states = [term["slug"].upper() for term in terms_by_taxonomy(post, "category") if term.get("slug") in STATE_SLUGS]
    if len(states) == 1:
        return states[0]

    local = local_field(page_html)
    local_state = re.search(r"(?:–|—|-)\s*([A-Z]{2})\s*$", local)
    if local_state and local_state.group(1).lower() in STATE_SLUGS:
        return local_state.group(1)

    title = clean_text(post["title"]["rendered"])
    title_state = re.search(r"(?:–|—|-)\s*([A-Z]{2})\s*$", title)
    if title_state and title_state.group(1).lower() in STATE_SLUGS:
        return title_state.group(1)

    raise ValueError(f"{post['link']}: expected one state, found categories {states}")


def title_parts(post: dict[str, Any], state: str, page_html: str) -> tuple[str, str]:
    states = [term["slug"].upper() for term in terms_by_taxonomy(post, "category") if term.get("slug") in STATE_SLUGS]
    if len(states) > 1:
        local = local_field(page_html)
        local_parts = [part.strip() for part in re.split(r"\s+[–—-]\s+", local) if part.strip()]
        if local_parts and local_parts[-1].upper() == state:
            local_parts.pop()
        if len(local_parts) >= 2:
            return local_parts[0].upper(), " - ".join(local_parts[1:]).upper()

    title = clean_text(post["title"]["rendered"])
    parts = [part.strip() for part in re.split(r"\s+[–—-]\s+", title) if part.strip()]
    if parts and parts[-1].upper() == state:
        parts.pop()
    if len(parts) >= 2:
        return parts[0].upper(), " - ".join(parts[1:]).upper()
    if len(parts) == 1:
        label = parts[0].strip()
        city_match = re.match(r"Rampa\s+(?:de\s+|da\s+|do\s+|das\s+|dos\s+)?(.+)", label, re.IGNORECASE)
        if city_match:
            return city_match.group(1).upper(), label.upper()
    raise ValueError(f"{post['link']}: title does not contain city and site name")


def decimal_number(text: str) -> float:
    if "," in text:
        return float(text.replace(".", "").replace(",", "."))
    if re.fullmatch(r"\d{1,3}(?:\.\d{3})+", text):
        return float(text.replace(".", ""))
    return float(text)


def coordinate_pair(text: str) -> tuple[float, float] | None:
    match = re.search(
        r"\(\s*(-?\d{1,2}[.,]\d{4,15})\s*[,; ]+\s*(-?\d{1,3}[.,]\d{4,15})\s*\)",
        text,
    )
    if not match:
        return None
    latitude = float(match.group(1).replace(",", "."))
    longitude = float(match.group(2).replace(",", "."))
    if -90 <= latitude <= 90 and -180 <= longitude <= 180:
        return latitude, longitude
    return None


def waypoint_field(content: str) -> str:
    text = clean_text(html.unescape(content))
    match = re.search(
        r"\bWayPoint\s*:\s*(?!not\b)(.+?)(?=\s+(?:Acesso|Decolagem|Pouso|Melhor época|Clube)\s*:)",
        text,
    )
    return match.group(1).strip() if match else ""


def dms_to_decimal(degrees: str, minutes: str, seconds: str, hemisphere: str) -> float:
    value = int(degrees) + int(minutes) / 60.0 + float(seconds.replace(",", ".")) / 3600.0
    return -value if hemisphere.upper() in {"S", "W"} else value


def dms_coordinates(text: str) -> tuple[float, float] | None:
    pattern = re.compile(
        r"([NS])?\s*(\d{1,2})\s*[º°]\s*(\d{1,2})\s*[’′']\s*([\d.,]+)\s*[”″\"]?\s*([NS])?"
        r"\s*[,;/ -]*\s*"
        r"([EW])?\s*(\d{1,3})\s*[º°]\s*(\d{1,2})\s*[’′']\s*([\d.,]+)\s*[”″\"]?\s*([EW])?",
        re.IGNORECASE,
    )
    match = pattern.search(text)
    if not match:
        return None
    latitude_hemisphere = match.group(1) or match.group(5)
    longitude_hemisphere = match.group(6) or match.group(10)
    if not latitude_hemisphere or not longitude_hemisphere:
        return None
    latitude = dms_to_decimal(match.group(2), match.group(3), match.group(4), latitude_hemisphere)
    longitude = dms_to_decimal(match.group(7), match.group(8), match.group(9), longitude_hemisphere)
    return latitude, longitude


def extract_coordinates(content: str, source_url: str) -> tuple[int, int]:
    waypoint = waypoint_field(content)
    coordinates = coordinate_pair(waypoint)
    if coordinates:
        return round(coordinates[0] * 10_000_000), round(coordinates[1] * 10_000_000)

    map_start = content.find("gmpAllMapsInfo")
    if map_start >= 0:
        map_script = content[map_start:map_start + 200_000]
        map_match = re.search(
            r'"coord_x"\s*:\s*"(-?\d{1,2}[.,]\d+)"\s*,\s*"coord_y"\s*:\s*"(-?\d{1,3}[.,]\d+)"',
            map_script,
            re.IGNORECASE,
        )
        if map_match:
            latitude = float(map_match.group(1).replace(",", "."))
            longitude = float(map_match.group(2).replace(",", "."))
            if -90 <= latitude <= 90 and -180 <= longitude <= 180:
                return round(latitude * 10_000_000), round(longitude * 10_000_000)

    coordinates = dms_coordinates(waypoint)
    if coordinates:
        return round(coordinates[0] * 10_000_000), round(coordinates[1] * 10_000_000)

    degrees_minutes = re.search(
        r"WayPoint\s*:\s*(\d{1,2})\s+(\d{1,2})\s*[sS]\s+(\d{1,3})\s+(\d{1,2})\s*[wW]\b",
        clean_text(html.unescape(content)),
        re.IGNORECASE,
    )
    if degrees_minutes:
        latitude = -(int(degrees_minutes.group(1)) + int(degrees_minutes.group(2)) / 60.0)
        longitude = -(int(degrees_minutes.group(3)) + int(degrees_minutes.group(4)) / 60.0)
        return round(latitude * 10_000_000), round(longitude * 10_000_000)

    raise ValueError(f"{source_url}: coordinates not found")


def extract_metric(content: str, label: str, required: bool = True) -> int | None:
    text = clean_text(content)
    match = re.search(rf"{label}\s*:\s*([\d.,]+)\s*m\b", text, re.IGNORECASE)
    if not match:
        if required:
            raise ValueError(f"{label} not found")
        return None
    return round(decimal_number(match.group(1)))


def factual_site(post: dict[str, Any], page_html: str) -> dict[str, Any]:
    state = state_from_post(post, page_html)
    city, name = title_parts(post, state, page_html)
    rendered = html.unescape(page_html)
    latitude_e7, longitude_e7 = extract_coordinates(rendered, post["link"])
    altitude_m = extract_metric(rendered, "Altitude")
    vertical_drop_m = extract_metric(rendered, r"Desn[ií]vel", required=False)

    categories = terms_by_taxonomy(post, "category")
    quadrants = [
        QUADRANT_SLUGS[term["slug"]]
        for term in categories
        if term.get("slug") in QUADRANT_SLUGS
    ]
    if not quadrants:
        text = clean_text(rendered)
        field = re.search(
            r"Quadrantes\s*:\s*(.+?)(?=\s+(?:Local|WayPoint|Acesso|Decolagem|Pouso)\s*:)",
            text,
            re.IGNORECASE,
        )
        if field:
            quadrants = re.findall(r"\((N|NE|E|SE|S|SW|W|NW)\)", field.group(1), re.IGNORECASE)
    quadrants = list(dict.fromkeys(quadrant.upper() for quadrant in quadrants))

    terrain: dict[str, int] = {"altitudeM": altitude_m}
    if vertical_drop_m is not None:
        terrain["verticalDropM"] = vertical_drop_m

    return {
        "schema": "brvario.flight-site",
        "schemaVersion": 1,
        "id": post["id"],
        "slug": post["slug"],
        "name": name[:39],
        "city": city[:27],
        "state": state,
        "country": "BR",
        "coordinates": {
            "latitudeE7": latitude_e7,
            "longitudeE7": longitude_e7,
        },
        "terrain": terrain,
        "windQuadrants": quadrants,
        "provenance": {
            "dataOwner": "BRVARIO",
            "sourceName": "Guia 4 Ventos - referência factual",
            "sourceUrl": post["link"],
            "referenceUrl": post["link"],
            "license": "NOASSERTION - somente fatos objetivos, sem conteúdo editorial",
            "contributedBy": "Importador factual BRVARIO",
            "lastVerified": dt.date.today().isoformat(),
        },
    }


def write_json_atomic(path: Path, data: dict[str, Any]) -> None:
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    temporary.replace(path)


def import_factual_sites(
    posts: list[dict[str, Any]],
    sites_dir: Path,
    context: ssl.SSLContext,
    delay_ms: int,
) -> tuple[int, list[str]]:
    sites_dir.mkdir(parents=True, exist_ok=True)
    imported = 0
    errors: list[str] = []
    for index, post in enumerate(posts):
        try:
            page_html = request_html(post["link"], context)
            site = factual_site(post, page_html)
            output = sites_dir / f"{site['id']:05d}.json"
            write_json_atomic(output, site)
            imported += 1
        except Exception as error:  # Keep a complete audit instead of stopping at the first legacy page.
            errors.append(f"{post['id']} {post['link']}: {error}")
        if delay_ms > 0 and index + 1 < len(posts):
            time.sleep(delay_ms / 1000.0)
    return imported, errors


def main() -> None:
    args = parse_args()
    context = ssl._create_unverified_context() if args.insecure else ssl.create_default_context()
    posts = fetch_all_ramp_posts(context)
    write_inventory(args.inventory, posts)
    print(f"Wrote {args.inventory} with {len(posts)} Guia 4 Ventos ramp source records")

    if not args.import_facts:
        return

    if args.delay_ms < 0:
        raise ValueError("--delay-ms cannot be negative")
    selected_posts = posts
    if args.ids:
        selected_ids = {int(value.strip()) for value in args.ids.split(",") if value.strip()}
        selected_posts = [post for post in posts if post["id"] in selected_ids]
        found_ids = {post["id"] for post in selected_posts}
        missing_ids = sorted(selected_ids - found_ids)
        if missing_ids:
            raise ValueError(f"post IDs not found in ramp inventory: {missing_ids}")
    imported, errors = import_factual_sites(selected_posts, args.sites_dir, context, args.delay_ms)
    print(f"Imported {imported} factual site records into {args.sites_dir}")
    if errors:
        error_path = args.inventory.with_suffix(".errors.txt")
        error_path.write_text("\n".join(errors) + "\n", encoding="utf-8")
        raise RuntimeError(f"{len(errors)} records need manual review; see {error_path}")


if __name__ == "__main__":
    main()
