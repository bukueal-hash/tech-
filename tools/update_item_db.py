#!/usr/bin/env python3
"""
update_item_db.py — Regenerate item rarity/value/type data from arcdata.mahcks.com.

Fetches the full item database from the arcdata.mahcks.com public API and:
  1. Regenerates items_meta.json  (snake_case id → name/rarity/value/asset)
  2. Updates  Bots_Items_Maps/en.json items section (UE asset key → name/type/rarity/value/weightKg)

Preserves existing UE asset key mappings and meta 'asset' fields that the API doesn't provide.
Only merges rarity/type/value/weight changes — does not delete or rename existing entries.

Usage:
    python tools/update_item_db.py              # dry-run (prints diff)
    python tools/update_item_db.py --apply      # writes files
    python tools/update_item_db.py --verbose    # extra logging

Requires: Python 3.8+, no external deps (uses urllib + json).
"""

import argparse
import json
import os
import sys
import urllib.request
from collections import OrderedDict
from pathlib import Path

API_BASE = "https://arcdata.mahcks.com/v1"
API_ITEMS_LIST = f"{API_BASE}/items"

# Resolve paths relative to the repo root (script is in tools/)
REPO_ROOT = Path(__file__).resolve().parent.parent
META_PATH = REPO_ROOT / "Project" / "Data" / "items_meta.json"
BM_PATH   = REPO_ROOT / "Project" / "Data" / "Bots_Items_Maps" / "en.json"


# ---------------------------------------------------------------------------
# API helpers
# ---------------------------------------------------------------------------

def fetch_json(url: str, retries: int = 3) -> dict:
    """Fetch JSON from URL with retries."""
    last_err = None
    for attempt in range(retries):
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "ArcRaiders-ItemDB-Updater/1.0"})
            with urllib.request.urlopen(req, timeout=15) as resp:
                return json.loads(resp.read().decode("utf-8"))
        except Exception as e:
            last_err = e
            if attempt < retries - 1:
                import time
                time.sleep(1 * (attempt + 1))
    raise RuntimeError(f"Failed to fetch {url} after {retries} attempts: {last_err}")


def fetch_all_items() -> list[dict]:
    """Fetch full item list then individual details."""
    listing = fetch_json(API_ITEMS_LIST)
    ids = [item["id"] for item in listing.get("items", [])]
    print(f"  API returned {len(ids)} item IDs")

    items = []
    for i, item_id in enumerate(ids):
        url = f"{API_BASE}/items/{item_id}"
        try:
            detail = fetch_json(url)
            items.append(detail)
        except Exception as e:
            print(f"  WARNING: failed to fetch {item_id}: {e}")
        if (i + 1) % 50 == 0:
            print(f"  fetched {i + 1}/{len(ids)}...")

    print(f"  fetched {len(items)}/{len(ids)} items total")
    return items


# ---------------------------------------------------------------------------
# items_meta.json generation
# ---------------------------------------------------------------------------

def generate_items_meta(api_items: list[dict], existing_meta: list[dict], verbose: bool = False) -> list[dict]:
    """
    Merge API data into items_meta.json.

    Strategy:
    - Start with existing entries (preserves entries not in API, e.g. traps)
    - Build name→existing lookup to preserve 'asset' fields
    - Update existing entries with fresh rarity/value from API
    - Add new API entries not yet present
    - Sort alphabetically by id
    """
    # Build name→existing lookup (preserves asset field)
    existing_by_name = {}
    for entry in existing_meta:
        existing_by_name[entry.get("name", "").lower()] = entry

    # Start with existing entries (preserved as-is)
    merged = {e.get("id", ""): dict(e) for e in existing_meta}

    api_updated = 0
    api_added = 0
    api_preserved_asset = 0

    for item in api_items:
        item_id = item.get("id", "")
        name_obj = item.get("name", {})
        en_name = name_obj.get("en", "") if isinstance(name_obj, dict) else ""
        rarity = (item.get("rarity") or "").lower()
        value = item.get("value", 0)

        if not en_name or not rarity:
            continue

        existing = merged.get(item_id)
        if existing:
            # Update rarity/value from API, preserve asset field
            changed = False
            if existing.get("rarity") != rarity:
                existing["rarity"] = rarity
                changed = True
            if existing.get("value") != value:
                existing["value"] = value
                changed = True
            if changed:
                api_updated += 1
        else:
            # New entry from API
            entry = {"id": item_id, "name": en_name, "rarity": rarity, "value": value}
            # Check if existing entries by name have an asset field to preserve
            name_existing = existing_by_name.get(en_name.lower())
            if name_existing and "asset" in name_existing:
                entry["asset"] = name_existing["asset"]
                api_preserved_asset += 1
            merged[item_id] = entry
            api_added += 1

    out = sorted(merged.values(), key=lambda e: e.get("id", ""))

    print(f"  items_meta: {len(existing_meta)} existing + {api_added} new - {api_updated} updated, {len(out)} total")
    return out


