"""
Aircraft database lookup helper for ADS-B enrichment.

Provides four classes of enrichment, all derived from local data:

1. By ICAO 24-bit address, the per-aircraft record from the Mictronics
   dataset (registration, type code, military flag).
2. By the prefix of the callsign field, the airline operator (name,
   country, phonetic callsign) — from Mictronics' operators.json.
3. By the ICAO type designator (e.g. "B738"), the human-readable model
   name ("Boeing 737-800"), the ICAO class ("L2J" = Land/2-engine/Jet)
   and the weight category ("M" = medium) — from Mictronics' types.json.
4. By the ICAO 24-bit address alone, the country of registration via the
   ITU/ICAO Annex 10 Vol III address-allocation table (static, embedded
   below). This is independent of the SQLite DB and always available.

If the SQLite file isn't present (Phase A-only install) the per-row
lookups gracefully return None — the country-from-ICAO logic still works.
"""

import bisect
import os
import re
import sqlite3
import threading

DEFAULT_DB_PATH = os.path.expanduser(
    "~/.local/share/sdr_map_launcher/aircraft_db.sqlite"
)

_PATTERN_HELI = re.compile(
    r"^(EC|AS3|AS5|AS6|R22|R44|R66|S6|S7|S9|S5|H47|H53|H60|H64|H65|"
    r"AW|BK17|MI8|MI17|NH90|TIGR|GAZL|PUMA|B06|B41|B43|MD5|MD60|MD90|"
    r"DJI|UH1|HUEY|LYNX)", re.IGNORECASE
)
_PATTERN_GLIDER = re.compile(
    r"^(ASW|ASK|ASH|ASG|DG[0-9]|GROB|LS[0-9]|G10|K7|K8|DUO|JS[0-9]?|"
    r"SF25|DISCUS|JAN|VENT|ARC|NIM|PIK)", re.IGNORECASE
)
_FIGHTER_TYPES = {
    "F4", "F5", "F14", "F15", "F16", "F18", "F22", "F35", "F117", "A10",
    "EUFI", "TYPH", "TYPE", "RAFL", "MIR2", "MR2", "MIR3",
    "JAS39", "JAS", "GRIP",
    "MIG29", "MG29", "MIG31", "MG31",
    "SU27", "SU30", "SU33", "SU34", "SU35", "SU57",
    "J10", "J11", "J15", "J20", "JH7",
}


def infer_category_from_type(type_code):
    """Best-effort wake-vortex category from an ICAO type designator."""
    if not type_code:
        return None
    if _PATTERN_HELI.match(type_code):
        return "A7"
    if _PATTERN_GLIDER.match(type_code):
        return "B1"
    if type_code.upper() in _FIGHTER_TYPES:
        return "A6"
    return None


