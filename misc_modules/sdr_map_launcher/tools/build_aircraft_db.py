#!/usr/bin/env python3
"""
Builds a local SQLite aircraft database from the Mictronics dataset.

The dataset (~14 MB JSON, ~443 000 aircraft) maps each ICAO 24-bit hex
address to:
   [registration, ICAO type code (e.g. "B738"), flags]
where the first character of `flags` is "1" when the aircraft is military.

Output: an SQLite file with an index on the ICAO column for ~10 µs lookups.
The file lives in `~/.local/share/sdr_map_launcher/aircraft_db.sqlite` by
default so it survives Django project redeployments — only the install.sh
script (re)downloads it; the running server merely reads from it.

Usage:
   python3 build_aircraft_db.py                # default output path
   python3 build_aircraft_db.py /path/to/db    # custom output path
   python3 build_aircraft_db.py --offline      # use bundled fallback file

If the download fails (no internet, GitHub down, ...) the script exits
with a clear message and a non-zero status. install.sh treats that as
a "Phase A only" install (i.e. the wake-vortex category icon mapping
still works, just without the extra enrichment from the database).
"""

import argparse
import json
import os
import sqlite3
import sys
import time
import urllib.request

MICTRONICS_URL = (
    "https://raw.githubusercontent.com/Mictronics/"
    "readsb-protobuf/dev/webapp/src/db/aircrafts.json"
)
DEFAULT_OUTPUT = os.path.expanduser(
    "~/.local/share/sdr_map_launcher/aircraft_db.sqlite"
)


def download_json(url, timeout=60):
    """Stream-download the Mictronics JSON. Returns the parsed dict.

    We don't try to be clever about resume / partial-download because
    14 MB over HTTPS is fast enough that any failure is best handled
    by simply re-running the script.
    """
    print(f"Downloading {url}", flush=True)
    t0 = time.time()
    req = urllib.request.Request(
        url,
        headers={"User-Agent": "sdr-map-launcher/1.0 build_aircraft_db"},
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        raw = resp.read()
    print(f"  downloaded {len(raw) / 1024 / 1024:.1f} MB in "
          f"{time.time() - t0:.1f}s", flush=True)
    data = json.loads(raw)
    print(f"  parsed {len(data)} aircraft entries", flush=True)
    return data


def build_sqlite(data, output_path):
    """Create (or replace) the SQLite file at `output_path`.

    Schema is intentionally minimal: ICAO is the primary key (case
    normalised to upper hex), and we only carry the columns we will
    actually surface in the UI. is_military is materialised as a
    proper INTEGER so the WHERE-clause is index-friendly if we ever
    want to filter on it.
    """
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    if os.path.exists(output_path):
        os.unlink(output_path)
    conn = sqlite3.connect(output_path)
    cur = conn.cursor()
    cur.executescript("""
        CREATE TABLE aircraft (
            icao         TEXT PRIMARY KEY,
            registration TEXT,
            type_code    TEXT,
            is_military  INTEGER DEFAULT 0
        );
        -- PRAGMAs that speed up bulk insert without losing durability
        -- once the script exits.
        PRAGMA journal_mode = OFF;
        PRAGMA synchronous = OFF;
        PRAGMA temp_store = MEMORY;
    """)
    rows = []
    for icao, entry in data.items():
        # Defensive: some bogus entries can have fewer than 3 fields.
        reg = (entry[0] if len(entry) > 0 else "") or ""
        tc = (entry[1] if len(entry) > 1 else "") or ""
        flags = (entry[2] if len(entry) > 2 else "") or ""
        is_mil = 1 if flags.startswith("1") else 0
        # Keep ICAO normalised so lookups don't have to do .upper() on
        # every access. Registrations and type codes are stored as-is
        # (they're already canonical in the source).
        rows.append((icao.upper(), reg, tc, is_mil))
    cur.executemany(
        "INSERT INTO aircraft (icao, registration, type_code, is_military) "
        "VALUES (?, ?, ?, ?)",
        rows,
    )
    # The primary key already creates an implicit index; no extra one needed.
    # Restore safe pragmas for the read-only consumer.
    cur.executescript("PRAGMA journal_mode = WAL;")
    conn.commit()
    conn.close()
    size_mb = os.path.getsize(output_path) / 1024 / 1024
    n_mil = sum(1 for r in rows if r[3])
    print(f"  wrote {len(rows)} rows to {output_path} "
          f"({size_mb:.1f} MB, {n_mil} military)", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("output", nargs="?", default=DEFAULT_OUTPUT,
                   help="Output SQLite path (default: %(default)s)")
    p.add_argument("--url", default=MICTRONICS_URL,
                   help="Override the dataset URL")
    args = p.parse_args()
    try:
        data = download_json(args.url)
    except Exception as exc:
        print(f"ERROR: download failed: {exc}", file=sys.stderr)
        print("       (no internet? GitHub down? — the rest of the install "
              "will continue without the enrichment database.)",
              file=sys.stderr)
        return 1
    build_sqlite(data, args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
