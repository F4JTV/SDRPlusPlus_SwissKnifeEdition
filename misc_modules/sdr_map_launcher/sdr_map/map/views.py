"""Views: map page, objects API (with recent track), track API (JSON/GPX),
retention settings API, service worker."""
from datetime import timedelta
import json
import os
import sqlite3
import subprocess
import sys
import threading
from pathlib import Path
from xml.sax.saxutils import escape as xml_escape

from django.conf import settings
from django.http import JsonResponse, HttpResponse, HttpResponseBadRequest
from django.shortcuts import render
from django.utils import timezone
from django.views.decorators.cache import never_cache
from django.views.decorators.csrf import csrf_exempt
from django.views.decorators.http import require_http_methods
from django.contrib.staticfiles import finders

from .models import SdrObject, SdrObjectTrack, RetentionSetting
from .services import aircraft_lookup


def index(request):
    """Main map page."""
    return render(request, "map/index.html", {
        "ws_scheme": "wss" if request.is_secure() else "ws",
        "center_lat": settings.SDR_MAP_CENTER_LAT,
        "center_lon": settings.SDR_MAP_CENTER_LON,
        "default_zoom": settings.SDR_MAP_DEFAULT_ZOOM,
        "retention_defaults": RetentionSetting.DEFAULTS,
        "type_labels": dict(SdrObject.TYPE_CHOICES),
    })


def _recent_track(obj_type, ident, limit):
    """Most recent positions [lat, lon] of an object, chronological order."""
    pts = list(
        SdrObjectTrack.objects
        .filter(obj_type=obj_type, ident=ident)
        .order_by("-timestamp")
        .values_list("lat", "lon")[:limit]
    )
    pts.reverse()
    return [[la, lo] for la, lo in pts]


@never_cache
def api_objects(request):
    """
    Non-stale objects with their recent positions (so traces can be redrawn
    on load). Stale = older than the per-type retention. Optional filter:
    ?types=AIS,APRS.
    """
    now = timezone.now()
    ttl_by_type = RetentionSetting.all_minutes()

    types = request.GET.get("types")
    wanted = set(t.strip() for t in types.split(",")) if types else None

    qs = SdrObject.objects.all()
    if wanted:
        qs = qs.filter(obj_type__in=wanted)

    n = settings.SDR_TRACK_DISPLAY_POINTS
    objects = []
    for o in qs:
        ttl = ttl_by_type.get(o.obj_type, 60)
        if (now - o.last_seen) > timedelta(minutes=ttl):
            continue  # stale per its type's retention
        objects.append(o.to_dict(track=_recent_track(o.obj_type, o.ident, n)))

    return JsonResponse(
        {"objects": objects}, json_dumps_params={"ensure_ascii": False}
    )


@never_cache
def api_track(request):
    """
    Full track of an object, for replay or GPX export.

    Params: ?type=ADSB&ident=ICAO:3c6dd2[&format=gpx][&limit=N]
    """
    obj_type = request.GET.get("type", "").strip()
    ident = request.GET.get("ident", "").strip()
    if not obj_type or not ident:
        return HttpResponseBadRequest("Missing 'type' or 'ident' parameter.")

    qs = (SdrObjectTrack.objects
          .filter(obj_type=obj_type, ident=ident)
          .order_by("timestamp"))
    try:
        limit = int(request.GET.get("limit", "0"))
    except ValueError:
        limit = 0
    points = list(qs)
    if limit > 0:
        points = points[-limit:]

    name = ident
    obj = SdrObject.objects.filter(obj_type=obj_type, ident=ident).first()
    if obj and obj.name:
        name = obj.name

    if request.GET.get("format") == "gpx":
        return _gpx_response(obj_type, name, points)

    return JsonResponse({
        "type": obj_type, "ident": ident, "name": name,
        "points": [{
            "lat": p.lat, "lon": p.lon,
            "t": p.timestamp.isoformat(),
            "speed": p.speed, "alt": p.altitude_ft,
        } for p in points],
    }, json_dumps_params={"ensure_ascii": False})


def _gpx_response(obj_type, name, points):
    """Build a downloadable GPX 1.1 file (one track)."""
    safe = xml_escape(name or "trace")
    rows = []
    for p in points:
        ele = ""
        if p.altitude_ft is not None:
            ele = f"<ele>{p.altitude_ft * 0.3048:.1f}</ele>"
        rows.append(
            f'<trkpt lat="{p.lat:.6f}" lon="{p.lon:.6f}">'
            f'{ele}<time>{p.timestamp.strftime("%Y-%m-%dT%H:%M:%SZ")}</time></trkpt>'
        )
    body = "\n".join(rows)
    gpx = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<gpx version="1.1" creator="SDR Map" '
        'xmlns="http://www.topografix.com/GPX/1/1">\n'
        f'<trk><name>{safe} ({xml_escape(obj_type)})</name><trkseg>\n'
        f'{body}\n'
        '</trkseg></trk>\n</gpx>\n'
    )
    filename = "".join(c if c.isalnum() else "_" for c in (name or "trace"))
    resp = HttpResponse(gpx, content_type="application/gpx+xml")
    resp["Content-Disposition"] = f'attachment; filename="{filename}.gpx"'
    return resp


