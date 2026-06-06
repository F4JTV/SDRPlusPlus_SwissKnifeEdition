# ADS-B module for SDR++ — 1090 MHz decoder with TCP output

An **ADS-B** decoder (Mode S Extended Squitter, 1090 MHz) for **SDR++**, which
forwards each decoded aircraft over **TCP** to an external collector (for
example a separate mapping application), using the same approach as the AIS
module.

- **Modulation**: PPM (Pulse Position Modulation) at 1 Mbit/s, processed
  dump1090-style on an amplitude (magnitude) stream sampled at 2 MHz
  (2 samples per bit).
- **Decoding**: preamble detection → PPM bit slicing → Downlink Format
  reading → **CRC-24** check (Mode S polynomial `0x1FFF409`) → ME field
  parsing (identification, airborne/surface position, velocity).
- **Position**: **CPR** decoding, both global (even/odd frame pair) and local
  (from an optional reference position).
- **Output**: one JSON line per positioned aircraft, sent through a
  non-blocking TCP client (dedicated thread + queue + automatic reconnect),
  built on SDR++'s built-in `net::` utility (**no external dependencies**).

## 1. Get SDR++ and wire in the module

The module integrates **in-tree**, like the `pager_decoder`.

```bash
git clone https://github.com/AlexandreRouma/SDRPlusPlus.git
# Wire in the module (copy + CMake/core patch, idempotent):
./apply_to_sdrpp.sh /path/to/SDRPlusPlus
```

If you prefer to do it manually, apply:

Root `CMakeLists.txt`, in the decoder options section:
```cmake
option(OPT_BUILD_ADSB_DECODER "Build the ADS-B (1090 MHz) decoder module (no dependencies required)" ON)
```
Root `CMakeLists.txt`, in the decoder `add_subdirectory` section:
```cmake
if (OPT_BUILD_ADSB_DECODER)
add_subdirectory("decoder_modules/adsb_decoder")
endif (OPT_BUILD_ADSB_DECODER)
```
`core/src/core.cpp`, in the default module list:
```cpp
core::configManager.conf["modules"][modCount++] = "adsb_decoder.so";
```

## 2. Dependencies (Ubuntu 24.04)

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
    libfftw3-dev libglfw3-dev libvolk-dev libzstd-dev \
    librtlsdr-dev libhackrf-dev libairspy-dev libairspyhf-dev \
    libusb-1.0-0-dev
