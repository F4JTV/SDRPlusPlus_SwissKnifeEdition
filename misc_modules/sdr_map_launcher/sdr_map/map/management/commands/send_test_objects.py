# map/management/commands/send_test_objects.py
#
# Simulateur : se connecte au serveur listen_sdr (comme le feraient les modules
# SDR++) et envoie des objets de test AIS / ADS-B / APRS / APRS Meteo qui se
# move around, so the map can be validated without real hardware.
#
#   python manage.py send_test_objects                 # 127.0.0.1:10100
#   python manage.py send_test_objects --port 10100 --interval 2

import json
import math
import socket
import time
from datetime import datetime, timezone

from django.core.management.base import BaseCommand

CENTER = (43.70, 7.25)  # Côte d'Azur


class Command(BaseCommand):
    help = "Send test SDR objects to the listen_sdr server (simulator)."

    def add_arguments(self, parser):
        parser.add_argument("--host", default="127.0.0.1")
        parser.add_argument("--port", type=int, default=10100)
        parser.add_argument("--interval", type=float, default=2.0,
                            help="Seconds between two bursts (default: 2)")

    def handle(self, *args, **opts):
        host, port, interval = opts["host"], opts["port"], opts["interval"]
        self.stdout.write(self.style.SUCCESS(
            f"Connecting to server {host}:{port}..."))
        sock = socket.create_connection((host, port), timeout=5)
        self.stdout.write(self.style.SUCCESS("Connected. Press Ctrl+C to stop.\n"))

        t = 0.0
        try:
            while True:
                now = datetime.now(timezone.utc)
                d, h = now.strftime("%Y-%m-%d"), now.strftime("%H:%M:%S")

                # AIS ship hugging the coast
                lat = CENTER[0] - 0.10 + 0.02 * math.sin(t / 10)
                lon = CENTER[1] + 0.05 * math.cos(t / 10)
                self._send(sock, {
                    "name": "TEST VESSEL", "mmsi": 227006760,
                    "date": d, "time": h, "lat": round(lat, 6),
                    "lon": round(lon, 6), "type": "AIS", "speed": 10.0,
                    "info": "MMSI=227006760 msg=1 COG=87.5 ship=Cargo",
                })

                # ADS-B aircraft passing through
                alat = CENTER[0] + 0.20 - 0.004 * t
                alon = CENTER[1] - 0.30 + 0.006 * t
                hdg = int((45 + t) % 360)
                self._send(sock, {
                    "name": "AFR1234", "icao": "3c6dd2",
                    "date": d, "time": h, "lat": round(alat, 6),
                    "lon": round(alon, 6), "type": "ADSB", "speed": 420.0,
                    "info": f"alt=38000ft hdg={hdg} vrate=-832fpm cs=AFR1234",
                })

                # Mobile APRS station
                platy = CENTER[0] + 0.03 * math.cos(t / 8)
                plonx = CENTER[1] + 0.10 + 0.03 * math.sin(t / 8)
                self._send(sock, {
                    "name": "F4JTV-9", "date": d, "time": h,
                    "lat": round(platy, 6), "lon": round(plonx, 6),
                    "type": "APRS", "speed": 36.0,
                    "info": "MIC-E sym=/> crs=88 via WIDE1-1 - ADRASEC 06 mobile",
                })

                # APRS weather station (fixed)
                self._send(sock, {
                    "name": "F4JTV-13", "date": d, "time": h,
                    "lat": CENTER[0] + 0.12, "lon": CENTER[1] - 0.05,
                    "type": "APRS Meteo",
                    "temperature": round(14 + 3 * math.sin(t / 20), 1),
                    "humidity": 65, "wind_dir": int((180 + t) % 360),
                    "wind_speed": round(12 + 4 * math.sin(t / 5), 1),
                    "wind_gust": round(22 + 5 * math.sin(t / 5), 1),
                    "pressure": 1013.2, "rain": 0.0,
                    "info": "Weather station",
                })

                # LRRP / DMR GPS beacon (DSD-FME), mobile RID
                llat = CENTER[0] - 0.05 + 0.02 * math.sin(t / 12)
                llon = CENTER[1] - 0.12 + 0.02 * math.cos(t / 12)
                self._send(sock, {
                    "name": "RID 2081371", "date": d, "time": h,
                    "lat": round(llat, 6), "lon": round(llon, 6),
                    "type": "lrrp", "speed": None, "source": "2081371",
                    "info": f"DMRA LRRP lat={llat:.4f} lon={llon:.4f} RID=2081371",
                })

                # RS41 radiosonde: drift + altitude profile (~5 m/s climb,
                # then burst and descent). info = key=value sub-fields.
                slat = CENTER[0] + 0.30 + 0.004 * t
                slon = CENTER[1] + 0.20 + 0.003 * t
                cycle = t % 240
                if cycle < 140:           # ascent
                    alt = 1000 + cycle * 250
                    climb = 5.0
                else:                     # descent under parachute
                    alt = max(300, 36000 - (cycle - 140) * 350)
                    climb = -8.5
                self._send(sock, {
                    "name": "W1234567", "date": d, "time": h,
                    "lat": round(slat, 6), "lon": round(slon, 6),
                    "type": "radiosonde", "speed": round(7 + 2 * math.sin(t / 6), 1),
                    "info": (f"alt={int(alt)};hdg={int((90 + t) % 360)};"
                             f"climb={climb};temp={round(-0.0065*alt + 15, 1)};"
                             f"rh={45};p={round(1013 * math.exp(-alt/7000), 1)}"),
                })

                self.stdout.write(f"Burst sent (t={t:.0f}s)")
                t += interval
                time.sleep(interval)
        except KeyboardInterrupt:
            self.stdout.write(self.style.WARNING("\nSimulator stopped."))
        finally:
            sock.close()

    def _send(self, sock, obj):
        sock.sendall((json.dumps(obj, ensure_ascii=False) + "\n").encode("utf-8"))
