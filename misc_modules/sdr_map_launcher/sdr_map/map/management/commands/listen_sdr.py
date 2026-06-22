# map/management/commands/listen_sdr.py
#
# SINGLE TCP server that receives objects decoded by ALL SDR++ modules
# (ais_decoder, adsb_decoder, aprs_decoder, dsd_decoder, radiosonde) on the SAME port.
#
# Each SDR++ module connects AS A CLIENT to this server (the module's
# "TCP output") and sends one JSON line per positioned object, terminated by '\n'.
# The server accepts as many simultaneous connections as needed (one per
# module instance: AIS channel 1, AIS channel 2, ADS-B, APRS, ...), all on
# the same listening port.
#
# Dispatch is based on the "type" field of each JSON line:
#   - "AIS"        : ships             (+ "mmsi" field)
#   - "ADSB"       : aircraft          (+ "icao" field, info: alt=/hdg=/vrate=)
#   - "APRS"       : stations / objects
#   - "APRS Meteo" : weather stations  (+ weather fields, instead of "speed")
#
# Launch:
#   python manage.py listen_sdr                  # 0.0.0.0:10100 by default
#   python manage.py listen_sdr --host 0.0.0.0 --port 10100
#
# SDR++ side: point the "TCP output" of EACH module to server_IP:10100.

import json
import re
import socket
import socketserver
import threading

from django.conf import settings
from django.core.management.base import BaseCommand
from django.db import transaction, connections
from django.utils import timezone

from map.models import SdrObject, SdrObjectTrack
from map.services.aircraft_lookup import DB as _AIRCRAFT_DB, country_from_icao

try:
    from channels.layers import get_channel_layer
    from asgiref.sync import async_to_sync
    _CHANNEL_LAYER = get_channel_layer()
except Exception:  # pragma: no cover - Channels is always present here
    _CHANNEL_LAYER = None

GROUP_NAME = "sdr_objects"

# Regexes to extract useful fields from the "info" string of
# ADS-B frames (e.g. "alt=38000ft hdg=270 vrate=-832fpm cs=AFR1234").
_RE_ALT = re.compile(r"alt=(-?\d+)\s*ft", re.IGNORECASE)
_RE_HDG = re.compile(r"hdg=(-?\d+(?:\.\d+)?)", re.IGNORECASE)
# Course Over Ground emitted by the AIS module inside info, e.g.
# "MMSI=227006760 msg=1 COG=87.5 HDG=88 nav=... ship=Cargo"
_RE_COG = re.compile(r"COG=(-?\d+(?:\.\d+)?)", re.IGNORECASE)
# ADS-B wake-vortex category, optionally packed inside the info string by
# the decoder (e.g. "hdg=270 alt_ft=35000 cat=A3"). The category code is
# 1 letter (A/B/C) + 1 digit (0-9). Accepted as either `cat=` or
# `category=`. The match is loose: "A3", "a3", "B6" all valid.
_RE_CATEGORY = re.compile(r"\bcat(?:egory)?=([ABCabc][0-9])", re.IGNORECASE)
# TETRA LIP messages carry direction as "dir=92deg" (course over ground)
# and GPS accuracy as "acc=20m". Both extracted from the info string.
_RE_DIR_DEG = re.compile(r"dir=(-?\d+(?:\.\d+)?)\s*deg", re.IGNORECASE)
_RE_ACC_M   = re.compile(r"acc=(-?\d+(?:\.\d+)?)\s*m\b", re.IGNORECASE)

# Radiosonde sub-fields "key=value" inside the info string, e.g.:
#   "alt=12345;hdg=270;climb=5.1;temp=-40.5;rh=45;p=210.0"
_RE_INFO_KV = re.compile(r"([A-Za-z_]+)\s*=\s*(-?\d+(?:\.\d+)?)")


def _parse_info_kv(info):
    """Extract {lowercase_key: float} dict from key=value pairs in info."""
    out = {}
    for key, val in _RE_INFO_KV.findall(info or ""):
        try:
            out[key.lower()] = float(val)
        except ValueError:
            pass
    return out


# --------------------------------------------------------------------------- #
# AIS MMSI classification (ITU-R M.585).
#
# A 9-digit MMSI is decoded by looking at its leading digits:
#   00MIDxxxx  -> coast station / shore radio
#   0MIDxxxxx  -> group of ships (flotilla)
#   111MIDxxx  -> SAR aircraft
#   970xxxxxx  -> AIS-SART (distress transponder)
#   972xxxxxx  -> MOB (Man Over Board) device
#   974xxxxxx  -> AIS-EPIRB (emergency beacon)
#   98xxxxxxx  -> craft associated with a parent ship (tender)
#   99MIDxxxx  -> AtoN (Aid to Navigation: buoy, lighthouse...)
#   MIDxxxxxx  -> regular ship (first 3 digits = MID = country code)
#
# Subcategory returned in extra.mmsi_kind; readable label in extra.mmsi_label.
# --------------------------------------------------------------------------- #