# ICAO 24-bit address allocation table per ICAO Annex 10 Vol III.
# Sorted by `start` so bisect can locate the range in O(log N).
_ICAO_RANGES = [
    (0x004000, 0x0043FF, "ZW", "Zimbabwe"),
    (0x006000, 0x006FFF, "MZ", "Mozambique"),
    (0x008000, 0x00FFFF, "ZA", "South Africa"),
    (0x010000, 0x017FFF, "EG", "Egypt"),
    (0x018000, 0x01FFFF, "LY", "Libya"),
    (0x020000, 0x027FFF, "MA", "Morocco"),
    (0x028000, 0x02FFFF, "TN", "Tunisia"),
    (0x030000, 0x0303FF, "BW", "Botswana"),
    (0x032000, 0x032FFF, "BI", "Burundi"),
    (0x034000, 0x034FFF, "CM", "Cameroon"),
    (0x035000, 0x0353FF, "KM", "Comoros"),
    (0x036000, 0x036FFF, "CG", "Congo"),
    (0x038000, 0x038FFF, "CI", "Côte d'Ivoire"),
    (0x03E000, 0x03EFFF, "GA", "Gabon"),
    (0x040000, 0x040FFF, "ET", "Ethiopia"),
    (0x042000, 0x042FFF, "GQ", "Equatorial Guinea"),
    (0x044000, 0x044FFF, "GH", "Ghana"),
    (0x046000, 0x046FFF, "GN", "Guinea"),
    (0x048000, 0x0483FF, "GW", "Guinea-Bissau"),
    (0x04A000, 0x04A3FF, "LS", "Lesotho"),
    (0x04C000, 0x04CFFF, "KE", "Kenya"),
    (0x050000, 0x050FFF, "LR", "Liberia"),
    (0x054000, 0x054FFF, "MG", "Madagascar"),
    (0x058000, 0x058FFF, "MW", "Malawi"),
    (0x05A000, 0x05A3FF, "MV", "Maldives"),
    (0x05C000, 0x05CFFF, "ML", "Mali"),
    (0x05E000, 0x05E3FF, "MR", "Mauritania"),
    (0x060000, 0x0603FF, "MU", "Mauritius"),
    (0x062000, 0x062FFF, "NE", "Niger"),
    (0x064000, 0x064FFF, "NG", "Nigeria"),
    (0x068000, 0x068FFF, "UG", "Uganda"),
    (0x06A000, 0x06A3FF, "QA", "Qatar"),
    (0x06C000, 0x06CFFF, "CF", "Central African Rep."),
    (0x06E000, 0x06EFFF, "RW", "Rwanda"),
    (0x070000, 0x070FFF, "SN", "Senegal"),
    (0x074000, 0x0743FF, "SC", "Seychelles"),
    (0x076000, 0x0763FF, "SL", "Sierra Leone"),
    (0x078000, 0x078FFF, "SO", "Somalia"),
    (0x07A000, 0x07A3FF, "SZ", "Eswatini"),
    (0x07C000, 0x07CFFF, "SD", "Sudan"),
    (0x080000, 0x080FFF, "TZ", "Tanzania"),
    (0x084000, 0x084FFF, "TD", "Chad"),
    (0x088000, 0x088FFF, "TG", "Togo"),
    (0x08A000, 0x08AFFF, "ZM", "Zambia"),
    (0x08C000, 0x08CFFF, "CD", "Congo (DRC)"),
    (0x090000, 0x0903FF, "AO", "Angola"),
    (0x094000, 0x0943FF, "BJ", "Benin"),
    (0x096000, 0x0963FF, "CV", "Cape Verde"),
    (0x098000, 0x0983FF, "DJ", "Djibouti"),
    (0x09A000, 0x09AFFF, "GM", "Gambia"),
    (0x09C000, 0x09C3FF, "BF", "Burkina Faso"),
    (0x09E000, 0x09E3FF, "ST", "Sao Tome and Principe"),
    (0x0A0000, 0x0A7FFF, "DZ", "Algeria"),
    (0x0A8000, 0x0A8FFF, "BS", "Bahamas"),
    (0x0AA000, 0x0AA3FF, "BB", "Barbados"),
    (0x0AB000, 0x0AB3FF, "BZ", "Belize"),
    (0x0AC000, 0x0ACFFF, "CO", "Colombia"),
    (0x0AE000, 0x0AEFFF, "CR", "Costa Rica"),
    (0x0B0000, 0x0B0FFF, "CU", "Cuba"),
    (0x0B2000, 0x0B2FFF, "SV", "El Salvador"),
    (0x0B4000, 0x0B4FFF, "GT", "Guatemala"),
    (0x0B6000, 0x0B6FFF, "GY", "Guyana"),
    (0x0B8000, 0x0B8FFF, "HT", "Haiti"),
    (0x0BA000, 0x0BAFFF, "HN", "Honduras"),
    (0x0BC000, 0x0BC3FF, "VC", "Saint Vincent & Grenadines"),
    (0x0BE000, 0x0BEFFF, "JM", "Jamaica"),
    (0x0C0000, 0x0C0FFF, "NI", "Nicaragua"),
    (0x0C2000, 0x0C2FFF, "PA", "Panama"),
    (0x0C4000, 0x0C4FFF, "DO", "Dominican Republic"),
    (0x0C6000, 0x0C6FFF, "TT", "Trinidad and Tobago"),
    (0x0C8000, 0x0C8FFF, "SR", "Suriname"),
    (0x0CA000, 0x0CA3FF, "AG", "Antigua and Barbuda"),
    (0x0CC000, 0x0CC3FF, "GD", "Grenada"),
    (0x100000, 0x1FFFFF, "RU", "Russia"),
    (0x201000, 0x2013FF, "NA", "Namibia"),
    (0x202000, 0x2023FF, "ER", "Eritrea"),
    (0x300000, 0x33FFFF, "IT", "Italy"),
    (0x340000, 0x37FFFF, "ES", "Spain"),
    (0x380000, 0x3BFFFF, "FR", "France"),
    (0x3C0000, 0x3FFFFF, "DE", "Germany"),
    (0x400000, 0x43FFFF, "GB", "United Kingdom"),
    (0x440000, 0x447FFF, "AT", "Austria"),
    (0x448000, 0x44FFFF, "BE", "Belgium"),
    (0x450000, 0x457FFF, "BG", "Bulgaria"),
    (0x458000, 0x45FFFF, "DK", "Denmark"),
    (0x460000, 0x467FFF, "FI", "Finland"),
    (0x468000, 0x46FFFF, "GR", "Greece"),
    (0x470000, 0x477FFF, "HU", "Hungary"),
    (0x478000, 0x47FFFF, "NO", "Norway"),
    (0x480000, 0x487FFF, "NL", "Netherlands"),
    (0x488000, 0x48FFFF, "PL", "Poland"),
    (0x490000, 0x497FFF, "PT", "Portugal"),
    (0x498000, 0x49FFFF, "CZ", "Czech Republic"),
    (0x4A0000, 0x4A7FFF, "RO", "Romania"),
    (0x4A8000, 0x4AFFFF, "SE", "Sweden"),
    (0x4B0000, 0x4B7FFF, "CH", "Switzerland"),
    (0x4B8000, 0x4BFFFF, "TR", "Turkey"),
    (0x4C0000, 0x4C7FFF, "RS", "Serbia"),
    (0x4C8000, 0x4C83FF, "CY", "Cyprus"),
    (0x4CA000, 0x4CAFFF, "IE", "Ireland"),
    (0x4CC000, 0x4CCFFF, "IS", "Iceland"),
    (0x4D0000, 0x4D03FF, "LU", "Luxembourg"),
    (0x4D2000, 0x4D2FFF, "MT", "Malta"),
    (0x4D4000, 0x4D43FF, "MC", "Monaco"),
    (0x500000, 0x5003FF, "SM", "San Marino"),
    (0x501000, 0x5013FF, "AL", "Albania"),
    (0x502000, 0x5023FF, "HR", "Croatia"),
    (0x503000, 0x5033FF, "LV", "Latvia"),
    (0x504000, 0x5043FF, "LT", "Lithuania"),
    (0x505000, 0x5053FF, "MD", "Moldova"),
    (0x506000, 0x5063FF, "SK", "Slovakia"),
    (0x507000, 0x5073FF, "SI", "Slovenia"),
    (0x508000, 0x508FFF, "UZ", "Uzbekistan"),
    (0x509000, 0x5093FF, "UA", "Ukraine"),
    (0x50A000, 0x50A3FF, "BY", "Belarus"),
    (0x50C000, 0x50CFFF, "EE", "Estonia"),
    (0x50D000, 0x50D3FF, "MK", "North Macedonia"),
    (0x50E000, 0x50E3FF, "BA", "Bosnia and Herzegovina"),
    (0x50F000, 0x50F3FF, "GE", "Georgia"),
    (0x510000, 0x5103FF, "TJ", "Tajikistan"),
    (0x511000, 0x5113FF, "ME", "Montenegro"),
    (0x600000, 0x6003FF, "AM", "Armenia"),
    (0x600800, 0x600BFF, "AZ", "Azerbaijan"),
    (0x601000, 0x6013FF, "KG", "Kyrgyzstan"),
    (0x602000, 0x6023FF, "TM", "Turkmenistan"),
    (0x680000, 0x6803FF, "BT", "Bhutan"),
    (0x682000, 0x6823FF, "MN", "Mongolia"),
    (0x683000, 0x6833FF, "KZ", "Kazakhstan"),
    (0x700000, 0x700FFF, "AF", "Afghanistan"),
    (0x702000, 0x702FFF, "BD", "Bangladesh"),
    (0x704000, 0x704FFF, "MM", "Myanmar"),
    (0x706000, 0x706FFF, "KW", "Kuwait"),
    (0x708000, 0x708FFF, "LA", "Laos"),
    (0x70A000, 0x70AFFF, "NP", "Nepal"),
    (0x70C000, 0x70C3FF, "OM", "Oman"),
    (0x70E000, 0x70EFFF, "KH", "Cambodia"),
    (0x710000, 0x717FFF, "SA", "Saudi Arabia"),
    (0x718000, 0x71FFFF, "KR", "South Korea"),
    (0x720000, 0x727FFF, "KP", "North Korea"),
    (0x728000, 0x72FFFF, "IQ", "Iraq"),
    (0x730000, 0x737FFF, "IR", "Iran"),
    (0x738000, 0x73FFFF, "IL", "Israel"),
    (0x740000, 0x747FFF, "JO", "Jordan"),
    (0x748000, 0x74FFFF, "LB", "Lebanon"),
    (0x750000, 0x757FFF, "MY", "Malaysia"),
    (0x758000, 0x75FFFF, "PH", "Philippines"),
    (0x760000, 0x767FFF, "PK", "Pakistan"),
    (0x768000, 0x76FFFF, "SG", "Singapore"),
    (0x770000, 0x777FFF, "LK", "Sri Lanka"),
    (0x778000, 0x77FFFF, "SY", "Syria"),
    (0x780000, 0x7BFFFF, "CN", "China"),
    (0x7C0000, 0x7FFFFF, "AU", "Australia"),
    (0x800000, 0x83FFFF, "IN", "India"),
    (0x840000, 0x87FFFF, "JP", "Japan"),
    (0x880000, 0x887FFF, "TH", "Thailand"),
    (0x888000, 0x88FFFF, "VN", "Viet Nam"),
    (0x890000, 0x890FFF, "YE", "Yemen"),
    (0x894000, 0x894FFF, "BH", "Bahrain"),
    (0x895000, 0x8953FF, "BN", "Brunei"),
    (0x896000, 0x896FFF, "AE", "United Arab Emirates"),
    (0x898000, 0x898FFF, "PG", "Papua New Guinea"),
    (0x899000, 0x8993FF, "TW", "Taiwan"),
    (0x8A0000, 0x8A7FFF, "ID", "Indonesia"),
    (0xA00000, 0xAFFFFF, "US", "United States"),
    (0xC00000, 0xC3FFFF, "CA", "Canada"),
    (0xC80000, 0xC87FFF, "NZ", "New Zealand"),
    (0xC88000, 0xC88FFF, "FJ", "Fiji"),
    (0xC8A000, 0xC8A3FF, "NR", "Nauru"),
    (0xC8C000, 0xC8C3FF, "LC", "Saint Lucia"),
    (0xC8D000, 0xC8D3FF, "TO", "Tonga"),
    (0xC8E000, 0xC8E3FF, "KI", "Kiribati"),
    (0xC90000, 0xC903FF, "VU", "Vanuatu"),
    (0xE00000, 0xE3FFFF, "AR", "Argentina"),
    (0xE40000, 0xE7FFFF, "BR", "Brazil"),
    (0xE80000, 0xE80FFF, "CL", "Chile"),
    (0xE84000, 0xE84FFF, "EC", "Ecuador"),
    (0xE88000, 0xE88FFF, "PY", "Paraguay"),
    (0xE8C000, 0xE8CFFF, "PE", "Peru"),
    (0xE90000, 0xE90FFF, "UY", "Uruguay"),
    (0xE94000, 0xE94FFF, "BO", "Bolivia"),
    (0xE98000, 0xE98FFF, "VE", "Venezuela"),
]
_ICAO_STARTS = [r[0] for r in _ICAO_RANGES]


