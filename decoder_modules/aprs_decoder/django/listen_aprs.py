# decoder/management/commands/listen_aprs.py
#
# Serveur TCP qui reçoit les objets APRS décodés par le module SDR++
# aprs_decoder et les enregistre en base / les diffuse via WebSocket
# (Django Channels), dans le même style que listen_pocsag / listen_ais.
#
# Le module SDR++ se connecte EN TANT QUE CLIENT à ce serveur et envoie une
# ligne JSON par objet positionné (terminée par '\n'). Schéma (identique à
# celui du module AIS, type = "APRS") :
#
#   {
#     "name":   "F4JTV-9",          # indicatif / nom d'objet / nom d'item
#     "date":   "2026-05-25",       # date UTC de réception
#     "time":   "12:34:56",         # heure UTC de réception
#     "lat":    43.30000,
#     "lon":    5.36667,
#     "type":   "APRS",
#     "symbol": "/>",               # symbole APRS : table ('/' '\' ou overlay) + code
#     "speed":  36.0,               # vitesse en nœuds, ou null si non renseignée
#     "course": 88,                 # cap en degrés, ou null
#     "altitude_m": 376,            # altitude en mètres, ou null
#     "info":   "MIC-E sym=/> crs=88 via WIDE1-1 - ADRASEC 06 mobile"
#   }
#
# Les stations météo sont envoyées avec type = "APRS Meteo" et des champs météo
# supplémentaires (unités métriques, null si absent) à la place de "speed" :
#
#   {
#     "name":"F4JTV","date":"...","time":"...","lat":..,"lon":..,
#     "type":"APRS Meteo","symbol":"/_",
#     "temp_c":25.0,"humidity":50,"wind_dir":220,"wind_kmh":6.4,
#     "gust_kmh":8.0,"pressure_hpa":990.0,"rain_mm_1h":0.0,
#     "info":"Weather sym=/_ - ..."
#   }
#
# Lancement :
#   python manage.py listen_aprs --host 0.0.0.0 --port 10111
#
# (Dans SDR++, régler le "TCP output (map)" du module sur l'IP/port de ce
#  serveur, puis cocher "Send decoded positions".)

import json
import socket
import threading
from datetime import datetime

from django.core.management.base import BaseCommand
from django.utils import timezone

# Import « souple » : si le modèle ou Channels ne sont pas encore en place,
# la commande fonctionne quand même (log console seul). Adaptez le chemin du
# modèle à votre projet (voir django_integration_examples.py).
try:
    from decoder.models import AprsContact
    HAS_MODEL = True
except Exception:
    AprsContact = None
    HAS_MODEL = False

try:
    from channels.layers import get_channel_layer
    from asgiref.sync import async_to_sync
    CHANNEL_LAYER = get_channel_layer()
except Exception:
    CHANNEL_LAYER = None

# Groupe WebSocket sur lequel la carte est abonnée (voir le Consumer).
WS_GROUP = "aprs"
# Au-delà de ce nombre d'enregistrements en base, on purge les plus anciens.
MAX_ROWS = 5000


