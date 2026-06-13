#!/usr/bin/env python3
# Parse dumphfdl's etc/systable.conf into a self-contained C++ header used by the
# SDR++ HFDL module for (a) the channel list and (b) ground-station name/position
# lookup for the map output. No external dependency at build time.
import re, sys
src = open(sys.argv[1]).read()
ver = re.search(r'version\s*=\s*(\d+)', src)
ver = ver.group(1) if ver else "0"
stations = []
for blk in re.findall(r'\{(.*?)\}', src, re.S):
    gid = re.search(r'id\s*=\s*(\d+)', blk)
    lat = re.search(r'lat\s*=\s*(-?[\d.]+)', blk)
    lon = re.search(r'lon\s*=\s*(-?[\d.]+)', blk)
    name = re.search(r'name\s*=\s*"([^"]*)"', blk)
    freqs = re.search(r'frequencies\s*=\s*\(([^)]*)\)', blk)
    if not (gid and name): continue
    fl = [float(x) for x in re.findall(r'[\d.]+', freqs.group(1))] if freqs else []
    stations.append((int(gid.group(1)),
                     float(lat.group(1)) if lat else 0.0,
                     float(lon.group(1)) if lon else 0.0,
                     name.group(1), fl))
# freq -> list of station ids
freqmap = {}
for sid, la, lo, nm, fl in stations:
    for f in fl:
        freqmap.setdefault(f, []).append(sid)

out = []
out.append("// Auto-generated from dumphfdl etc/systable.conf (version %s)." % ver)
out.append("// Regenerate with tools/gen_systable.py. Do not edit by hand.")
out.append("#pragma once")
out.append("#include <cstdint>")
out.append("")
out.append("struct HfdlStation { int id; double lat; double lon; const char* name; };")
out.append("struct HfdlFreq    { double khz; int primaryStationId; };")
out.append("")
out.append("static const int HFDL_SYSTABLE_VERSION = %s;" % ver)
out.append("")
out.append("// Ground stations (id, lat, lon, name).")
out.append("static const HfdlStation HFDL_STATIONS[] = {")
for sid, la, lo, nm, fl in sorted(stations):
    out.append('    { %d, %.6f, %.6f, "%s" },' % (sid, la, lo, nm))
out.append("};")
out.append("static const int HFDL_STATION_CNT = (int)(sizeof(HFDL_STATIONS)/sizeof(HFDL_STATIONS[0]));")
out.append("")
out.append("// All assigned HFDL channel frequencies (kHz), ascending, with their")
out.append("// primary (lowest-id) ground station for the menu label.")
out.append("static const HfdlFreq HFDL_FREQS[] = {")
for f in sorted(freqmap):
    out.append("    { %.1f, %d }," % (f, min(freqmap[f])))
out.append("};")
out.append("static const int HFDL_FREQ_CNT = (int)(sizeof(HFDL_FREQS)/sizeof(HFDL_FREQS[0]));")
out.append("")
print("\n".join(out))

# Also emit the raw systable.conf as a C string so the module can write it out
# and hand it to dumphfdl via --system-table (enables GS name substitution).
def emit_raw(path):
    raw = open(path).read()
    lines = ['', '// Raw systable.conf, written to a temp file at runtime for dumphfdl --system-table.',
             'static const char* HFDL_SYSTABLE_CONF =']
    for ln in raw.splitlines():
        esc = ln.replace('\\', '\\\\').replace('"', '\\"')
        lines.append('    "%s\\n"' % esc)
    lines.append(';')
    return "\n".join(lines)

if __name__ == "__main__":
    import sys as _s
    print(emit_raw(_s.argv[1]))