# Minimal MID -> country/flag table. Easy to extend.
# Source: ITU MID list (kept short — covers most likely traffic on European
# coastlines + a few common flags of convenience).
_MID_TABLE = {
    201: ("AL", "Albania"),         202: ("AD", "Andorra"),
    203: ("AT", "Austria"),         204: ("PT", "Azores"),
    205: ("BE", "Belgium"),         206: ("BY", "Belarus"),
    207: ("BG", "Bulgaria"),        208: ("VA", "Vatican"),
    209: ("CY", "Cyprus"),          210: ("CY", "Cyprus"),
    211: ("DE", "Germany"),         212: ("CY", "Cyprus"),
    213: ("GE", "Georgia"),         214: ("MD", "Moldova"),
    215: ("MT", "Malta"),           218: ("DE", "Germany"),
    219: ("DK", "Denmark"),         220: ("DK", "Denmark"),
    224: ("ES", "Spain"),           225: ("ES", "Spain"),
    226: ("FR", "France"),          227: ("FR", "France"),
    228: ("FR", "France"),          229: ("MT", "Malta"),
    230: ("FI", "Finland"),         231: ("FO", "Faroe Islands"),
    232: ("GB", "United Kingdom"),  233: ("GB", "United Kingdom"),
    234: ("GB", "United Kingdom"),  235: ("GB", "United Kingdom"),
    236: ("GI", "Gibraltar"),       237: ("GR", "Greece"),
    238: ("HR", "Croatia"),         239: ("GR", "Greece"),
    240: ("GR", "Greece"),          241: ("GR", "Greece"),
    242: ("MA", "Morocco"),         243: ("HU", "Hungary"),
    244: ("NL", "Netherlands"),     245: ("NL", "Netherlands"),
    246: ("NL", "Netherlands"),     247: ("IT", "Italy"),
    248: ("MT", "Malta"),           249: ("MT", "Malta"),
    250: ("IE", "Ireland"),         251: ("IS", "Iceland"),
    252: ("LI", "Liechtenstein"),   253: ("LU", "Luxembourg"),
    254: ("MC", "Monaco"),          255: ("PT", "Madeira"),
    256: ("MT", "Malta"),           257: ("NO", "Norway"),
    258: ("NO", "Norway"),          259: ("NO", "Norway"),
    261: ("PL", "Poland"),          262: ("ME", "Montenegro"),
    263: ("PT", "Portugal"),        264: ("RO", "Romania"),
    265: ("SE", "Sweden"),          266: ("SE", "Sweden"),
    267: ("SK", "Slovakia"),        268: ("SM", "San Marino"),
    269: ("CH", "Switzerland"),     270: ("CZ", "Czech Republic"),
    271: ("TR", "Turkey"),          272: ("UA", "Ukraine"),
    273: ("RU", "Russia"),          274: ("MK", "North Macedonia"),
    275: ("LV", "Latvia"),          276: ("EE", "Estonia"),
    277: ("LT", "Lithuania"),       278: ("SI", "Slovenia"),
    279: ("RS", "Serbia"),
    301: ("AI", "Anguilla"),        303: ("US", "Alaska (US)"),
    304: ("AG", "Antigua"),         305: ("AG", "Antigua"),
    306: ("CW", "Curaçao"),         307: ("AW", "Aruba"),
    308: ("BS", "Bahamas"),         309: ("BS", "Bahamas"),
    311: ("BS", "Bahamas"),         312: ("BZ", "Belize"),
    314: ("BB", "Barbados"),        316: ("CA", "Canada"),
    319: ("KY", "Cayman Is."),      321: ("CR", "Costa Rica"),
    323: ("CU", "Cuba"),            325: ("DM", "Dominica"),
    327: ("DO", "Dominican Rep."),  329: ("GP", "Guadeloupe"),
    330: ("GD", "Grenada"),         331: ("GL", "Greenland"),
    332: ("GT", "Guatemala"),       334: ("HN", "Honduras"),
    336: ("HT", "Haiti"),           338: ("US", "United States"),
    339: ("JM", "Jamaica"),         341: ("KN", "St Kitts & Nevis"),
    343: ("LC", "St Lucia"),        345: ("MX", "Mexico"),
    347: ("MQ", "Martinique"),      348: ("MS", "Montserrat"),
    350: ("NI", "Nicaragua"),       351: ("PA", "Panama"),
    352: ("PA", "Panama"),          353: ("PA", "Panama"),
    354: ("PA", "Panama"),          355: ("PA", "Panama"),
    356: ("PA", "Panama"),          357: ("PA", "Panama"),
    358: ("PR", "Puerto Rico"),     359: ("SV", "El Salvador"),
    361: ("PM", "St Pierre"),       362: ("TT", "Trinidad & Tobago"),
    364: ("TC", "Turks & Caicos"),  366: ("US", "United States"),
    367: ("US", "United States"),   368: ("US", "United States"),
    369: ("US", "United States"),   370: ("PA", "Panama"),
    371: ("PA", "Panama"),          372: ("PA", "Panama"),
    373: ("PA", "Panama"),          374: ("PA", "Panama"),
    375: ("VC", "St Vincent"),      376: ("VC", "St Vincent"),
    377: ("VC", "St Vincent"),      378: ("VG", "BVI"),
    379: ("VI", "USVI"),
    401: ("AF", "Afghanistan"),     403: ("SA", "Saudi Arabia"),
    405: ("BD", "Bangladesh"),      408: ("BH", "Bahrain"),
    410: ("BT", "Bhutan"),          412: ("CN", "China"),
    413: ("CN", "China"),           414: ("CN", "China"),
    416: ("TW", "Taiwan"),          417: ("LK", "Sri Lanka"),
    419: ("IN", "India"),           422: ("IR", "Iran"),
    423: ("AZ", "Azerbaijan"),      425: ("IQ", "Iraq"),
    428: ("IL", "Israel"),          431: ("JP", "Japan"),
    432: ("JP", "Japan"),           434: ("TM", "Turkmenistan"),
    436: ("KZ", "Kazakhstan"),      437: ("UZ", "Uzbekistan"),
    438: ("JO", "Jordan"),          440: ("KR", "South Korea"),
    441: ("KR", "South Korea"),     443: ("PS", "Palestine"),
    445: ("KP", "North Korea"),     447: ("KW", "Kuwait"),
    450: ("LB", "Lebanon"),         451: ("KG", "Kyrgyzstan"),
    453: ("MO", "Macao"),           455: ("MV", "Maldives"),
    457: ("MN", "Mongolia"),        459: ("NP", "Nepal"),
    461: ("OM", "Oman"),            463: ("PK", "Pakistan"),
    466: ("QA", "Qatar"),           468: ("SY", "Syria"),
    470: ("AE", "UAE"),             472: ("TJ", "Tajikistan"),
    473: ("YE", "Yemen"),           475: ("YE", "Yemen"),
    477: ("HK", "Hong Kong"),       478: ("BA", "Bosnia"),
    501: ("AQ", "Adelie Land"),     503: ("AU", "Australia"),
    506: ("MM", "Myanmar"),         508: ("BN", "Brunei"),
    510: ("FM", "Micronesia"),      511: ("PW", "Palau"),
    512: ("NZ", "New Zealand"),     514: ("KH", "Cambodia"),
    515: ("KH", "Cambodia"),        516: ("CX", "Christmas Is."),
    518: ("CK", "Cook Is."),        520: ("FJ", "Fiji"),
    523: ("CC", "Cocos Is."),       525: ("ID", "Indonesia"),
    529: ("KI", "Kiribati"),        531: ("LA", "Laos"),
    533: ("MY", "Malaysia"),        536: ("MP", "Northern Marianas"),
    538: ("MH", "Marshall Is."),    540: ("NC", "New Caledonia"),
    542: ("NU", "Niue"),            544: ("NR", "Nauru"),
    546: ("PF", "French Polynesia"),548: ("PH", "Philippines"),
    553: ("PG", "Papua New Guinea"),555: ("PN", "Pitcairn"),
    557: ("SB", "Solomon Is."),     559: ("AS", "American Samoa"),
    561: ("WS", "Samoa"),           563: ("SG", "Singapore"),
    564: ("SG", "Singapore"),       565: ("SG", "Singapore"),
    566: ("SG", "Singapore"),       567: ("TH", "Thailand"),
    570: ("TO", "Tonga"),           572: ("TV", "Tuvalu"),
    574: ("VN", "Vietnam"),         576: ("VU", "Vanuatu"),
    577: ("VU", "Vanuatu"),         578: ("WF", "Wallis & Futuna"),
    601: ("ZA", "South Africa"),    603: ("AO", "Angola"),
    605: ("DZ", "Algeria"),         607: ("TF", "French S. Territories"),
    608: ("IO", "Ascension"),       609: ("BI", "Burundi"),
    610: ("BJ", "Benin"),           611: ("BW", "Botswana"),
    612: ("CF", "Central African Rep."),613: ("CM", "Cameroon"),
    615: ("CG", "Congo"),           616: ("KM", "Comoros"),
    617: ("CV", "Cape Verde"),      618: ("AQ", "Crozet"),
    619: ("CI", "Ivory Coast"),     620: ("KM", "Comoros"),
    621: ("DJ", "Djibouti"),        622: ("EG", "Egypt"),
    624: ("ET", "Ethiopia"),        625: ("ER", "Eritrea"),
    626: ("GA", "Gabon"),           627: ("GH", "Ghana"),
    629: ("GM", "Gambia"),          630: ("GW", "Guinea-Bissau"),
    631: ("GQ", "Equatorial Guinea"),632: ("GN", "Guinea"),
    633: ("BF", "Burkina Faso"),    634: ("KE", "Kenya"),
    635: ("AQ", "Kerguelen"),       636: ("LR", "Liberia"),
    637: ("LR", "Liberia"),         638: ("SS", "South Sudan"),
    642: ("LY", "Libya"),           644: ("LS", "Lesotho"),
    645: ("MU", "Mauritius"),       647: ("MG", "Madagascar"),
    649: ("ML", "Mali"),            650: ("MZ", "Mozambique"),
    654: ("MR", "Mauritania"),      655: ("MW", "Malawi"),
    656: ("NE", "Niger"),           657: ("NG", "Nigeria"),
    659: ("NA", "Namibia"),         660: ("RE", "Réunion"),
    661: ("RW", "Rwanda"),          662: ("SD", "Sudan"),
    663: ("SN", "Senegal"),         664: ("SC", "Seychelles"),
    665: ("SH", "St Helena"),       666: ("SO", "Somalia"),
    667: ("SL", "Sierra Leone"),    668: ("ST", "São Tomé"),
    669: ("SZ", "Eswatini"),        670: ("TD", "Chad"),
    671: ("TG", "Togo"),            672: ("TN", "Tunisia"),
    674: ("TZ", "Tanzania"),        675: ("UG", "Uganda"),
    676: ("CD", "DR Congo"),        677: ("TZ", "Tanzania"),
    678: ("ZM", "Zambia"),          679: ("ZW", "Zimbabwe"),
    701: ("AR", "Argentina"),       710: ("BR", "Brazil"),
    720: ("BO", "Bolivia"),         725: ("CL", "Chile"),
    730: ("CO", "Colombia"),        735: ("EC", "Ecuador"),
    740: ("FK", "Falklands"),       745: ("GF", "French Guiana"),
    750: ("GY", "Guyana"),          755: ("PY", "Paraguay"),
    760: ("PE", "Peru"),            765: ("SR", "Suriname"),
    770: ("UY", "Uruguay"),         775: ("VE", "Venezuela"),
}