class Command(BaseCommand):
    help = "Serveur TCP collecteur des objets APRS décodés par SDR++ (aprs_decoder)."

    def add_arguments(self, parser):
        parser.add_argument("--host", default="0.0.0.0", help="Adresse d'écoute (défaut 0.0.0.0)")
        parser.add_argument("--port", type=int, default=10111, help="Port d'écoute (défaut 10111)")

    def handle(self, *args, **options):
        host = options["host"]
        port = options["port"]
        self.running = True

        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind((host, port))
        srv.listen(5)
        srv.settimeout(1.0)  # pour pouvoir interrompre proprement (Ctrl+C)

        self.stdout.write(self.style.SUCCESS(
            f"\n✓ listen_aprs : écoute TCP sur {host}:{port} (Ctrl+C pour arrêter)\n"))
        if not HAS_MODEL:
            self.stdout.write(self.style.WARNING(
                "  (modèle AprsContact introuvable : enregistrement DB désactivé, log seul)"))
        if CHANNEL_LAYER is None:
            self.stdout.write(self.style.WARNING(
                "  (Channels indisponible : diffusion WebSocket désactivée)"))

        try:
            while self.running:
                try:
                    conn, addr = srv.accept()
                except socket.timeout:
                    continue
                self.stdout.write(self.style.SUCCESS(f"→ connexion SDR++ depuis {addr[0]}:{addr[1]}"))
                t = threading.Thread(target=self._handle_client, args=(conn, addr), daemon=True)
                t.start()
        except KeyboardInterrupt:
            self.stdout.write(self.style.WARNING("\n\nArrêt de l'écoute…"))
        finally:
            self.running = False
            try:
                srv.close()
            except Exception:
                pass
            self.stdout.write(self.style.WARNING("listen_aprs arrêté."))

    # ---- une connexion client (= une instance SDR++) -----------------------
    def _handle_client(self, conn, addr):
        buf = b""
        conn.settimeout(1.0)
        try:
            while self.running:
                try:
                    chunk = conn.recv(4096)
                except socket.timeout:
                    continue
                if not chunk:
                    break  # connexion fermée côté SDR++
                buf += chunk
                # Le module envoie une ligne JSON par objet, séparée par '\n'.
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    line = line.strip()
                    if line:
                        self._process_line(line)
        except Exception as e:
            self.stdout.write(self.style.ERROR(f"Erreur client {addr[0]} : {e}"))
        finally:
            try:
                conn.close()
            except Exception:
                pass
            self.stdout.write(self.style.WARNING(f"← déconnexion {addr[0]}:{addr[1]}"))

    # ---- traitement d'une ligne JSON ---------------------------------------
    def _process_line(self, raw):
        try:
            obj = json.loads(raw.decode("utf-8", errors="replace"))
        except json.JSONDecodeError:
            self.stdout.write(self.style.ERROR(f"JSON invalide ignoré : {raw[:120]!r}"))
            return

        name = str(obj.get("name", "") or "?")
        lat = obj.get("lat")
        lon = obj.get("lon")
        speed = obj.get("speed", None)  # peut être null
        course = obj.get("course", None)        # cap en degrés (peut être null)
        altitude_m = obj.get("altitude_m", None)  # altitude en mètres (peut être null)
        symbol = str(obj.get("symbol", "") or "")  # 2 car. APRS : table + code
        info = str(obj.get("info", "") or "")
        d = obj.get("date", "")
        h = obj.get("time", "")
        # type = "APRS" (position) ou "APRS Meteo" (station météo)
        rec_type = str(obj.get("type", "APRS") or "APRS")
        is_weather = (rec_type == "APRS Meteo")
        # Champs météo (présents et éventuellement null si type = APRS Meteo)
        wx = {k: obj.get(k) for k in (
            "temp_c", "humidity", "wind_dir", "wind_kmh",
            "gust_kmh", "pressure_hpa", "rain_mm_1h")}

        if lat is None or lon is None:
            return  # le module ne devrait envoyer que des objets positionnés

        # Horodatage : on respecte la date/heure UTC fournies par le module ;
        # à défaut on prend l'heure courante.
        ts = timezone.now()
        if d and h:
            try:
                naive = datetime.strptime(f"{d} {h}", "%Y-%m-%d %H:%M:%S")
                ts = timezone.make_aware(naive, timezone.utc)
            except ValueError:
                pass

        if is_weather:
            t = wx.get("temp_c")
            t_str = f"{t:.1f}C" if isinstance(t, (int, float)) else "—"
            self.stdout.write(self.style.SUCCESS(
                f"✓ APRS Meteo [{name}] {lat:.5f},{lon:.5f}  T={t_str}  {info[:50]}"))
        else:
            speed_str = f"{speed:.0f} kn" if isinstance(speed, (int, float)) else "—"
            self.stdout.write(self.style.SUCCESS(
                f"✓ APRS [{name}] {lat:.5f},{lon:.5f}  v={speed_str}  {info[:60]}"))

        # 1) Enregistrement en base
        contact = None
        if HAS_MODEL:
            try:
                contact = AprsContact.objects.create(
                    name=name,
                    latitude=float(lat),
                    longitude=float(lon),
                    speed=(float(speed) if isinstance(speed, (int, float)) else None),
                    course=(int(course) if isinstance(course, (int, float)) else None),
                    altitude_m=(float(altitude_m) if isinstance(altitude_m, (int, float)) else None),
                    symbol=symbol,
                    kind=rec_type,
                    temp_c=(float(wx["temp_c"]) if isinstance(wx["temp_c"], (int, float)) else None),
                    humidity=(int(wx["humidity"]) if isinstance(wx["humidity"], (int, float)) else None),
                    wind_dir=(int(wx["wind_dir"]) if isinstance(wx["wind_dir"], (int, float)) else None),
                    wind_kmh=(float(wx["wind_kmh"]) if isinstance(wx["wind_kmh"], (int, float)) else None),
                    gust_kmh=(float(wx["gust_kmh"]) if isinstance(wx["gust_kmh"], (int, float)) else None),
                    pressure_hpa=(float(wx["pressure_hpa"]) if isinstance(wx["pressure_hpa"], (int, float)) else None),
                    rain_mm_1h=(float(wx["rain_mm_1h"]) if isinstance(wx["rain_mm_1h"], (int, float)) else None),
                    info=info,
                    timestamp=ts,
                    raw=raw.decode("utf-8", errors="replace"),
                )
                # Purge des plus anciens
                if AprsContact.objects.count() > MAX_ROWS:
                    old = AprsContact.objects.order_by("timestamp").first()
                    if old:
                        old.delete()
            except Exception as e:
                self.stdout.write(self.style.ERROR(f"Erreur DB : {e}"))

        # 2) Diffusion WebSocket vers la carte
        payload = {
            "name": name,
            "lat": float(lat),
            "lon": float(lon),
            "speed": (float(speed) if isinstance(speed, (int, float)) else None),
            "course": (int(course) if isinstance(course, (int, float)) else None),
            "altitude_m": (float(altitude_m) if isinstance(altitude_m, (int, float)) else None),
            "symbol": symbol,
            "type": rec_type,
            "info": info,
            "date": d,
            "time": h,
            "id": (contact.id if contact is not None else None),
        }
        if is_weather:
            payload.update(wx)   # temp_c, humidity, wind_dir, wind_kmh, gust_kmh, pressure_hpa, rain_mm_1h
        self._broadcast(payload)

    def _broadcast(self, payload):
        if CHANNEL_LAYER is None:
            return
        try:
            async_to_sync(CHANNEL_LAYER.group_send)(
                WS_GROUP, {"type": "aprs.message", "data": payload})
        except Exception as e:
            self.stdout.write(self.style.ERROR(f"Erreur WebSocket : {e}"))
