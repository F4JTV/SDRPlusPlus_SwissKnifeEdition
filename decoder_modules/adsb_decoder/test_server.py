#!/usr/bin/env python3
# Minimal TCP server that mirrors listen_adsb.py's JSON-line parsing,
# used only to validate the SDR++ -> TCP -> collector chain end to end.
import json, socket, sys

port = int(sys.argv[1]) if len(sys.argv) > 1 else 10100
srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
srv.bind(("0.0.0.0", port))
srv.listen(1)
srv.settimeout(10.0)
print(f"server listening on {port}", flush=True)

conn, addr = srv.accept()
print(f"client connected {addr}", flush=True)
conn.settimeout(5.0)
buf = b""
count = 0
try:
    while True:
        chunk = conn.recv(4096)
        if not chunk:
            break
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line.decode())
            count += 1
            spd = rec.get("speed")
            spd = f"{spd}" if spd is not None else "null"
            print(f"RX {count}: name={rec['name']} icao={rec['icao']} "
                  f"lat={rec['lat']} lon={rec['lon']} type={rec['type']} speed={spd}",
                  flush=True)
except socket.timeout:
    pass
finally:
    conn.close()
    srv.close()
print(f"received {count} records", flush=True)
sys.exit(0 if count == 3 else 1)
