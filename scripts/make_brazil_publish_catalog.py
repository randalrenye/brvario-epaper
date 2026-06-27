#!/usr/bin/env python3
"""Create the GitHub Pages catalog.json from generated .brmap files.

Use this after generating maps by state. It reads the full Brazil plan and
includes only packages that already exist in the publish directory.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Build catalog.json for BRVARIO map publishing")
    parser.add_argument("--plan", type=Path, default=Path("build/maps-brasil/catalog.plan.json"))
    parser.add_argument("--output-root", type=Path, default=Path("build/maps-brasil"))
    parser.add_argument("--catalog", type=Path, default=None)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    catalog_path = args.catalog or (args.output_root / "catalog.json")
    plan = json.loads(args.plan.read_text(encoding="utf-8"))
    entries: list[dict] = []
    for entry in plan:
        file_path = args.output_root / entry["file"]
        if not file_path.exists():
            continue
        published = dict(entry)
        published["size"] = file_path.stat().st_size
        entries.append(published)

    catalog_path.parent.mkdir(parents=True, exist_ok=True)
    catalog_path.write_text(json.dumps(entries, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(f"Wrote {catalog_path} with {len(entries)} generated maps")


if __name__ == "__main__":
    main()