# ---------------------------------------------------------- retention API ---
@never_cache
@csrf_exempt
@require_http_methods(["GET", "POST"])
def api_retention(request):
    """
    GET  -> {"AIS": 30, "ADSB": 5, ...} (minutes per type)
    POST {"ADSB": 10, "AIS": 20, ...}  -> upserts and returns the new map.

    Posting also triggers an immediate purge of objects AND tracks that no
    longer fit the new retention.
    """
    if request.method == "GET":
        return JsonResponse(RetentionSetting.all_minutes())

    try:
        payload = json.loads(request.body or "{}")
    except json.JSONDecodeError:
        return HttpResponseBadRequest("Invalid JSON body")

    valid_types = dict(SdrObject.TYPE_CHOICES)
    updated = {}
    for obj_type, minutes in payload.items():
        if obj_type not in valid_types:
            continue
        try:
            m = max(1, int(minutes))  # at least 1 minute
        except (TypeError, ValueError):
            continue
        RetentionSetting.objects.update_or_create(
            obj_type=obj_type, defaults={"minutes": m},
        )
        updated[obj_type] = m

    # Apply right away: purge stale objects & tracks per the new settings.
    _apply_retention_purge()

    return JsonResponse(RetentionSetting.all_minutes())


def _apply_retention_purge():
    """Delete objects and track points older than their type's retention."""
    now = timezone.now()
    for obj_type, minutes in RetentionSetting.all_minutes().items():
        cutoff = now - timedelta(minutes=minutes)
        SdrObject.objects.filter(obj_type=obj_type, last_seen__lt=cutoff).delete()
        SdrObjectTrack.objects.filter(obj_type=obj_type, timestamp__lt=cutoff).delete()


# ---------------------------------------------------------- service worker ---
@never_cache
def service_worker(request):
    """Served at '/sw.js' so its scope is '/'."""
    path = finders.find("map/js/sw.js")
    with open(path, "r", encoding="utf-8") as fh:
        body = fh.read()
    resp = HttpResponse(body, content_type="application/javascript")
    resp["Service-Worker-Allowed"] = "/"
    return resp


# -------------------------------------------------------- aircraft DB API ----
#
# Two endpoints for the "Aircraft DB" panel section that lets the user trigger
# a Mictronics database refresh without touching SDR++ or the shell.
#   GET  /api/aircraft_db/status  -> stats about the on-disk SQLite file
#   POST /api/aircraft_db/update  -> runs tools/build_aircraft_db.py
# The build script writes to a `.new` file and os.replace()s it on success,
# so concurrent ADS-B lookups never see a half-written database.

# Serialises Update calls. A second concurrent call returns "already running"
# rather than launching a duplicate subprocess fighting for the same file.
_db_update_lock = threading.Lock()


def api_aircraft_db_status(request):
    """Return on-disk stats of the local aircraft enrichment database."""
    db_path = aircraft_lookup.DB.path
    if not os.path.exists(db_path):
        return JsonResponse({
            "exists": False,
            "path":   db_path,
        })
    try:
        st = os.stat(db_path)
    except OSError as exc:
        return JsonResponse({"exists": False, "error": str(exc)})

    counts = {"aircraft": 0, "military": 0, "operators": 0, "types": 0}
    # Open a dedicated short-lived connection so we don't perturb the
    # singleton used by the live ADS-B pipeline. Errors are swallowed —
    # the file might be mid-replacement during an update.
    try:
        conn = sqlite3.connect(db_path)
        try:
            counts["aircraft"]  = conn.execute("SELECT COUNT(*) FROM aircraft").fetchone()[0]
            counts["military"]  = conn.execute("SELECT COUNT(*) FROM aircraft WHERE is_military=1").fetchone()[0]
            counts["operators"] = conn.execute("SELECT COUNT(*) FROM operators").fetchone()[0]
            counts["types"]     = conn.execute("SELECT COUNT(*) FROM types").fetchone()[0]
        finally:
            conn.close()
    except sqlite3.Error:
        pass

    return JsonResponse({
        "exists":     True,
        "path":       db_path,
        "mtime":      st.st_mtime,
        "size_bytes": st.st_size,
        "counts":     counts,
    })


@csrf_exempt
@require_http_methods(["POST"])
def api_aircraft_db_update(request):
    """Run the Mictronics download/build script and replace the local DB.

    Synchronous: the request blocks for ~3-5 seconds (download + SQLite
    build). The browser stays responsive thanks to the async fetch on the
    UI side.
    """
    if not _db_update_lock.acquire(blocking=False):
        return JsonResponse({
            "ok": False,
            "log": "An update is already in progress.",
        })
    try:
        # Drop our open SQLite handle so the build script can replace
        # the file (essential on Windows; harmless on Linux).
        aircraft_lookup.DB._close()

        # The build script lives in `tools/` which is a sibling of the
        # Django project directory (BASE_DIR points at sdr_map/).
        script = Path(settings.BASE_DIR).parent / "tools" / "build_aircraft_db.py"
        if not script.exists():
            return JsonResponse({
                "ok": False,
                "log": f"Build script not found at {script}",
            })

        try:
            result = subprocess.run(
                [sys.executable, str(script)],
                capture_output=True,
                text=True,
                timeout=180,
            )
        except subprocess.TimeoutExpired:
            return JsonResponse({
                "ok":  False,
                "log": "Build script timed out after 180 seconds.",
            })

        log = (result.stdout or "") + (result.stderr or "")
        return JsonResponse({
            "ok":  result.returncode == 0,
            "log": log.strip() or "(no output)",
        })
    finally:
        _db_update_lock.release()