# Reverse lookup: country name (upper-case) -> ISO 2-letter code.
# Derived from the MID table above; covers ~190 countries. Used by the
# Cospas-Sarsat ingestion: beacons carry "country=France" in their info
# string, and we want to render the same flag SVGs as AIS does. A few
# common name variants are added by hand below.
_COUNTRY_NAME_TO_ISO = {}
for _iso, _name in _MID_TABLE.values():
    _COUNTRY_NAME_TO_ISO.setdefault(_name.upper(), _iso)
# Aliases: Cospas-Sarsat uses some country names that don't match the AIS
# table exactly. Add the most common ones.
for _alt, _iso in {
    "UNITED STATES": "US", "USA": "US", "UNITED STATES OF AMERICA": "US",
    "UNITED KINGDOM": "GB", "UK": "GB", "GREAT BRITAIN": "GB",
    "SOUTH KOREA": "KR", "KOREA (REPUBLIC OF)": "KR",
    "NORTH KOREA": "KP", "KOREA (DEM. PEOPLE'S REP. OF)": "KP",
    "RUSSIA": "RU", "RUSSIAN FEDERATION": "RU",
    "CZECH REPUBLIC": "CZ", "CZECHIA": "CZ",
    "VIET NAM": "VN", "VIETNAM": "VN",
    "TAIWAN": "TW",
    "IVORY COAST": "CI", "CÔTE D'IVOIRE": "CI",
    "BURMA": "MM", "MYANMAR": "MM",
    "MOLDOVA": "MD",
    "BOLIVIA": "BO",
    "VENEZUELA": "VE",
    "TANZANIA": "TZ",
}.items():
    _COUNTRY_NAME_TO_ISO.setdefault(_alt, _iso)


def _parse_kv_info(info):
    """Parse a ``key=value;key=value`` string into a dict.

    Used for the Cospas-Sarsat ``info`` field, which packs structured beacon
    metadata as ``beacon=EPIRB;country=France;protocol=...``. Values can
    contain spaces (e.g. "EPIRB with MMSI") but not ``;`` or ``=``.
    """
    out = {}
    for pair in (info or "").split(";"):
        pair = pair.strip()
        if "=" in pair:
            k, v = pair.split("=", 1)
            k = k.strip()
            v = v.strip()
            if k:
                out[k] = v
    return out


def classify_mmsi(mmsi):
    """
    Return (kind, label, mid, country_iso, country_name) for an MMSI.

    `kind` is one of: 'ship', 'aton', 'sar_aircraft', 'sart', 'mob', 'epirb',
    'coast_station', 'group', 'aux_craft'. `mid` is the country code numeric
    or None when not applicable (e.g. SART/EPIRB don't carry a MID).
    """
    if mmsi is None:
        return ("ship", "Ship", None, None, None)
    s = str(mmsi).zfill(9)

    def _mid_lookup(mid_num):
        try:
            mid = int(mid_num)
        except (TypeError, ValueError):
            return (None, None, None)
        cc, name = _MID_TABLE.get(mid, (None, None))
        return (mid, cc, name)

    # Order matters: longer/more specific prefixes first.
    if s.startswith("111"):
        mid, cc, name = _mid_lookup(s[3:6])
        return ("sar_aircraft", "SAR aircraft", mid, cc, name)
    if s.startswith("970"):
        return ("sart", "AIS-SART (distress)", None, None, None)
    if s.startswith("972"):
        return ("mob", "MOB device", None, None, None)
    if s.startswith("974"):
        return ("epirb", "AIS-EPIRB (distress)", None, None, None)
    if s.startswith("99"):
        mid, cc, name = _mid_lookup(s[2:5])
        return ("aton", "Aid to Navigation", mid, cc, name)
    if s.startswith("98"):
        return ("aux_craft", "Auxiliary craft", None, None, None)
    if s.startswith("00"):
        mid, cc, name = _mid_lookup(s[2:5])
        return ("coast_station", "Coast station", mid, cc, name)
    if s.startswith("0"):
        mid, cc, name = _mid_lookup(s[1:4])
        return ("group", "Group of ships", mid, cc, name)
    # Standard ship: first 3 digits = MID.
    mid, cc, name = _mid_lookup(s[:3])
    return ("ship", "Ship", mid, cc, name)


