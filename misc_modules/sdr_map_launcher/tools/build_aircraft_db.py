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

MICTRONICS_BASE = (
    "https://raw.githubusercontent.com/Mictronics/"
    "readsb-protobuf/dev/webapp/src/db"
)
MICTRONICS_URL_AIRCRAFT  = f"{MICTRONICS_BASE}/aircrafts.json"   # ~14 MB
MICTRONICS_URL_OPERATORS = f"{MICTRONICS_BASE}/operators.json"   # ~0.3 MB
MICTRONICS_URL_TYPES     = f"{MICTRONICS_BASE}/types.json"       # ~0.1 MB
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


def build_sqlite(aircraft_data, operators_data, types_data, output_path):
    """Create (or replace) the SQLite file at `output_path` atomically.

    Writes to `output_path + ".new"` first, then renames it over the
    destination. On POSIX (Linux/macOS) os.replace is atomic, so a
    concurrent reader either sees the old or the new database — never
    a half-written one. On Windows we drop any open handle on the
    destination first (handled by the /api/aircraft_db/update endpoint
    via AircraftDB._close()).

    Three tables ... (same as before)
    """
    output_path = str(output_path)
    tmp_path = output_path + ".new"
    os.makedirs(os.path.dirname(output_path) or ".", exist_ok=True)
    # If a previous interrupted run left a .new file around, clear it
    # so sqlite3.connect doesn't reuse a partial schema.
    if os.path.exists(tmp_path):
        os.unlink(tmp_path)

    conn = sqlite3.connect(tmp_path)
    cur = conn.cursor()
    cur.executescript("""
        CREATE TABLE aircraft (
            icao         TEXT PRIMARY KEY,
            registration TEXT,
            type_code    TEXT,
            is_military  INTEGER DEFAULT 0
        );
        CREATE TABLE operators (
            code         TEXT PRIMARY KEY,  -- 3-letter ICAO airline code
            name         TEXT,              -- e.g. "Air France"
            country      TEXT,              -- e.g. "France"
            callsign     TEXT               -- phonetic, e.g. "AIRFRANS"
        );
        CREATE TABLE types (
            type_code    TEXT PRIMARY KEY,  -- e.g. "B738"
            description  TEXT,              -- "BOEING 737-800"
            icao_class   TEXT,              -- "L2J" (Land 2-engine Jet)
            weight       TEXT               -- "L"|"M"|"H"|"J"
        );
        -- Bulk-insert PRAGMAs. Restored to safe defaults at the end.
        PRAGMA journal_mode = OFF;
        PRAGMA synchronous = OFF;
        PRAGMA temp_store = MEMORY;
    """)

    # --- aircraft table --------------------------------------------------
    rows = []
    for icao, entry in aircraft_data.items():
        reg = (entry[0] if len(entry) > 0 else "") or ""
        tc = (entry[1] if len(entry) > 1 else "") or ""
        flags = (entry[2] if len(entry) > 2 else "") or ""
        is_mil = 1 if flags.startswith("1") else 0
        rows.append((icao.upper(), reg, tc, is_mil))
    cur.executemany(
        "INSERT INTO aircraft (icao, registration, type_code, is_military) "
        "VALUES (?, ?, ?, ?)", rows,
    )
    n_aircraft, n_mil = len(rows), sum(1 for r in rows if r[3])

    # --- operators table -------------------------------------------------
    rows = []
    for code, entry in operators_data.items():
        name     = (entry[0] if len(entry) > 0 else "") or ""
        country  = (entry[1] if len(entry) > 1 else "") or ""
        callsign = (entry[2] if len(entry) > 2 else "") or ""
        rows.append((code.upper(), name, country, callsign))
    cur.executemany(
        "INSERT INTO operators (code, name, country, callsign) "
        "VALUES (?, ?, ?, ?)", rows,
    )
    n_operators = len(rows)

    # --- types table -----------------------------------------------------
    rows = []
    for tc, entry in types_data.items():
        desc    = (entry[0] if len(entry) > 0 else "") or ""
        icao_cl = (entry[1] if len(entry) > 1 else "") or ""
        weight  = (entry[2] if len(entry) > 2 else "") or ""
        rows.append((tc.upper(), desc, icao_cl, weight))
    cur.executemany(
        "INSERT INTO types (type_code, description, icao_class, weight) "
        "VALUES (?, ?, ?, ?)", rows,
    )
    n_types = len(rows)

    cur.executescript("PRAGMA journal_mode = WAL;")
    conn.commit()
    conn.close()
    # Atomic replace — the old DB (if any) and the new one are never
    # both seen as "current" by readers.
    os.replace(tmp_path, output_path)
    size_mb = os.path.getsize(output_path) / 1024 / 1024
    print(f"  wrote SQLite ({size_mb:.1f} MB)", flush=True)
    print(f"    aircraft : {n_aircraft} rows ({n_mil} military)", flush=True)
    print(f"    operators: {n_operators} rows", flush=True)
    print(f"    types    : {n_types} rows", flush=True)


def main():
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("output", nargs="?", default=DEFAULT_OUTPUT,
                   help="Output SQLite path (default: %(default)s)")
    args = p.parse_args()
    try:
        aircraft  = download_json(MICTRONICS_URL_AIRCRAFT)
        operators = download_json(MICTRONICS_URL_OPERATORS)
        types     = download_json(MICTRONICS_URL_TYPES)
    except Exception as exc:
        print(f"ERROR: download failed: {exc}", file=sys.stderr)
        print("       (no internet? GitHub down? — the rest of the install "
              "will continue without the enrichment database.)",
              file=sys.stderr)
        return 1
    build_sqlite(aircraft, operators, types, args.output)
    return 0


if __name__ == "__main__":
    sys.exit(main())
