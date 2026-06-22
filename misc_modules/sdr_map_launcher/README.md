# SDR Map Launcher

A live map for SDR++ decoder modules. Picks up positions over a single TCP
socket from several decoders running in parallel, persists them in a small
Django app, and shows them on an offline-capable Leaflet map served locally.

Bundled here:

- The SDR++ module sources (`src/`, `CMakeLists.txt`)
- The full Django project (`sdr_map/`) — schema, views, static assets
- `install.sh` / `install.bat` — deploys the Django app and fetches the
  optional aircraft enrichment database
- `tools/build_aircraft_db.py` — converts the Mictronics dataset to a
  local SQLite for offline ADS-B model lookup

## What the module does in SDR++

A panel under the *Misc* section with Start/Stop and Open-map controls.
Start spawns the bundled Django web server (`runserver_sdr`), bound to the
host/port configured in the panel. The process is killed cleanly when SDR++
exits (Linux `prctl(PR_SET_PDEATHSIG)`, Windows `JobObject`).

## What other modules need to do

Each decoder module emits **one JSON line per positioned object** over a
TCP connection to the configured port (default 10100). All currently
supported types share the same envelope:

```json
{"name": "...", "date": "YYYY-MM-DD", "time": "HH:MM:SS",
 "lat": 43.7, "lon": 7.2, "type": "ADSB|AIS|APRS|APRS Meteo|lrrp|
 radiosonde|TETRA|SARSAT|satellite",
 "speed": 250, "info": "key=value ..."}
```

Per-type extras packed in `info` (or as top-level fields where listed):

| Type        | Identity     | Notable extras                                    |
|-------------|--------------|---------------------------------------------------|
| AIS         | MMSI         | `COG=...`, country derived from MID               |
| ADSB        | ICAO         | `hdg=`, `alt_ft=`, optional `category=A3/A7/B6/…` |
| APRS        | callsign     | weather sub-type for `APRS Meteo`                 |
| lrrp        | source RID   | DSD-FME DMR positions                             |
| radiosonde  | serial       | `alt_m=`, `climb=`                                |
| TETRA       | SSI          | `dir=...deg`, `acc=...m`                          |
| SARSAT      | 15-char Hex  | `beacon=`, `country=`, `protocol=`, `test=yes`    |
| satellite   | NORAD or name| `norad=`, `alt=`, `az=`, `el=`, `doppler=`,...    |

See `sdr_map/map/management/commands/listen_sdr.py` (the TCP collector)
for the exact field-by-field handling per type.

## Features by aircraft type (ADS-B)

The map shows 7 distinct icons selected from the **wake-vortex category**
emitted by the aircraft itself (Aircraft Identification message, TC 1-4):

| Icon       | Categories  | Examples                                  |
|------------|-------------|-------------------------------------------|
| airliner   | A3 / A4 / A5| A320, B737, A380, B747                    |
| light      | A1 / B4     | Cessna 172, PA-28, ultralights            |
| bizjet     | A2          | Citation, Pilatus, Falcon                 |
| fighter    | A6          | F-16, F-18, Eurofighter, Rafale, MIG, SU  |
| helicopter | A7          | EC135, R44, NH90, AW139                   |
| glider     | B1          | ASW27, DG808, Schleicher                  |
| drone      | B6          | UAVs                                      |

If the frame doesn't carry a category, a **local ICAO-to-model database**
(Mictronics, ~440 000 aircraft, ~18 MB SQLite) fills in registration,
exact type code, military flag, and a category derived from the type code
(so e.g. an Eurofighter — which transmits "no info" as category — still
gets the fighter icon). The database is downloaded by `install.sh` once
and used offline afterwards.

## Build & install

```bash
# from a fresh checkout of this SDR++ fork:
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_SDR_MAP_LAUNCHER=ON
make -j$(nproc) sdr_map_launcher
sudo cp misc_modules/sdr_map_launcher/sdr_map_launcher.so /usr/lib/sdrpp/plugins/

# deploy the Django app and the aircraft DB:
cd ../misc_modules/sdr_map_launcher
./install.sh
```

After that, restart SDR++, add the *SDR Map Launcher* module from the
Module Manager, click Start, then Open map.

## License

GPL v3, inherited from SDR++.