def country_from_icao(icao_hex):
    """Resolve an ICAO 24-bit hex string to (iso, country_name).

    Returns (None, None) for unallocated/unknown ranges. O(log N) bisect.
    """
    if not icao_hex:
        return (None, None)
    try:
        addr = int(icao_hex, 16)
    except (TypeError, ValueError):
        return (None, None)
    idx = bisect.bisect_right(_ICAO_STARTS, addr) - 1
    if idx < 0:
        return (None, None)
    start, end, iso, name = _ICAO_RANGES[idx]
    if start <= addr <= end:
        return (iso, name)
    return (None, None)


class AircraftDB:
    """Read-only wrapper around the SQLite enrichment database."""

    def __init__(self, path=DEFAULT_DB_PATH):
        self.path = path
        self._conn = None
        self._lock = threading.Lock()
        self._available = os.path.exists(path)

    @property
    def available(self):
        # Re-check disk on every access so the DB becomes usable the moment
        # build_aircraft_db.py finishes writing — no Django restart needed.
        if not self._available and os.path.exists(self.path):
            self._available = True
        return self._available

    def _ensure_conn(self):
        if self._conn is None and self.available:
            try:
                self._conn = sqlite3.connect(
                    self.path,
                    check_same_thread=False,
                    isolation_level=None,
                )
                self._conn.execute("PRAGMA query_only = 1")
            except sqlite3.Error:
                self._available = False
                self._conn = None
        return self._conn

    def _close(self):
        """Close the open connection so the SQLite file can be replaced.

        Called by the /api/aircraft_db/update endpoint just before the
        build script runs. The next lookup() call will lazily reopen via
        _ensure_conn(). Safe to call when no connection is open.

        Important on Windows: SQLite holds the file open even in read-only
        mode, so the new aircraft_db.sqlite cannot replace the old one
        until this close happens. On Linux it's not strictly required
        (you can unlink an open file), but explicit close is cleaner.
        """
        with self._lock:
            if self._conn is not None:
                try:
                    self._conn.close()
                except sqlite3.Error:
                    pass
                self._conn = None

    def _fetchone(self, sql, params):
        conn = self._ensure_conn()
        if conn is None:
            return None
        try:
            with self._lock:
                row = conn.execute(sql, params).fetchone()
        except sqlite3.Error:
            return None
        return row

    def lookup(self, icao):
        if not icao:
            return None
        row = self._fetchone(
            "SELECT registration, type_code, is_military "
            "FROM aircraft WHERE icao = ?",
            (icao.upper(),),
        )
        if row is None:
            return None
        reg, type_code, is_mil = row
        return {
            "registration": reg or None,
            "type_code":    type_code or None,
            "is_military":  bool(is_mil),
            "category":     infer_category_from_type(type_code),
        }

    _CALLSIGN_PREFIX = re.compile(r"^([A-Z]{3})[0-9]")

    def lookup_operator(self, callsign):
        """Airline operator from a callsign (e.g. AFR1234 -> Air France)."""
        if not callsign:
            return None
        m = self._CALLSIGN_PREFIX.match(callsign.upper())
        if not m:
            return None
        row = self._fetchone(
            "SELECT name, country, callsign FROM operators WHERE code = ?",
            (m.group(1),),
        )
        if row is None:
            return None
        name, country, phonetic = row
        return {
            "name":     name or None,
            "country":  country or None,
            "callsign": phonetic or None,
        }

    def lookup_type(self, type_code):
        """Detailed model information for an ICAO type designator."""
        if not type_code:
            return None
        row = self._fetchone(
            "SELECT description, icao_class, weight "
            "FROM types WHERE type_code = ?",
            (type_code.upper(),),
        )
        if row is None:
            return None
        desc, cls, weight = row
        return {
            "description": desc or None,
            "icao_class":  cls or None,
            "weight":      weight or None,
        }


DB = AircraftDB()
