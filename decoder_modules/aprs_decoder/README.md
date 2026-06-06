# APRS module for SDR++ — `aprs_decoder`

**APRS** decoder (AX.25 / AFSK 1200 baud, Bell 202) for
[SDR++](https://github.com/AlexandreRouma/SDRPlusPlus), featuring:

- real-time decoding of APRS frames (positions, MIC-E, objects, items, etc.);
- decoding of APRS **weather stations** (temperature, humidity, wind, gusts,
  pressure, rain) — both position+weather (symbol `_`) and positionless reports;
- a **separate two-tab window**: *Positions* (positioned/misc traffic) and
  *Weather* (weather stations, in metric units), opened with a button — and it
  **does not move the VFO** when dragged over the waterfall;
- **TCP output** of one JSON line per positioned object, to a map (separate
  Django project): type `"APRS"` for positions and `"APRS Meteo"` for weather
  stations, using the **same base format as the AIS module**.

---

## 1. Archive contents

```
aprs_decoder/
├── src/
│   ├── dsp.h               ← DSP front-end: FM quadrature demod + DC blocker
│   ├── aprs/
│   │   ├── afsk.h          ← AFSK 1200 demod (dual-tone correlator) + bit PLL + HDLC + CRC X.25
│   │   ├── ax25.h          ← AX.25 frame parsing + APRS parsing (pos, MIC-E, compressed, objects, weather…)
│   │   └── symbols.h       ← icon sheet loading + ImGui rendering (OpenGL texture)
│   ├── tcp_sender.h        ← non-blocking TCP client + queue + auto-reconnect
│   └── main.cpp            ← ModuleManager module: VFO, GUI, 2-tab window, JSON output
├── resources/
│   └── aprs/               ← APRS icon sheets (installed to share/sdrpp/aprs)
│       ├── aprs-symbols-64-0.png   (primary table '/')
│       ├── aprs-symbols-64-1.png   (secondary table '\' + overlays)
│       ├── aprs-symbols-64-2.png   (overlay characters)
│       └── CREDITS.txt             (CC BY-SA 4.0 attribution)
├── CMakeLists.txt
├── CMAKE_INTEGRATION.txt   ← how to wire the module into the SDR++ tree
├── apply_to_sdrpp.sh       ← idempotent script that applies the CMake patches
├── BUILDING.md             ← in-tree build instructions (Ubuntu 24)
├── README.md               ← this file
├── test_decoder.cpp        ← end-to-end test (synthetic frame → audio → decode)
├── test_parse.cpp          ← MIC-E + compressed position tests (round-trip)
├── test_weather.cpp        ← weather-station parsing tests
└── django/
    ├── listen_aprs.py                  ← management command: TCP collector server
    └── django_integration_examples.py  ← model + WS consumer + routing + Leaflet JS
```

---

## 2. Dependencies (Ubuntu 24.04)

The module adds **no dependencies**: it only uses the SDR++ core (like
`pager_decoder`). To build SDR++ you need:

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
    libfftw3-dev libglfw3-dev libvolk-dev libzstd-dev \
    libairspy-dev librtlsdr-dev
```

(Adjust the SDR source `*-dev` packages to the devices you use;
`librtlsdr-dev` is enough for an RTL-SDR.)

---

## 3. Building

```bash
# 1. Clone SDR++ (if not already done)
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
cd SDRPlusPlus

# 2. Copy the module
cp -r /path/to/aprs_decoder decoder_modules/

# 3. Wire the module into CMake (idempotent)
cd decoder_modules/aprs_decoder
./apply_to_sdrpp.sh
cd ../..
#    (manual equivalent: see CMAKE_INTEGRATION.txt)

# 4. Build
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_APRS_DECODER=ON
make -j$(nproc)
sudo make install
sudo ldconfig
```

---

## 4. Using it in SDR++

1. **Module Manager** (left menu) → add an instance:
   *Type* = `aprs_decoder`, *Name* = e.g. `APRS`, then **+**.
2. The module creates a VFO (12.5 kHz channel). **Quick tune** buttons:
   - **144.800 (EU)** — Region 1 APRS frequency (Europe);
   - **144.390 (US)** — North America.
3. Put the VFO on the APRS channel; valid frames (FCS OK) appear when you click
   **Show packets** (separate window).
4. The tuning step (**Snap**) is adjustable (default 1000 Hz).

> Tip: to decode two frequencies at once, add **two instances** of the module
> (the number of instances is not limited).

### VFO drag protection

The "APRS packets" window locks the waterfall controls
(`lockWaterfallControls`) while it is hovered or focused. You can therefore drag
it over the waterfall without changing the VFO frequency (same fix as in the
POCSAG / AIS modules).

### APRS symbol icons

The first column (**Sym**) of both tabs shows the APRS symbol icon. The icon
sheets (open-source `hessu/aprs-symbols` set, 64 px, CC BY-SA 4.0) are bundled in
`resources/aprs/` and installed automatically by `make install` to
`<resourcesDirectory>/aprs/` (i.e. `/usr/share/sdrpp/aprs/` with the default
prefix). A **Show icons** checkbox appears in the toolbar when the sheets are
available.

If the sheets are missing, the column simply shows the 2-character symbol
(table + code) and a warning is written to the log; decoding is unaffected.
Hovering an icon shows its 2-character code as a tooltip.

> Icons are loaded into an OpenGL texture on first display of the window (GUI
> thread). Rendering crops the relevant cell from the sheet (16-column × 6-row
> grid, index = `code − '!'`).

---

## 5. TCP output to the map (format)

In the module: the **TCP output (map)** section → set *Host* (Django server IP)
and *Port* (default **10111**), then tick **Send decoded positions**. The status
(Connected / Disconnected) and the *Sent / Dropped* counters are shown below.

The module connects **as a TCP client** and sends **one JSON line per positioned
object** (terminated by `\n`). Schema (same as the AIS module, `type` =
`"APRS"`):

```json
{
  "name":   "N0CALL-9",
  "date":   "2026-05-25",
  "time":   "12:34:56",
  "lat":    43.30000,
  "lon":    5.36667,
  "type":   "APRS",
  "symbol": "/>",
  "speed":  36.0,
  "course": 88,
  "altitude_m": 376,
  "info":   "MIC-E sym=/> crs=88 via WIDE1-1 - comment"
}
```

- Field mapping: `name` = object name, `date` = date, `time` = time, `lat`/`lon`
  = latitude/longitude, `type` = object type (APRS), `symbol` = APRS symbol,
  `speed` = speed in knots (`null` if not reported), `course` = course in degrees
  (`null` if absent), `altitude_m` = altitude in metres (`null` if absent),
  `info` = miscellaneous info (APRS type, symbol, course, digipeater path,
  comment).
- **`course` / `altitude_m`**: also shown in dedicated columns of the
  *Positions* tab. Altitude comes from the `/A=dddddd` extension (feet, converted
  to metres), the MIC-E altitude, or the compressed bytes depending on the frame
  format.
- **`symbol`**: 2 characters = table identifier (`/` primary, `\` secondary, or
  an overlay character) followed by the symbol code. This is the key to use on
  the map side to draw the correct icon (see the Leaflet rendering provided in
  `django/django_integration_examples.py`, based on the open-source
  `hessu/aprs-symbols` set).
- **Only positioned objects** are sent; frames without a position (messages,
  status, telemetry…) remain visible in the window but are not forwarded to the
  map.
- Timestamps are **UTC**.

### Weather stations (`type":"APRS Meteo"`)

Weather stations are sent with `type` = `"APRS Meteo"` and additional weather
fields in **metric units** (in place of `speed`). Missing values are `null`:

```json
{
  "name": "N0CALL",
  "date": "2026-05-25",
  "time": "12:34:56",
  "lat": 49.058333,
  "lon": -72.029167,
  "type": "APRS Meteo",
  "symbol": "/_",
  "temp_c": 25.0,
  "humidity": 50,
  "wind_dir": 220,
  "wind_kmh": 6.4,
  "gust_kmh": 8.0,
  "pressure_hpa": 990.0,
  "rain_mm_1h": 0.0,
  "info": "Weather sym=/_ - ..."
}
```

Conversions applied by the module: temperature °F → °C, wind/gusts mph → km/h,
rain (1/100 inch) → mm, pressure already in hPa. The module window shows these
stations in the **Weather** tab (regular positions stay in the **Positions**
tab). On the map side, the Leaflet rendering provided in
`django/django_integration_examples.py` shows the weather parameters in the popup
when `type === "APRS Meteo"`.

---

## 6. Django side (reception + map)

Copy the management command into your app, then run the collector:

```bash
cp django/listen_aprs.py  <your_app>/management/commands/listen_aprs.py
python manage.py listen_aprs --host 0.0.0.0 --port 10111
```

The `AprsContact` model, the WebSocket consumer, the routing and the Leaflet JS
are provided (commented) in `django/django_integration_examples.py`. The command
also works **without** a model or Channels (console log only), while you set up
the integration.

---

## 7. Technical details

Processing chain:

```
complex IQ (VFO 24 kHz)
   → FM quadrature demod (dsp.h)
   → AFSK 1200: dual-tone correlator mark 1200 Hz / space 2200 Hz (afsk.h)
   → bit-clock recovery PLL + NRZI decoding
   → HDLC (flag 0x7E, bit de-stuffing, LSB-first assembly)
   → FCS CRC-16/X.25 check
   → AX.25 parsing (addresses, SSID, digipeater path) (ax25.h)
   → APRS parsing (position, MIC-E, compressed base-91, objects, items, weather…)
   → window + JSON TCP output
```

- Internal sample rate **24 kHz** = exactly 20 samples per bit at 1200 baud →
  short correlators and clean bit synchronization.
- CRC: polynomial `0x8408`, init `0xFFFF`, final XOR `0xFFFF` (X.25 / AX.25).

### Validation

- `test_decoder.cpp` — generates an AX.25 frame (HDLC + NRZI + bit-stuffing →
  AFSK audio) then decodes it: callsign, path, **position 43.30000, 5.36667**,
  course, speed, comment. **PASS.**
- `test_parse.cpp` — **MIC-E** and base-91 **compressed position** round-trip.
  **PASS.**
- `test_weather.cpp` — weather field parsing (position+weather, positionless,
  negative temperature, 100% humidity). **PASS.**

Building the tests (outside SDR++):

```bash
g++ -std=c++17 -O2 -I src test_decoder.cpp -o test_decoder && ./test_decoder
g++ -std=c++17 -O2 -I src test_parse.cpp   -o test_parse   && ./test_parse
g++ -std=c++17 -O2 -I src test_weather.cpp -o test_weather && ./test_weather
```

---

## 8. License / credits

This module is modeled on the SDR++ decoder modules (notably `pager_decoder`).
SDR++ is the work of Alexandre Rouma and its contributors. The bundled APRS
symbol graphics are from the `hessu/aprs-symbols` set (CC BY-SA 4.0); see
`resources/aprs/CREDITS.txt`.
