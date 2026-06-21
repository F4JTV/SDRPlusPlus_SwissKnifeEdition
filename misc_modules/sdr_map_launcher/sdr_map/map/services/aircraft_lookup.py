"""
Aircraft database lookup helper for ADS-B enrichment.

Looks up an ICAO 24-bit hex address in the local Mictronics-derived SQLite
database (built by tools/build_aircraft_db.py) and returns enrichment
fields: registration, type code, military flag, and a derived wake-vortex
category when one can be inferred from the type code.

If the database file doesn't exist (Phase A only install) the module
returns None for every lookup — listen_sdr.py treats that as "no extra
info" and falls back to whatever the ADS-B frame itself transmitted.

Design notes:
  - The lookup module is loaded once at import time and keeps a single
    read-only sqlite3 connection alive. We rely on SQLite's PRIMARY KEY
    index for ~10 µs lookups; no in-process LRU cache is needed.
  - The category inference is pattern-based on the ICAO type code, with
    helicopters / gliders / fighters covered. It is *only* used when the
    ADS-B frame doesn't transmit its own category (~5-15 % of aircraft).
"""

import os
import re
import sqlite3
import threading

DEFAULT_DB_PATH = os.path.expanduser(
    "~/.local/share/sdr_map_launcher/aircraft_db.sqlite"
)

# Pattern-based ICAO type code -> wake-vortex category inference.
# Ordered most-specific-first: helicopters and gliders are checked before
# fighters so that a "F" prefix doesn't accidentally classify a Falcon
# (Dassault Falcon = F900, F2TH) as a fighter. Fighters use stricter
# patterns specifically targeting military fast-jet type codes.
#
# This is *fallback* logic — if the aircraft transmits its own wake-vortex
# category in the ADS-B Aircraft Identification message, that's what we
# use. The patterns below only fire when the category is missing AND we
# managed to find a type code in the database.
_PATTERN_HELI = re.compile(
    r"^(EC|AS3|AS5|AS6|R22|R44|R66|S6|S7|S9|S5|H47|H53|H60|H64|H65|"
    r"AW|BK17|MI8|MI17|NH90|TIGR|GAZL|PUMA|B06|B41|B43|MD5|MD60|MD90|"
    r"DJI|UH1|HUEY|LYNX)", re.IGNORECASE
)
_PATTERN_GLIDER = re.compile(
    r"^(ASW|ASK|ASH|ASG|DG[0-9]|GROB|LS[0-9]|G10|K7|K8|DUO|JS[0-9]?|"
    r"SF25|DISCUS|JAN|VENT|ARC|NIM|PIK)", re.IGNORECASE
)
# Fighter / military jet type codes — explicit set rather than a loose
# "F[0-9]" pattern that would misclassify the Dassault Falcon family
# (F900, F2TH, F7X, F10X — all bizjets) as fighters.
_FIGHTER_TYPES = {
    # US fighters
    "F4", "F5", "F14", "F15", "F16", "F18", "F22", "F35", "F117",
    "A10",
    # European
    "EUFI", "TYPH", "TYPE", "RAFL", "MIR2", "MR2", "MIR3",
    "JAS39", "JAS", "GRIP",
    # Russian / Chinese
    "MIG29", "MG29", "MIG31", "MG31",
    "SU27", "SU30", "SU33", "SU34", "SU35", "SU57",
    "J10", "J11", "J15", "J20", "JH7",
}


def infer_category_from_type(type_code):
    """Best-effort wake-vortex category from an ICAO type designator.

    Returns one of "A1".."A7", "B1".."B6" or None if no rule fires.
    """
    if not type_code:
        return None
    if _PATTERN_HELI.match(type_code):
        return "A7"
    if _PATTERN_GLIDER.match(type_code):
        return "B1"
    if type_code.upper() in _FIGHTER_TYPES:
        return "A6"
    return None


class AircraftDB:
    """Read-only wrapper around the SQLite enrichment database.

    Lazily opens the connection on first lookup. Thread-safe: sqlite3
    connections can be shared across threads in 'check_same_thread=False'
    mode, and we add a tiny lock around `execute` to keep cursor state
    sane under contention from the batch writer thread.
    """

    def __init__(self, path=DEFAULT_DB_PATH):
        self.path = path
        self._conn = None
        self._lock = threading.Lock()
        self._available = os.path.exists(path)

    @property
    def available(self):
        return self._available

    def _ensure_conn(self):
        if self._conn is None and self._available:
            try:
                self._conn = sqlite3.connect(
                    self.path,
                    check_same_thread=False,
                    isolation_level=None,        # autocommit; we only read
                )
                # Read-only friendly pragmas.
                self._conn.execute("PRAGMA query_only = 1")
            except sqlite3.Error:
                self._available = False
                self._conn = None
        return self._conn

    def lookup(self, icao):
        """Return a dict with enrichment fields, or None.

        Keys when found:
          registration  -> str (e.g. "F-GZNT")
          type_code     -> str (e.g. "B772")
          is_military   -> bool
          category      -> str|None — inferred wake-vortex category from
                                       type_code (helicopter / glider /
                                       fighter only). Caller uses this as
                                       fallback if the ADS-B frame didn't
                                       carry its own category.
        """
        if not icao:
            return None
        conn = self._ensure_conn()
        if conn is None:
            return None
        try:
            with self._lock:
                cur = conn.execute(
                    "SELECT registration, type_code, is_military "
                    "FROM aircraft WHERE icao = ?",
                    (icao.upper(),),
                )
                row = cur.fetchone()
        except sqlite3.Error:
            return None
        if row is None:
            return None
        reg, type_code, is_mil = row
        return {
            "registration": reg or None,
            "type_code":    type_code or None,
            "is_military":  bool(is_mil),
            "category":     infer_category_from_type(type_code),
        }


# Module-level singleton, instantiated at import time so listen_sdr.py
# can import and use it without any boilerplate.
DB = AircraftDB()
