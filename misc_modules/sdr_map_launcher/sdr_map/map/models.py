"""
Unified model for all objects received from SDR++ modules.

The four modules (ais_decoder, adsb_decoder, aprs_decoder, dsd_decoder,
radiosonde_decoder) all send one JSON line per positioned object on the **same
base schema** (modelled after the AIS module):

    {"name", "date", "time", "lat", "lon", "type", "speed", "info"}

with, depending on the type, additional fields:
    - AIS          -> "mmsi"
    - ADSB         -> "icao"
    - APRS Meteo   -> weather fields instead of "speed"
    - lrrp         -> "source" (DMR RID)
    - radiosonde   -> info: alt= (m), hdg=, climb=, temp=, rh=, p=

A single object type is stored, identified by the (obj_type, ident) pair so
that an existing object is updated rather than duplicated.
"""
from datetime import timedelta

from django.db import models
from django.utils import timezone


class SdrObject(models.Model):
    TYPE_AIS = "AIS"
    TYPE_ADSB = "ADSB"
    TYPE_APRS = "APRS"
    TYPE_APRS_METEO = "APRS Meteo"
    TYPE_LRRP = "lrrp"
    TYPE_RADIOSONDE = "radiosonde"
    TYPE_TETRA = "TETRA"
    TYPE_SARSAT = "SARSAT"
    TYPE_SATELLITE = "satellite"
    TYPE_CHOICES = [
        (TYPE_AIS, "AIS (ships)"),
        (TYPE_ADSB, "ADS-B (aircraft)"),
        (TYPE_APRS, "APRS (stations)"),
        (TYPE_APRS_METEO, "APRS weather"),
        (TYPE_LRRP, "LRRP / DMR GPS (DSD-FME)"),
        (TYPE_RADIOSONDE, "Radiosonde"),
        (TYPE_TETRA, "TETRA (LIP)"),
        (TYPE_SARSAT, "Cospas-Sarsat (406 MHz beacons)"),
        (TYPE_SATELLITE, "Satellites (orbital trackers)"),
    ]

    # --- Identity --------------------------------------------------------- #
    obj_type = models.CharField(
        "Type", max_length=16, choices=TYPE_CHOICES, db_index=True
    )
    ident = models.CharField("Identifier", max_length=64, db_index=True)
    name = models.CharField("Name", max_length=128, blank=True)

    # --- Position --------------------------------------------------------- #
    lat = models.FloatField("Latitude")
    lon = models.FloatField("Longitude")

    # --- Kinematics ------------------------------------------------------- #
    speed = models.FloatField("Speed", null=True, blank=True)
    heading = models.FloatField("Heading", null=True, blank=True)
    altitude_ft = models.IntegerField("Altitude (ft)", null=True, blank=True)

    # --- Type-specific ---------------------------------------------------- #
    mmsi = models.BigIntegerField("MMSI", null=True, blank=True)
    icao = models.CharField("ICAO", max_length=8, blank=True)
    ssi  = models.BigIntegerField("SSI", null=True, blank=True)   # TETRA

    # --- Radiosonde ------------------------------------------------------- #
    altitude_m = models.FloatField("Altitude (m)", null=True, blank=True)
    climb_rate = models.FloatField("Vertical speed (m/s)", null=True, blank=True)

    # --- APRS weather (metric, null when not relevant) -------------------- #
    temp_c = models.FloatField("Temperature (°C)", null=True, blank=True)
    humidity = models.FloatField("Humidity (%)", null=True, blank=True)
    wind_dir = models.FloatField("Wind direction (°)", null=True, blank=True)
    wind_speed = models.FloatField("Wind (km/h)", null=True, blank=True)
    wind_gust = models.FloatField("Gust (km/h)", null=True, blank=True)
    pressure_hpa = models.FloatField("Pressure (hPa)", null=True, blank=True)
    rain_mm = models.FloatField("Rain (mm)", null=True, blank=True)

    # --- Misc ------------------------------------------------------------- #
    info = models.TextField("Info", blank=True)
    obs_date = models.CharField(max_length=16, blank=True)
    obs_time = models.CharField(max_length=16, blank=True)
    extra = models.JSONField("Raw data", default=dict, blank=True)

    first_seen = models.DateTimeField("First seen", auto_now_add=True)
    last_seen = models.DateTimeField("Last seen", auto_now=True, db_index=True)

    class Meta:
        verbose_name = "SDR object"
        verbose_name_plural = "SDR objects"
        unique_together = ("obj_type", "ident")
        ordering = ("-last_seen",)

    def __str__(self):
        return f"[{self.obj_type}] {self.name or self.ident}"

    def to_dict(self, track=None):
        """JSON representation sent to the browser (API + WebSocket)."""
        return {
            "id": self.pk,
            "type": self.obj_type,
            "ident": self.ident,
            "name": self.name,
            "lat": self.lat,
            "lon": self.lon,
            "speed": self.speed,
            "heading": self.heading,
            "altitude_ft": self.altitude_ft,
            "altitude_m": self.altitude_m,
            "climb_rate": self.climb_rate,
            "mmsi": self.mmsi,
            "icao": self.icao,
            # ADS-B wake-vortex category (A1-A7, B1-B7, C1-C3), extracted by
            # listen_sdr from the Aircraft Identification message. Drives the
            # icon variant on the client (airliner / light / fighter / heli /
            # drone / glider). None when the aircraft didn't emit it.
            "adsb_category": (self.extra or {}).get("adsb_category"),
            # Phase B enrichment from the local Mictronics aircraft database,
            # looked up by ICAO. All optional; null when the database file is
            # not installed (Phase A only) or when the ICAO isn't catalogued.
            "aircraft_reg":      (self.extra or {}).get("aircraft_reg"),
            "aircraft_type":     (self.extra or {}).get("aircraft_type"),
            "aircraft_military": (self.extra or {}).get("aircraft_military"),
            "ssi":  self.ssi,
            "gps_acc_m": (self.extra or {}).get("gps_acc_m"),
            "source": (self.extra or {}).get("source"),
            # AIS subcategory (ship / aton / sart / sar_aircraft / ...) + MID.
            # Computed by listen_sdr and stored in extra; surfaced here so the
            # client can pick the right icon and display the country.
            "mmsi_kind": (self.extra or {}).get("mmsi_kind"),
            "mmsi_label": (self.extra or {}).get("mmsi_label"),
            "mid": (self.extra or {}).get("mid"),
            "country_iso": (self.extra or {}).get("country_iso"),
            "country_name": (self.extra or {}).get("country_name"),
            # Cospas-Sarsat 406 MHz beacon fields (extracted from `info` and
            # stored in extra by listen_sdr). Surfaced at top level so the
            # client picks them up without digging into extra.
            "sarsat_beacon":    (self.extra or {}).get("sarsat_beacon"),
            "sarsat_protocol":  (self.extra or {}).get("sarsat_protocol"),
            "sarsat_aircraft":  (self.extra or {}).get("sarsat_aircraft"),
            "sarsat_callsign":  (self.extra or {}).get("sarsat_callsign"),
            "sarsat_serial":    (self.extra or {}).get("sarsat_serial"),
            "sarsat_operator":  (self.extra or {}).get("sarsat_operator"),
            "sarsat_src":       (self.extra or {}).get("sarsat_src"),
            "sarsat_homing121": (self.extra or {}).get("sarsat_homing121"),
            "sarsat_bch1":      (self.extra or {}).get("sarsat_bch1"),
            "sarsat_bch2":      (self.extra or {}).get("sarsat_bch2"),
            "sarsat_test":      (self.extra or {}).get("sarsat_test"),
            # Satellite tracker fields (sub-satellite point + look angles +
            # Doppler), parsed from `info` and stored in extra by listen_sdr.
            "sat_norad":        (self.extra or {}).get("sat_norad"),
            "sat_alt_km":       (self.extra or {}).get("sat_alt_km"),
            "sat_az":           (self.extra or {}).get("sat_az"),
            "sat_el":           (self.extra or {}).get("sat_el"),
            "sat_range_km":     (self.extra or {}).get("sat_range_km"),
            "sat_doppler_hz":   (self.extra or {}).get("sat_doppler_hz"),
            "sat_footprint_km": (self.extra or {}).get("sat_footprint_km"),
            "weather": {
                "temp_c": self.temp_c,
                "humidity": self.humidity,
                "wind_dir": self.wind_dir,
                "wind_speed": self.wind_speed,
                "wind_gust": self.wind_gust,
                "pressure_hpa": self.pressure_hpa,
                "rain_mm": self.rain_mm,
            },
            "info": self.info,
            "date": self.obs_date,
            "time": self.obs_time,
            "last_seen": self.last_seen.isoformat(),
            "track": track or [],
        }