```

> The ADS-B module itself has **no external dependencies**: all networking goes
> through the `net::` utility built into `sdrpp_core`. The packages above are
> only needed to build SDR++ and its usual hardware sources.

## 3. Build

```bash
cd /path/to/SDRPlusPlus
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DOPT_BUILD_ADSB_DECODER=ON
make -j$(nproc)
sudo make install
sudo ldconfig
```

## 4. Usage inside SDR++

1. Launch `sdrpp`. If the module does not appear, open the **module manager**
   and add an `adsb_decoder` instance.
2. **Tune the SDR to 1090 MHz** as the center frequency, with a
   **sample rate >= 2 MHz** (an RTL-SDR at 2.4 MHz works fine).
3. The module creates its own centered VFO (offset 0, 2 MHz wide). Do not move
   it: ADS-B occupies the whole band around the carrier.
4. In the module menu:
   - **TCP output**: enable it and set the collector IP/port
     (default `127.0.0.1:10100`). The connection state is shown live.
   - **Reference position** (optional): if set, isolated position frames
     (without an even/odd pair) are decoded locally, which makes nearby
     aircraft appear faster. See the note below.
   - **Statistics**: number of CRC-OK frames, aircraft tracked / positioned.
   - **Open aircraft window**: opens a separate, movable/resizable window with
     the live aircraft table (ICAO, callsign, lat/lon, altitude, speed,
     heading, age). Rows stay in first-seen order — an aircraft never jumps
     rows. Moving this window does not affect the VFO.

> A 1090 MHz antenna (or a wire cut to ~6.9 cm) and ideally a 1090 SAW
> filter/LNA greatly increase the number of decoded frames.

### About "Use reference position"

ADS-B never sends a plain latitude/longitude; it sends CPR-encoded coordinates
that must be resolved. There are two ways to do it:

- **Global decode (default, no reference)**: each aircraft alternates "even"
  and "odd" position frames. With one fresh even frame **and** one fresh odd
  frame (the module requires them less than 10 s apart), the absolute position
  can be computed without any prior knowledge. Robust and universal, but you
  must wait for both halves of the pair before the aircraft appears.
- **Local decode (when "Use reference position" is enabled)**: given a
  reference position — typically your receiver's coordinates — the position can
  be resolved from a **single** frame, assuming the aircraft is nearby
  (unambiguous within roughly 180 NM of the reference while airborne).

The module always tries the global decode first; **only if it fails** (no fresh
even/odd pair yet) and the option is enabled does it fall back to a local decode
using your lat/lon.

Enabling it makes nearby aircraft appear sooner and lets you fix a position from
a single frame. The risk: a **wrong or too-distant** reference can resolve an
isolated frame into the wrong geographic cell and misplace the aircraft by
hundreds of kilometres. If you enable it, set your actual antenna position.

## 5. TCP output format

The module connects **as a client** to the collector and sends one JSON line per
positioned aircraft, terminated by `\n`:

```json
{
  "name":  "AFR1234",
  "icao":  "3c6dd2",
  "date":  "2026-05-25",
  "time":  "12:34:56",
  "lat":   43.295000,
  "lon":   5.370000,
  "type":  "ADSB",
  "speed": 420.0,
  "info":  "alt=38000ft hdg=270 vrate=-832fpm cs=AFR1234"
}
```

Fields (name, date, time, lat, lon, type, speed if available, misc info):

- `name`: callsign if known, otherwise `ICAO:xxxxxx`.
- `icao`: 24-bit ICAO address (lowercase hex), the stable aircraft identifier.
- `date` / `time`: **UTC** timestamp of the position fix.
- `lat` / `lon`: decoded position (CPR), in decimal degrees.
- `type`: always `"ADSB"`.
- `speed`: ground speed in **knots**, or `null` if not available.
- `info`: free-form field — altitude, heading, vertical rate, callsign.

## 6. Collector side (Django example)

The `django/` folder provides:

- `listen_adsb.py` — a management command that opens the TCP server, parses the
  JSON lines, stores contacts (`update_or_create` keyed by ICAO) and broadcasts
  them over a WebSocket (Django Channels).
- `django_integration_examples.py` — the `AdsbContact` model, ASGI routing, the
  `AdsbConsumer` (WebSocket) and the Leaflet JS to display/update aircraft
  markers in real time.

```bash
# In your mapping project:
cp django/listen_adsb.py <app>/management/commands/listen_adsb.py
# (then add the model, the consumer and the routing — see the examples file)
python manage.py listen_adsb --host 0.0.0.0 --port 10100
```

The SDR++ module must point its "TCP output" at this server's IP/port.

## 7. Included tests

- `test_decoder.cpp` — validates the pure decoding logic against known
  reference frames: CRC, identification (`KLM1023`), a CPR position pair
  (→ 52.2572°N, 3.9194°E), velocity (159 kt, heading 182.88°, −832 ft/min).
  ```bash
  g++ -std=c++17 -O2 -I src test_decoder.cpp src/adsb/adsb.cpp -o /tmp/adsb_test && /tmp/adsb_test
  ```
- `test_detector.cpp` — synthesizes a PPM stream (preamble + bits) and checks
  that the frame detector recovers identification, position and velocity.
  ```bash
  g++ -std=c++17 -O2 -I src test_detector.cpp src/adsb/adsb.cpp -o /tmp/adsb_det && /tmp/adsb_det
  ```
- `test_tcp.cpp` + `test_server.py` — a TCP-output test bench: the C++ client
  (via SDR++'s real `net::`) sends a few JSON frames to a small Python server.
  Compile from the SDR++ root:
  ```bash
  g++ -std=c++17 -O2 -I core/src -I decoder_modules/adsb_decoder/src \
      decoder_modules/adsb_decoder/test_tcp.cpp \
      core/src/utils/net.cpp core/src/utils/flog.cpp -lpthread -o /tmp/adsb_tcp
  python3 decoder_modules/adsb_decoder/test_server.py 10100 &
  /tmp/adsb_tcp 127.0.0.1 10100
  ```

## 8. Known limitations

- The decoder handles **DF17/DF18** frames (ADS-B). Other Mode S replies
  (DF0/4/5/11/20/21…) are not used for position.
- No single-bit error correction: any frame with a non-zero CRC is rejected.
  This is intentional, to avoid false positives on the map.
- Sensitivity depends heavily on the antenna and RF front-end filtering.