# Possible weather keys on the APRS side (we accept several variants for
# robustness against changes in the aprs_decoder module).
_WEATHER_KEYS = {
    "temp_c": ("temp_c", "temperature", "temp"),
    "humidity": ("humidity", "hum"),
    "wind_dir": ("wind_dir", "wind_direction", "wdir"),
    "wind_speed": ("wind_speed", "wind", "wspeed"),
    "wind_gust": ("wind_gust", "gust", "gusts"),
    "pressure_hpa": ("pressure_hpa", "pressure", "baro"),
    "rain_mm": ("rain_mm", "rain", "rain_1h"),
}

# Fields explicitly mapped (everything else ends up in "extra").
_KNOWN_KEYS = {
    "name", "date", "time", "lat", "lon", "type", "speed",
    "info", "mmsi", "icao", "heading", "altitude_ft",
}
for _variants in _WEATHER_KEYS.values():
    _KNOWN_KEYS.update(_variants)


def _to_float(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def _to_int(value):
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return None


# Match labels emitted by the AIS module before the ship's real name is decoded:
#   "MMSI:227006760"   (colon)
#   "MMSI 227006760"   (space)
#   "MMSI227006760"    (no separator)
# Also catches "227006760" alone (raw MMSI as name).
_RE_MMSI_NAME = re.compile(r"^\s*MMSI[\s:_-]*\d{6,9}\s*$", re.IGNORECASE)


def _looks_like_mmsi_placeholder(name, mmsi):
    """True if `name` carries no real ship name (just the MMSI in disguise).

    Used so an AIS position message doesn't overwrite a previously decoded
    ship name (from a static msg type 5/24) with the bare MMSI placeholder.
    """
    if not name:
        return True
    s = name.strip()
    if _RE_MMSI_NAME.match(s):
        return True
    if mmsi is not None and s == str(mmsi):
        return True
    return False


class Command(BaseCommand):
    help = (
        "Single TCP server receiving objects from all SDR++ modules "
        "(AIS / ADS-B / APRS / LRRP / radiosonde) on the same port and broadcast them to the map."
    )

    def add_arguments(self, parser):
        parser.add_argument("--host", default="0.0.0.0",
                            help="Listen address (default: 0.0.0.0)")
        parser.add_argument("--port", type=int, default=10100,
                            help="Single listen port (default: 10100)")
        parser.add_argument("--no-broadcast", action="store_true",
                            help="Do not broadcast via WebSocket (DB-only mode)")
        parser.add_argument("--quiet", action="store_true",
                            help="Reduce console verbosity")

    # --------------------------------------------------------------------- #
    def handle(self, *args, **opts):
        host, port = opts["host"], opts["port"]
        self.broadcast = not opts["no_broadcast"]
        self.quiet = opts["quiet"]

        # Periodic purge of stale objects (per-type retention).
        self._stop = threading.Event()
        # Serialize DB writes: several modules = several threads, and SQLite
        # does not like concurrent writes. WAL + busy_timeout (settings.py)
        # already handle concurrency; this extra lock prevents any remaining
        # contention and keeps transactions short and ordered.
        self._db_lock = threading.Lock()

        # --- Coalescing + batch writer ---------------------------------------
        # Multiple modules pumping positions concurrently used to saturate the
        # DB lock: a single write cycle (SELECT + INSERT + track INSERT +
        # fsync) takes 30-50 ms on SQLite, so 50 ADS-B updates/sec consume the
        # lock 100% of the time and other modules (satellite tracker, etc.)
        # see their updates starve.
        #
        # Fix: pending updates are coalesced per (obj_type, ident) into a dict
        # — newer position overwrites older — and a single worker thread
        # drains the dict every BATCH_INTERVAL seconds, writing all of them
        # in ONE atomic transaction. Net effect:
        #   - one fsync per cycle instead of one per trame (~10× speedup),
        #   - duplicate positions for the same object within a cycle are
        #     deduplicated for free,
        #   - the TCP-reader threads return instantly (just a dict insert),
        #     never blocked by SQLite contention.
        # The visual delay (< BATCH_INTERVAL) is imperceptible (well below
        # the GPS update cadence of any of the decoders).
        self._pending = {}                       # (obj_type, ident) -> trame
        self._pending_lock = threading.Lock()
        # In-memory cache of the last (lat, lon) recorded per object, to skip
        # the SELECT-last-track query that the batch writer would otherwise
        # have to do for each ident.
        self._last_track_pos = {}                # (obj_type, ident) -> (lat, lon)
        self._BATCH_INTERVAL = 0.15              # 150 ms — see comment above
        writer = threading.Thread(target=self._batch_writer_loop, daemon=True)
        writer.start()
        # ----------------------------------------------------------------------

        purger = threading.Thread(target=self._purge_loop, daemon=True)
        purger.start()

        cmd = self

        class Handler(socketserver.StreamRequestHandler):
            # Wait for the first frame before declaring a module "silent".
            FIRST_FRAME_TIMEOUT = 5.0

            def handle(self):
                import datetime, json as _json
                peer = self.client_address
                opened = datetime.datetime.now()
                cmd._log(cmd.style.SUCCESS(
                    f"+ [{opened:%H:%M:%S}] TCP connection from "
                    f"{peer[0]}:{peer[1]} (module type still unknown)"))

                # If the module stays silent (e.g. a disabled SDR++ plugin that
                # opens the socket on load but doesn't publish anything), tell
                # the operator so they can identify which module is leaking
                # a connection without sending data.
                silent_warn = threading.Timer(
                    self.FIRST_FRAME_TIMEOUT,
                    lambda: cmd._log(cmd.style.WARNING(
                        f"  [{peer[0]}:{peer[1]}] silent for "
                        f"{self.FIRST_FRAME_TIMEOUT:.0f}s — module likely "
                        "DISABLED but opens TCP on load. Disable its 'TCP "
                        "output' in SDR++ to stop the connection.")))
                silent_warn.daemon = True
                silent_warn.start()

                first = True
                try:
                    # rfile reads one line at a time (separator '\n').
                    for raw in self.rfile:
                        if cmd._stop.is_set():
                            break
                        line = raw.decode("utf-8", "replace").strip()
                        if not line:
                            continue
                        # First frame: cancel the silent-timer, and announce
                        # which module just identified itself (type + name).
                        if first:
                            first = False
                            silent_warn.cancel()
                            try:
                                preview = _json.loads(line)
                                what = (f"type={preview.get('type','?')!r} "
                                        f"name={preview.get('name','?')!r}")
                            except Exception:
                                what = f"raw={line[:80]!r}"
                            cmd._log(cmd.style.SUCCESS(
                                f"  [{peer[0]}:{peer[1]}] identified — {what}"))
                        cmd._process_line(line, peer)
                except (ConnectionError, OSError):
                    pass
                finally:
                    silent_warn.cancel()
                    # Close this thread's DB connection so we don't
                    # accumulate open connections (a source of lock contention).
                    connections.close_all()
                    closed = datetime.datetime.now()
                    duration = (closed - opened).total_seconds()
                    cmd._log(cmd.style.WARNING(
                        f"- [{closed:%H:%M:%S}] {peer[0]}:{peer[1]} "
                        f"disconnected (was open {duration:.1f}s)"))

        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = True

        self._log(self.style.SUCCESS(
            f"\nSDR Map server listening on {host}:{port}"))
        self._log("Point each SDR++ module's 'TCP output' to "
                  f"this address:{port}.")
        accepted = ", ".join(t for t, _ in SdrObject.TYPE_CHOICES)
        self._log(f"Accepted types: {accepted}. "
                  "Press Ctrl+C to stop.\n")

        server = Server((host, port), Handler)
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            self._log(self.style.WARNING("\nShutdown requested..."))
        finally:
            self._stop.set()
            server.shutdown()
            server.server_close()
            # Drain any trames left in the coalescing queue before exit, so
            # the last frames received don't get lost in the 150 ms window
            # between arrival and the next batch tick.
            self._flush_pending_once()
            self._log(self.style.WARNING("Server stopped."))

    def _flush_pending_once(self):
        """Final drain of the coalescing queue, called once at shutdown.

        Mirrors the body of _batch_writer_loop but runs synchronously and
        doesn't loop. Safe to call after `_stop` has been set: the writer
        thread will exit on its own (via _stop.wait) without competing for
        the pending dict."""
        with self._pending_lock:
            if not self._pending:
                return
            batch = self._pending
            self._pending = {}
        try:
            with self._db_lock, transaction.atomic():
                tracks = []
                for (obj_type, ident), item in batch.items():
                    defaults = item["defaults"]
                    create_defaults = item["create_defaults"]
                    if create_defaults is not None:
                        SdrObject.objects.update_or_create(
                            obj_type=obj_type, ident=ident,
                            defaults=defaults, create_defaults=create_defaults,
                        )
                    else:
                        SdrObject.objects.update_or_create(
                            obj_type=obj_type, ident=ident, defaults=defaults,
                        )
                    track_alt_ft = item["altitude_ft"]
                    if track_alt_ft is None and item["altitude_m"] is not None:
                        track_alt_ft = int(round(item["altitude_m"] / 0.3048))
                    tracks.append(SdrObjectTrack(
                        obj_type=obj_type, ident=ident,
                        lat=item["lat"], lon=item["lon"],
                        speed=item["speed"], altitude_ft=track_alt_ft,
                        timestamp=defaults["last_seen"],
                    ))
                if tracks:
                    SdrObjectTrack.objects.bulk_create(tracks)
            self._log(self.style.WARNING(
                f"Flushed {len(batch)} pending trame(s) at shutdown."))
        except Exception as exc:
            self._log(self.style.ERROR(f"  Final flush failed: {exc}"))

    # --------------------------------------------------------------------- #
    def _process_line(self, line, peer):
        try:
            data = json.loads(line)
        except json.JSONDecodeError:
            self._log(self.style.ERROR(f"  Invalid JSON ignored: {line[:80]}"))
            return

        obj_type = (data.get("type") or "").strip()
        if obj_type not in dict(SdrObject.TYPE_CHOICES):
            self._log(self.style.ERROR(f"  Unknown type ignored: {obj_type!r}"))
            return

        lat = _to_float(data.get("lat"))
        lon = _to_float(data.get("lon"))
        if lat is None or lon is None:
            # Only positioned objects are mapped.
            return
        # AIS: "position not available" is often emitted as (0, 0) (or at
        # 91°N/181°E depending on the decoder). These points must be ignored:
        # otherwise all ships without a valid fix collide on the same
        # fallback ident (rounded lat,lon), overwrite each other and only one stays.
        if obj_type == SdrObject.TYPE_AIS:
            if abs(lat) < 1e-4 and abs(lon) < 1e-4:
                return
            if lat >= 90.0 or lat <= -90.0 or lon >= 180.0 or lon <= -180.0:
                return

        name = str(data.get("name") or "").strip()
        info = str(data.get("info") or "")

        # --- Identifiant stable selon le type ---
        mmsi = _to_int(data.get("mmsi"))
        icao = str(data.get("icao") or "").strip().lower()
        if obj_type == SdrObject.TYPE_AIS:
            # MMSI is the absolute identity of an AIS ship. We look in the
            # JSON field, then in info ("MMSI=NNNNNNNNN"), then in name
            # ("MMSI:NNNNNNNNN" generated by the module when the ship's
            # name is not yet decoded). Without MMSI we create NOTHING rather
            # than a (lat,lon) fallback that would collide several ships on
            # the same ident.
            if mmsi is None:
                m = re.search(r"MMSI[=: ]\s*(\d{6,9})", info)
                if m:
                    mmsi = _to_int(m.group(1))
            if mmsi is None:
                m = re.search(r"MMSI[:= ]\s*(\d{6,9})", name)
                if m:
                    mmsi = _to_int(m.group(1))
            if mmsi is None:
                # No usable MMSI: skip (and log it).
                self._log(self.style.ERROR(
                    f"  AIS rejected (no MMSI): name={name!r} info={info[:60]!r}"))
                return
            ident = f"MMSI:{mmsi}"
        elif obj_type == SdrObject.TYPE_ADSB and icao:
            ident = f"ICAO:{icao}"
        elif obj_type == SdrObject.TYPE_LRRP:
            # LRRP/DSD-FME: sender's DMR RID if available, otherwise the name.
            src = str(data.get("source") or "").strip()
            ident = f"RID:{src}" if src else (name or f"lrrp:{lat:.4f},{lon:.4f}")
        elif obj_type == SdrObject.TYPE_RADIOSONDE:
            # Radiosonde: the serial number (name) is the stable identity.
            ident = name or f"sonde:{lat:.4f},{lon:.4f}"
        elif obj_type == SdrObject.TYPE_TETRA:
            # TETRA terminals carry an SSI (Short Subscriber Identity).
            # The TETRA decoder reuses the AIS-like schema, so the value may
            # come in as JSON "ssi" or "mmsi", or as "SSI:NNNN" in name.
            ssi = _to_int(data.get("ssi"))
            if ssi is None:
                ssi = _to_int(data.get("mmsi"))   # legacy reuse of AIS schema
            if ssi is None:
                m = re.search(r"SSI[\s:_-]*(\d+)", name)
                if m:
                    ssi = _to_int(m.group(1))
            if ssi is None:
                m = re.search(r"SSI[\s=:_-]*(\d+)", info)
                if m:
                    ssi = _to_int(m.group(1))
            if ssi is None:
                self._log(self.style.ERROR(
                    f"  TETRA rejected (no SSI): name={name!r} info={info[:60]!r}"))
                return
            ident = f"SSI:{ssi}"
        elif obj_type == SdrObject.TYPE_SARSAT:
            # Cospas-Sarsat: the 15-character beacon Hex ID (in `name`) is the
            # unique global identifier of the beacon. Normalise to upper case
            # so case mismatches between transmissions don't create duplicates.
            hex_id = (name or "").strip().upper()
            ident = f"SARSAT:{hex_id}" if hex_id else f"sarsat:{lat:.4f},{lon:.4f}"
        elif obj_type == SdrObject.TYPE_SATELLITE:
            # Satellite tracker: NORAD catalogue number is the canonical
            # identifier (globally unique). Falls back to the TLE name if
            # NORAD wasn't transmitted for some reason.
            kv = _parse_kv_info(info)
            norad_str = kv.get("norad", "").strip()
            if norad_str.isdigit():
                ident = f"SAT:{norad_str}"
            elif name:
                ident = f"SAT:{name}"
            else:
                ident = f"sat:{lat:.4f},{lon:.4f}"
        else:  # APRS / APRS Meteo: callsign/name is authoritative
            ident = name or f"{obj_type}:{lat:.4f},{lon:.4f}"

        # --- Kinematics ---
        speed = _to_float(data.get("speed"))
        heading = _to_float(data.get("heading"))
        altitude_ft = _to_int(data.get("altitude_ft"))
        # Extracted from "info" for ADS-B when not provided as a clean field.
        if heading is None:
            m = _RE_HDG.search(info)
            if m:
                heading = _to_float(m.group(1))
        # For AIS ships, the heading comes from COG (course over ground), or
        # falling back to HDG (true heading). The module writes it in info.
        if heading is None and obj_type == SdrObject.TYPE_AIS:
            m = _RE_COG.search(info)
            if m:
                heading = _to_float(m.group(1))
        # TETRA LIP: course over ground in info as "dir=92deg".
        if heading is None and obj_type == SdrObject.TYPE_TETRA:
            m = _RE_DIR_DEG.search(info)
            if m:
                heading = _to_float(m.group(1))
        if altitude_ft is None:
            m = _RE_ALT.search(info)
            if m:
                altitude_ft = _to_int(m.group(1))

        # --- APRS weather ---
        weather = {}
        for field, variants in _WEATHER_KEYS.items():
            for key in variants:
                if key in data:
                    weather[field] = _to_float(data[key])
                    break

        # --- Radiosonde-specific: altitude (m), climb/descent, PTU ---
        altitude_m = climb_rate = None
        if obj_type == SdrObject.TYPE_RADIOSONDE:
            kv = _parse_info_kv(info)
            altitude_m = kv.get("alt")
            climb_rate = kv.get("climb")
            if heading is None and "hdg" in kv:
                heading = kv["hdg"]
            # Onboard PTU -> reuse existing weather columns.
            if "temp" in kv:
                weather.setdefault("temp_c", kv["temp"])
            if "rh" in kv:
                weather.setdefault("humidity", kv["rh"])
            if "p" in kv:
                weather.setdefault("pressure_hpa", kv["p"])

        # --- Additional unmapped data ---
        extra = {k: v for k, v in data.items() if k not in _KNOWN_KEYS}

        # AIS subcategory (ship / AtoN / SAR / coast station / etc.) and MID.
        # Computed once on the server so the browser can pick the right icon
        # without re-implementing the classification.
        if obj_type == SdrObject.TYPE_AIS and mmsi is not None:
            kind, label, mid, country_iso, country_name = classify_mmsi(mmsi)
            extra["mmsi_kind"] = kind
            extra["mmsi_label"] = label
            if mid is not None:
                extra["mid"] = mid
            if country_iso is not None:
                extra["country_iso"] = country_iso
            if country_name is not None:
                extra["country_name"] = country_name

        # TETRA-specific extras: GPS accuracy (metres) parsed from info as
        # "acc=20m". We surface it in the popup so operators can judge fix
        # quality at a glance. Stored in `extra` (no dedicated column needed).
        ssi_val = None
        if obj_type == SdrObject.TYPE_TETRA:
            # Re-derive SSI from the ident we built above ("SSI:NNNN").
            try:
                ssi_val = int(ident.split(":", 1)[1])
            except (IndexError, ValueError):
                ssi_val = None
            m = _RE_ACC_M.search(info)
            if m:
                try:
                    extra["gps_acc_m"] = float(m.group(1))
                except ValueError:
                    pass

        # Cospas-Sarsat (406 MHz distress beacons): the info field is
        # structured as "beacon=EPIRB;country=France;protocol=...". We parse
        # it once and promote the relevant fields into extra under sarsat_*
        # keys so to_dict() surfaces them cleanly. We also try to resolve the
        # country name to an ISO code so the popup can show the same flag SVG
        # used by AIS.
        if obj_type == SdrObject.TYPE_SARSAT:
            kv = _parse_kv_info(info)
            # Top-level country fields (shared with AIS): country in clear
            # plus ISO code when we can resolve it.
            country_clear = kv.get("country", "").strip()
            if country_clear:
                extra["country_name"] = country_clear
                iso = _COUNTRY_NAME_TO_ISO.get(country_clear.upper())
                if iso:
                    extra["country_iso"] = iso
            # SARSAT-specific fields promoted to extra under sarsat_* prefix.
            # The "test" flag is a real boolean; other fields stay as strings.
            for k_src, k_dst in (
                ("beacon",    "sarsat_beacon"),
                ("protocol",  "sarsat_protocol"),
                ("aircraft",  "sarsat_aircraft"),
                ("callsign",  "sarsat_callsign"),
                ("serial",    "sarsat_serial"),
                ("operator",  "sarsat_operator"),
                ("src",       "sarsat_src"),
                ("homing121", "sarsat_homing121"),
                ("bch1",      "sarsat_bch1"),
                ("bch2",      "sarsat_bch2"),
            ):
                if k_src in kv:
                    extra[k_dst] = kv[k_src]
            extra["sarsat_test"] = (kv.get("test", "").lower() == "yes")

        # ADS-B wake-vortex category (Aircraft Identification message,
        # Type Code 1-4). Accepted from three places, in priority order:
        #   1. top-level JSON field   "category": "A3"   (or "A7", "B6", …)
        #   2. top-level JSON field   "adsb_category": "A3"   (alias)
        #   3. embedded in info       ... cat=A3 ...   or  ... category=A3 ...
        # If none is found, extra stays empty for this field and the client
        # falls back to the generic airliner icon — no regression.
        if obj_type == SdrObject.TYPE_ADSB:
            cat = data.get("category") or data.get("adsb_category")
            if not cat:
                m = _RE_CATEGORY.search(info)
                if m:
                    cat = m.group(1)
            if cat:
                cat_norm = str(cat).strip().upper()
                # Accept only the canonical ICAO format (letter + digit).
                if len(cat_norm) == 2 and cat_norm[0] in "ABC" and cat_norm[1].isdigit():
                    extra["adsb_category"] = cat_norm

            # Phase B enrichment: look the ICAO up in the local Mictronics
            # database (built by tools/build_aircraft_db.py). If the file
            # isn't present (Phase A-only install), the lookup just returns
            # None and we proceed unchanged. When found, we surface the
            # registration / type code / military flag and — only if the
            # frame itself did NOT carry a wake-vortex category — we use
            # the type-code-derived category so the client can still pick
            # the right icon (helicopter / glider / fighter).
            ac = _AIRCRAFT_DB.lookup(icao) if icao else None
            if ac:
                if ac["registration"]:
                    extra["aircraft_reg"] = ac["registration"]
                if ac["type_code"]:
                    extra["aircraft_type"] = ac["type_code"]
                if ac["is_military"]:
                    extra["aircraft_military"] = True
                # Fallback category from the type code, only when the ADS-B
                # frame didn't carry one of its own.
                if "adsb_category" not in extra and ac["category"]:
                    extra["adsb_category"] = ac["category"]
                # An identified military aircraft whose category we still
                # can't infer (e.g. a transport plane like a C-17) gets a
                # fighter icon as a visual cue. Type-code-derived heli /
                # glider categories already trumped this earlier.
                elif ac["is_military"] and "adsb_category" not in extra:
                    extra["adsb_category"] = "A6"
                # Type description ("BOEING 737-800"), ICAO class ("L2J" =
                # Land/2-engine/Jet) and weight ("L"/"M"/"H"/"J"). All
                # cheap second-table lookup if we have a type_code.
                if ac["type_code"]:
                    td = _AIRCRAFT_DB.lookup_type(ac["type_code"])
                    if td:
                        if td["description"]:
                            extra["aircraft_type_desc"] = td["description"]
                        if td["icao_class"]:
                            extra["aircraft_icao_class"] = td["icao_class"]

            # Country of registration: derived from the ICAO 24-bit address
            # range alone (ICAO Annex 10 Vol III allocation table). Works
            # even when the SQLite DB isn't installed.
            if icao:
                cc_iso, cc_name = country_from_icao(icao)
                if cc_iso:
                    extra["aircraft_country_iso"] = cc_iso
                if cc_name:
                    extra["aircraft_country_name"] = cc_name

            # Airline operator from the callsign prefix (3 letters). The
            # `name` field on an ADS-B trame is the callsign emitted by the
            # aircraft itself. e.g. "AFR1234" -> Air France, "RYR4123" ->
            # Ryanair. Doesn't fire for personal/military callsigns
            # without a registered airline code.
            if name:
                op = _AIRCRAFT_DB.lookup_operator(name)
                if op:
                    if op["name"]:
                        extra["aircraft_operator"] = op["name"]
                    if op["country"]:
                        extra["aircraft_operator_country"] = op["country"]

        # Satellite tracker (orbital): info packs the look angles, range,
        # altitude, doppler and footprint diameter as
        # "norad=33591;alt=845;az=137.4;el=22.8;range=1043;doppler=1256;footprint=4541"
        # All numeric. Doppler is a signed integer (Hz), elevation can be
        # negative (satellite below the horizon). We keep them in metric units.
        if obj_type == SdrObject.TYPE_SATELLITE:
            kv = _parse_kv_info(info)
            def _as_int(s):
                try: return int(float(s))
                except (TypeError, ValueError): return None
            def _as_float(s):
                try: return float(s)
                except (TypeError, ValueError): return None
            for k_src, k_dst, conv in (
                ("norad",     "sat_norad",        _as_int),
                ("alt",       "sat_alt_km",       _as_int),
                ("az",        "sat_az",           _as_float),
                ("el",        "sat_el",           _as_float),
                ("range",     "sat_range_km",     _as_int),
                ("doppler",   "sat_doppler_hz",   _as_int),
                ("footprint", "sat_footprint_km", _as_int),
            ):
                v = conv(kv.get(k_src))
                if v is not None:
                    extra[k_dst] = v

        defaults = {
            "name": name,
            "lat": lat,
            "lon": lon,
            "speed": speed,
            "heading": heading,
            "altitude_ft": altitude_ft,
            "altitude_m": altitude_m,
            "climb_rate": climb_rate,
            "mmsi": mmsi,
            "icao": icao,
            "ssi":  ssi_val,
            "info": info,
            "obs_date": str(data.get("date") or ""),
            "obs_time": str(data.get("time") or ""),
            "extra": extra,
            "last_seen": timezone.now(),
            **weather,
        }

        # AIS: a position message (type 1/2/3/18) carries NO ship name, so the
        # decoder fills "name" with the MMSI placeholder. If we let that go
        # through, every new position would overwrite the real ship name that
        # came in from a static message (type 5/24). So:
        #   - update: drop "name" from defaults when it's a placeholder, to
        #     keep whatever name was already stored.
        #   - create: still set a fallback name (via create_defaults) so a
        #     brand new ship has at least its MMSI shown as a label.
        # Note: Django's update_or_create uses create_defaults INSTEAD OF
        # defaults at creation time, so create_defaults must hold every field
        # we want on insert (we copy defaults and only override "name").
        create_defaults = None
        if obj_type == SdrObject.TYPE_AIS:
            placeholder = (
                not name
                or _looks_like_mmsi_placeholder(name, mmsi)
            )
            if placeholder:
                # On update: don't touch the stored name.
                defaults.pop("name", None)
                # On create: show "MMSI 123456789" rather than empty.
                create_defaults = dict(defaults)
                create_defaults["name"] = f"MMSI {mmsi}" if mmsi else ""

        # Push the parsed trame into the coalescing queue. The actual DB
        # write happens in the batch writer thread (every _BATCH_INTERVAL).
        # If a previous trame for the same (obj_type, ident) is still
        # pending, it is overwritten — the most recent position wins, older
        # positions for the same object within the same cycle are dropped.
        with self._pending_lock:
            self._pending[(obj_type, ident)] = {
                "defaults": defaults,
                "create_defaults": create_defaults,
                "lat": lat, "lon": lon,
                "speed": speed,
                "altitude_ft": altitude_ft,
                "altitude_m": altitude_m,
            }
        self._log(f"  [{obj_type:10s}] queued {name or ident} "
                  f"({lat:.4f}, {lon:.4f})")

    # ------------------------------------------------------------------------
    def _batch_writer_loop(self):
        """Drain the coalescing queue every _BATCH_INTERVAL and persist all
        pending positions in a single SQLite transaction.

        Why a single transaction: SQLite serialises commits with an fsync
        each, so N separate writes cost N × fsync (~10 ms each on a typical
        disk). One transaction with N updates costs one fsync regardless of
        N. With 50 trames/sec spread across multiple modules, this is the
        difference between saturating the lock and having spare capacity.
        """
        while not self._stop.wait(self._BATCH_INTERVAL):
            # Swap the pending dict for an empty one under the queue lock,
            # so the TCP threads can keep accepting new trames while we work.
            with self._pending_lock:
                if not self._pending:
                    continue
                batch = self._pending
                self._pending = {}

            # Each item is a dict of fields ready to feed to update_or_create.
            broadcast_objs = []
            new_tracks = []
            try:
                with self._db_lock, transaction.atomic():
                    for (obj_type, ident), item in batch.items():
                        defaults = item["defaults"]
                        create_defaults = item["create_defaults"]
                        if create_defaults is not None:
                            obj, created = SdrObject.objects.update_or_create(
                                obj_type=obj_type, ident=ident,
                                defaults=defaults,
                                create_defaults=create_defaults,
                            )
                        else:
                            obj, created = SdrObject.objects.update_or_create(
                                obj_type=obj_type, ident=ident,
                                defaults=defaults,
                            )

                        # Track point: only insert if the position actually
                        # changed since the last recorded point. We keep an
                        # in-memory cache of the last position per object so
                        # we don't hit SQLite with a SELECT per ident.
                        lat = item["lat"]; lon = item["lon"]
                        cached = self._last_track_pos.get((obj_type, ident))
                        if cached is None:
                            # Cache miss: ask SQLite once, then trust the cache.
                            cached = (SdrObjectTrack.objects
                                      .filter(obj_type=obj_type, ident=ident)
                                      .order_by("-timestamp")
                                      .values_list("lat", "lon")
                                      .first())
                        if cached is None or cached[0] != lat or cached[1] != lon:
                            track_alt_ft = item["altitude_ft"]
                            if track_alt_ft is None and item["altitude_m"] is not None:
                                track_alt_ft = int(round(item["altitude_m"] / 0.3048))
                            new_tracks.append(SdrObjectTrack(
                                obj_type=obj_type, ident=ident,
                                lat=lat, lon=lon,
                                speed=item["speed"], altitude_ft=track_alt_ft,
                                timestamp=defaults["last_seen"],
                            ))
                            self._last_track_pos[(obj_type, ident)] = (lat, lon)

                        broadcast_objs.append(obj.to_dict())

                    # Tracks in one bulk_create — much cheaper than N
                    # individual create() calls inside the same transaction.
                    if new_tracks:
                        SdrObjectTrack.objects.bulk_create(new_tracks)
            except Exception as exc:
                self._log(self.style.ERROR(
                    f"  Batch write failed ({len(batch)} items): {exc}"))
                continue

            # WebSocket broadcast outside the DB lock. We still send one
            # message per object (the client expects "object" events one by
            # one); a future optimisation would be to bundle them, but each
            # message is already small and the InMemoryChannelLayer is fast.
            if self.broadcast and _CHANNEL_LAYER is not None and broadcast_objs:
                for d in broadcast_objs:
                    try:
                        async_to_sync(_CHANNEL_LAYER.group_send)(
                            GROUP_NAME,
                            {"type": "sdr.object", "object": d},
                        )
                    except Exception as exc:
                        self._log(self.style.ERROR(
                            f"  WS broadcast failed: {exc}"))

    # --------------------------------------------------------------------- #
    def _purge_loop(self):
        """Periodically purges objects AND their tracks per the retention
        configured for each type (RetentionSetting, editable from the UI)."""
        from datetime import timedelta
        from map.models import RetentionSetting

        while not self._stop.wait(30):  # check every 30 s
            try:
                ttl_by_type = RetentionSetting.all_minutes()
            except Exception as exc:
                self._log(self.style.ERROR(f"  Retention read error: {exc}"))
                continue

            now = timezone.now()
            total_stale = 0
            total_track_deleted = 0
            for obj_type, minutes in ttl_by_type.items():
                cutoff = now - timedelta(minutes=minutes)
                # Stale objects
                stale = list(SdrObject.objects.filter(
                    obj_type=obj_type, last_seen__lt=cutoff))
                for obj in stale:
                    pk = obj.pk
                    # Drop the cached last-position too, otherwise the next
                    # trame for this ident would see a stale cache entry and
                    # miss the chance to insert the first track point of a
                    # fresh observation.
                    self._last_track_pos.pop((obj.obj_type, obj.ident), None)
                    obj.delete()
                    if self.broadcast and _CHANNEL_LAYER is not None:
                        try:
                            async_to_sync(_CHANNEL_LAYER.group_send)(
                                GROUP_NAME, {"type": "sdr.remove", "id": pk},
                            )
                        except Exception:
                            pass
                total_stale += len(stale)
                # Track points older than the same retention:
                # les traces disparaissent avec leurs objets.
                deleted, _ = SdrObjectTrack.objects.filter(
                    obj_type=obj_type, timestamp__lt=cutoff).delete()
                total_track_deleted += deleted

            if (total_stale or total_track_deleted) and not self.quiet:
                self._log(self.style.WARNING(
                    f"  Retention purge: {total_stale} object(s), "
                    f"{total_track_deleted} track point(s) removed"))

    # --------------------------------------------------------------------- #
    def _log(self, msg):
        if not self.quiet:
            self.stdout.write(msg)
