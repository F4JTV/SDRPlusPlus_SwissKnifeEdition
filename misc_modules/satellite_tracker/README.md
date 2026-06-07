# SDR++ Satellite Tracker

A satellite-tracking decoder/control module for [SDR++](https://github.com/AlexandreRouma/SDRPlusPlus).
It predicts satellite passes with **libpredict** (SGP4/SDP4), corrects the receive
(and transmit) frequency for **Doppler shift** in real time, downloads **TLE**
catalogues from CelesTrak, exposes an **embedded rigctl/hamlib server** (so
gpredict, GpsdoTest, WSJT-X, etc. can read/steer the tuned frequency), and streams
the live tracking state as line-delimited JSON to a map backend over TCP.

Author: **F4JTV** — built for ADRASEC 06.

---

## What it does

* **Orbital propagation** with the vendored libpredict engine (SGP4 for LEO,
  SDP4 for deep-space / geosynchronous objects). Az/El, slant range, range-rate,
  sub-satellite point, altitude, footprint, eclipse flag and decay detection.
* **Doppler correction** of the controlled VFO: the corrected downlink is
  `downlink - downlink * range_rate / c`. Uplink (if set) is corrected with the
  opposite sign. Correction only fires when the satellite is above the configured
  minimum elevation and the frequency delta exceeds a configurable step (Hz), to
  avoid hammering the tuner.
* **TLE management**: download & replace, or download & merge, from any CelesTrak
  group (amateur, weather, NOAA, cubesat, active, stations, GNSS) or a custom URL,
  plus add-by-NORAD. The catalogue is parsed with a faithful port of the SatDump
  TLE parser (checksum-validated) and cached on disk.
* **Embedded rigctl server** (the integrated rigctl base): a minimal hamlib TCP
  protocol (`F`/`f`, `M`/`m`, `\dump_state`, `\chk_vfo`, `\get_powerstat`, …) so
  external software can follow the Doppler-corrected frequency. Off by default.
* **Map output (TCP/JSON)**: non-blocking line sender with auto-reconnect, emitting
  one JSON object per update with `"type":"satellite"`, matching the AIS/ADS-B/
  radiosonde convention used by the SDR Map backend. Off by default.

---

## Build

The module is **in-tree** in an SDR++ source checkout.

### 1. Place the module

This is a *control* module (like `rigctl_server`), so it lives under
`misc_modules/`:

```
cp -r satellite_tracker <SDRPlusPlus>/misc_modules/satellite_tracker
```

> If you keep your whole decoder suite under `decoder_modules/` for consistency
> with `build_sdrpp_modules.sh`, you can put it there instead — just match the
> path in the `add_subdirectory` line below.

### 2. Wire it into the root `CMakeLists.txt`

Add an option near the other `OPT_BUILD_*` lines:

```cmake
option(OPT_BUILD_SATELLITE_TRACKER "Satellite tracker with Doppler correction (libpredict)" ON)
```

…and the subdirectory near the other `add_subdirectory` calls:

```cmake
if (OPT_BUILD_SATELLITE_TRACKER)
add_subdirectory("misc_modules/satellite_tracker")
endif (OPT_BUILD_SATELLITE_TRACKER)
```

### 3. System dependency

CelesTrak is HTTPS-only, so the module links libcurl:

```bash
sudo apt install libcurl4-openssl-dev
```

### 4. Build as usual

```bash
cd <SDRPlusPlus>
mkdir -p build && cd build
cmake .. -DOPT_BUILD_SATELLITE_TRACKER=ON
make -j$(nproc)
sudo make install
```

Then enable **satellite_tracker** in SDR++ → *Module Manager*.

---

## Usage

1. **Observer / QTH** — set your latitude, longitude and altitude (defaults to
   Marseille, 43.2965 N / 5.3698 E / 50 m).
2. **TLE catalogue** — pick a CelesTrak group (or a custom URL). **Selecting a
   source instantly shows that source's satellites** in the Track list, loaded
   from its own on-disk cache — no network access. Each source keeps its own
   cache file (`satellite_tracker_tles_<group>.txt`). The *Update* button is the
   only thing that downloads: it refreshes the currently selected source from
   CelesTrak and rewrites that source's cache. *Reload disk* re-reads the current
   source's cache. *Add NORAD* fetches a single object by catalogue number and
   merges it into the current source's cache. A source you have never updated
   shows an empty list until you press *Update* once.
3. **Satellite** — type in the search box to filter, then pick the satellite to
   track. When the satellite is known, **its downlink frequency is filled in
   automatically** (see below).
4. **Frequencies** — the downlink (and optional uplink) auto-fill on selection
   for known satellites; you can still type any value, or click *Set downlink =
   current VFO*. If a satellite has more than one known downlink (e.g. NOAA APT
   vs HRPT) a *Preset* dropdown lets you switch between them.
5. **Doppler correction** — choose the controlled VFO, tick *Enable Doppler*.
   The status word shows `off` / `armed` (waiting for AOS) / `tracking`. Tune the
   min-elevation gate and update interval as needed.
6. **Tracking** — a live table shows Az/El, range, range-rate, Doppler, the
   corrected RX/TX frequencies, sub-point, altitude, footprint, eclipse state and
   the AOS/LOS countdown with max elevation for the next pass.
7. **rigctl server** *(optional)* — start the embedded hamlib server on a
   host/port; external software can then poll the corrected frequency.
8. **Map output** *(optional)* — enable the TCP JSON stream to your SDR Map
   backend (default `127.0.0.1:10100`).

> By design, none of the *enable* toggles (Doppler, rigctl, map TCP) are persisted
> or auto-started — the module never opens a socket or steers the tuner on its own
> at startup. Host/port and all other settings are saved.

---

## JSON output format

One newline-delimited JSON object per update (throttled to ~0.5 Hz), following the
same `name / date / time / lat / lon / type / speed / info` schema as the other
SDR Map feeds (AIS, radiosonde, …). The satellite-specific telemetry is packed
into the `info` field. `lat`/`lon` are the sub-satellite point.

```json
{"name":"ISS (ZARYA)","date":"2026-06-07","time":"10:46:46",
 "lat":41.700000,"lon":6.200000,"type":"satellite","speed":null,
 "info":"norad=25544;alt=421;az=137.4;el=22.8;range=1043;doppler=1256;footprint=4541"}
```

---

## Automatic downlink frequencies

When you select a satellite, the module fills the downlink from a built-in
NORAD-indexed table (`src/satellite_freqs.h`) covering ~700 satellites.

A note on sourcing: SatDump keeps frequencies inside its *pipelines*
(`resources/pipelines/*.json`) keyed by **protocol** — e.g. GOES → HRIT 1694.1 /
LRIT 1691, NOAA → APT 137.x — with the satellite chosen separately by the user,
so the repo has no NORAD → frequency map to read directly. So the table is built
two ways: the **weather / imagery** downlinks (NOAA, Meteor, Metop, GOES, GK-2A)
are taken from those SatDump pipeline values and re-keyed by NORAD, and the
**amateur** downlinks are generated from the AMSAT / JE9PEL / SatNOGS amateur
satellite frequency list (re-entered, decayed and not-yet-launched objects are
dropped). Satellites with several published downlinks (e.g. ISS 145.800 voice /
437.800, or NOAA APT vs HRPT) expose a *Preset* selector so you can switch.

Satellites not in the table simply keep whatever downlink is set, and the table
is a plain list — add a `{ norad, hz, "label" }` row to extend it.

---

## TLE updates & CelesTrak rate limits

CelesTrak (behind Cloudflare) actively throttles frequent or large downloads, and
asks clients not to refresh a given group more than about once every two hours.
When you are throttled you will *not* get TLE data back — instead the status line
under *Update* now reports the real cause, e.g.:

* `HTTP 403 (rate-limited / blocked by CelesTrak)` — the request was rejected.
* `blocked / rate-limited (HTTP 200): <snippet>` — a 200 response whose body is an
  HTML/error page rather than TLEs (typical Cloudflare throttle).
* `Loaded N sats` — success.

The module enables gzip on the transfer, so the large `active` group (~2 MB
uncompressed, 10 000+ objects) downloads as a few hundred kB and no longer times
out. Practical advice: update only the small group you actually need (amateur,
weather, noaa, stations…), not `active`, and lean on the on-disk cache between
updates — the catalogue is reloaded automatically at startup and via *Reload disk*.

---

## Credits & licences

* **[libpredict](https://github.com/la1k/libpredict)** — orbital engine (SGP4/SDP4),
  GPLv2-or-later. Vendored under `src/predict/`.
* **[SatDump](https://github.com/SatDump/SatDump)** — reference for the TLE parser
  and the Doppler approach.
* **[SDR++](https://github.com/AlexandreRouma/SDRPlusPlus)** — host application;
  the embedded rigctl server is modelled on its `rigctl_server` module.

This module is distributed under the GPLv3, consistent with SDR++.