class SdrObjectTrack(models.Model):
    """
    Timestamped history of an object's positions.

    Decoupled from SdrObject (logical key obj_type+ident instead of FK): the
    TTL purge of a "live" object does not delete its track, which remains
    available (replay, GPX export) until it exceeds its own retention.
    """
    obj_type = models.CharField(max_length=16, db_index=True)
    ident = models.CharField(max_length=64, db_index=True)
    lat = models.FloatField()
    lon = models.FloatField()
    speed = models.FloatField(null=True, blank=True)
    altitude_ft = models.IntegerField(null=True, blank=True)
    timestamp = models.DateTimeField(db_index=True)

    class Meta:
        verbose_name = "Track point"
        verbose_name_plural = "Track points"
        ordering = ("timestamp",)
        indexes = [
            models.Index(fields=["obj_type", "ident", "timestamp"]),
        ]

    def __str__(self):
        return f"{self.obj_type}:{self.ident} @ {self.timestamp:%H:%M:%S}"


class RetentionSetting(models.Model):
    """
    Configurable retention (in MINUTES) per object type. Drives the
    "live"/visible window AND the track purge so objects and their tracks
    disappear together once they exceed the configured age.

    Edited live from the web UI (no server restart required). Sensible
    defaults: 5 min for aircraft, 30 min for ships, 60 min for the rest.
    """
    DEFAULTS = {
        SdrObject.TYPE_ADSB: 5,
        SdrObject.TYPE_AIS: 30,
        SdrObject.TYPE_APRS: 60,
        SdrObject.TYPE_APRS_METEO: 60,
        SdrObject.TYPE_LRRP: 60,
        SdrObject.TYPE_RADIOSONDE: 60,
        SdrObject.TYPE_TETRA: 30,
        SdrObject.TYPE_SARSAT: 120,
        SdrObject.TYPE_SATELLITE: 30,
    }

    obj_type = models.CharField(
        max_length=16, choices=SdrObject.TYPE_CHOICES, unique=True
    )
    minutes = models.PositiveIntegerField(default=60)
    updated_at = models.DateTimeField(auto_now=True)

    class Meta:
        verbose_name = "Retention setting"
        verbose_name_plural = "Retention settings"
        ordering = ("obj_type",)

    def __str__(self):
        return f"{self.obj_type}: {self.minutes} min"

    @classmethod
    def get_minutes(cls, obj_type):
        """Returns the configured retention (minutes) for a given type."""
        row = cls.objects.filter(obj_type=obj_type).first()
        if row is not None:
            return row.minutes
        return cls.DEFAULTS.get(obj_type, 60)

    @classmethod
    def all_minutes(cls):
        """Dict {obj_type: minutes} for every known type."""
        existing = {r.obj_type: r.minutes for r in cls.objects.all()}
        return {t: existing.get(t, m) for t, m in cls.DEFAULTS.items()}

    @classmethod
    def cutoff(cls, obj_type):
        """Datetime threshold for `<` comparisons (older = stale)."""
        return timezone.now() - timedelta(minutes=cls.get_minutes(obj_type))