# ---------------------------------------------------------------------------
# Bots_Items_Maps/en.json update
# ---------------------------------------------------------------------------

def update_bm_items(api_items: list[dict], bm_data: dict, verbose: bool = False) -> dict:
    """
    Update the 'items' section of Bots_Items_Maps/en.json.

    Strategy:
    - Build a name→api lookup
    - For each existing en.json entry, update type/rarity/value/weightKg from API
    - Report entries that were updated vs unchanged vs not found in API
    """
    items = bm_data.get("items", {})

    # Build name→api_data lookup
    api_by_name = {}
    for item in api_items:
        name_obj = item.get("name", {})
        en_name = name_obj.get("en", "") if isinstance(name_obj, dict) else ""
        if en_name:
            api_by_name[en_name.lower()] = item

    updated = 0
    unchanged = 0
    not_found = 0

    for key, entry in items.items():
        name = entry.get("name", "")
        api_data = api_by_name.get(name.lower())

        if not api_data:
            not_found += 1
            if verbose:
                print(f"    BM: {key} ({name}) not found in API")
            continue

        # Update fields from API
        changed = False
        api_rarity = (api_data.get("rarity") or "").lower()
        api_type = api_data.get("type", "")
        api_value = api_data.get("value", 0)
        api_weight = api_data.get("weightKg", 0)

        if api_rarity and entry.get("rarity", "").lower() != api_rarity:
            entry["rarity"] = api_rarity.capitalize()
            changed = True
        if api_type and entry.get("type") != api_type:
            entry["type"] = api_type
            changed = True
        if isinstance(api_value, (int, float)) and entry.get("value") != api_value:
            entry["value"] = api_value
            changed = True
        if isinstance(api_weight, (int, float)) and entry.get("weightKg") != api_weight:
            entry["weightKg"] = api_weight
            changed = True

        if changed:
            updated += 1
            if verbose:
                print(f"    BM: {key} ({name}) UPDATED")
        else:
            unchanged += 1

    print(f"  en.json items: {updated} updated, {unchanged} unchanged, {not_found} not in API")
    return bm_data


# ---------------------------------------------------------------------------
# File I/O (preserves formatting)
# ---------------------------------------------------------------------------

def load_json(path: Path):
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def write_items_meta(data: list[dict], path: Path, apply: bool):
    """Write items_meta.json in the existing format (one entry per line, compact)."""
    if not apply:
        print(f"  [dry-run] Would write {len(data)} entries to {path}")
        return

    lines = ["[\n"]
    for i, entry in enumerate(data):
        line = "  " + json.dumps(entry, ensure_ascii=False)
        if i < len(data) - 1:
            line += ","
        lines.append(line + "\n")
    lines.append("]\n")

    with open(path, "w", encoding="utf-8") as f:
        f.writelines(lines)
    print(f"  Wrote {len(data)} entries to {path}")


def write_bm_json(data: dict, path: Path, apply: bool):
    """Write en.json preserving 2-space indent, ensure_ascii=False."""
    if not apply:
        print(f"  [dry-run] Would write en.json to {path}")
        return

    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
        f.write("\n")
    print(f"  Wrote en.json to {path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="Update item DB from arcdata.mahcks.com API")
    parser.add_argument("--apply", action="store_true", help="Write files (default: dry-run)")
    parser.add_argument("--verbose", "-v", action="store_true", help="Extra logging")
    args = parser.parse_args()

    print("=== Arc Raiders Item DB Updater ===")
    print(f"  API: {API_BASE}")
    print(f"  items_meta: {META_PATH}")
    print(f"  en.json:    {BM_PATH}")
    print()

    # 1. Fetch from API
    print("[1/4] Fetching items from arcdata.mahcks.com...")
    api_items = fetch_all_items()
    print()

    # 2. Load existing files
    print("[2/4] Loading existing data...")
    existing_meta = load_json(META_PATH)
    bm_data = load_json(BM_PATH)
    print(f"  items_meta.json: {len(existing_meta)} entries")
    print(f"  en.json items:   {len(bm_data.get('items', {}))} entries")
    print()

    # 3. Generate items_meta.json
    print("[3/4] Generating items_meta.json...")
    new_meta = generate_items_meta(api_items, existing_meta, args.verbose)
    write_items_meta(new_meta, META_PATH, args.apply)
    print()

    # 4. Update en.json
    print("[4/4] Updating Bots_Items_Maps/en.json...")
    bm_data = update_bm_items(api_items, bm_data, args.verbose)
    write_bm_json(bm_data, BM_PATH, args.apply)
    print()

    if not args.apply:
        print("=== DRY RUN — no files written. Use --apply to write. ===")
    else:
        print("=== Done. Files updated. ===")


if __name__ == "__main__":
    main()
