# =============================================================================
#  django_integration_examples.py
#
#  Exemples à recopier dans votre projet Django (carte ADRASEC 06) pour
#  recevoir et afficher les objets APRS envoyés par le module SDR++.
#  Ce fichier n'est PAS importé tel quel : c'est un aide-mémoire. Copiez
#  chaque bloc dans le fichier indiqué.
#
#  Vue d'ensemble du flux :
#     SDR++ aprs_decoder  --TCP(JSON)-->  listen_aprs (management command)
#         --> AprsContact (DB)  +  group_send  --WebSocket-->  carte Leaflet
# =============================================================================


# -----------------------------------------------------------------------------
# 1) MODÈLE  ->  decoder/models.py
# -----------------------------------------------------------------------------
"""
from django.db import models


class AprsContact(models.Model):
    name       = models.CharField(max_length=64, db_index=True)   # indicatif / objet
    latitude   = models.FloatField()
    longitude  = models.FloatField()
    speed      = models.FloatField(null=True, blank=True)         # nœuds (peut être null)
    course     = models.IntegerField(null=True, blank=True)       # cap en degrés
    altitude_m = models.FloatField(null=True, blank=True)         # altitude en mètres
    symbol     = models.CharField(max_length=2, blank=True)       # symbole APRS (table+code)
    kind       = models.CharField(max_length=16, default="APRS")  # "APRS" ou "APRS Meteo"
    info       = models.CharField(max_length=512, blank=True)
    timestamp  = models.DateTimeField(db_index=True)              # UTC
    raw        = models.TextField(blank=True)                     # ligne JSON brute

    # Champs météo (renseignés uniquement quand kind == "APRS Meteo"), métriques
    temp_c       = models.FloatField(null=True, blank=True)       # °C
    humidity     = models.IntegerField(null=True, blank=True)     # %
    wind_dir     = models.IntegerField(null=True, blank=True)     # degrés
    wind_kmh     = models.FloatField(null=True, blank=True)       # km/h
    gust_kmh     = models.FloatField(null=True, blank=True)       # km/h
    pressure_hpa = models.FloatField(null=True, blank=True)       # hPa
    rain_mm_1h   = models.FloatField(null=True, blank=True)       # mm

    class Meta:
        ordering = ['-timestamp']

    def __str__(self):
        return f"{self.name} @ {self.latitude:.5f},{self.longitude:.5f}"
"""
# Puis :  python manage.py makemigrations decoder && python manage.py migrate


# -----------------------------------------------------------------------------
# 2) CONSUMER WebSocket  ->  decoder/consumers.py
# -----------------------------------------------------------------------------
"""
import json
from channels.generic.websocket import AsyncWebsocketConsumer


class AprsConsumer(AsyncWebsocketConsumer):
    GROUP = "aprs"

    async def connect(self):
        await self.channel_layer.group_add(self.GROUP, self.channel_name)
        await self.accept()

    async def disconnect(self, code):
        await self.channel_layer.group_discard(self.GROUP, self.channel_name)

    # Déclenché par group_send(type="aprs.message") dans listen_aprs.py
    async def aprs_message(self, event):
        await self.send(text_data=json.dumps(event["data"]))
"""


# -----------------------------------------------------------------------------
# 3) ROUTING WebSocket  ->  decoder/routing.py
# -----------------------------------------------------------------------------
"""
from django.urls import re_path
from . import consumers

websocket_urlpatterns = [
    re_path(r'ws/aprs/$', consumers.AprsConsumer.as_asgi()),
]
"""

# ... et dans  <projet>/asgi.py  (si pas déjà fait pour POCSAG/AIS) :
"""
import os
from django.core.asgi import get_asgi_application
from channels.routing import ProtocolTypeRouter, URLRouter
from channels.auth import AuthMiddlewareStack

os.environ.setdefault('DJANGO_SETTINGS_MODULE', '<projet>.settings')
django_asgi_app = get_asgi_application()

from decoder.routing import websocket_urlpatterns  # noqa: E402

application = ProtocolTypeRouter({
    'http': django_asgi_app,
    'websocket': AuthMiddlewareStack(URLRouter(websocket_urlpatterns)),
})
"""

# settings.py — si Channels n'est pas déjà configuré :
"""
INSTALLED_APPS += ['channels']
ASGI_APPLICATION = '<projet>.asgi.application'
# Couche en mémoire (mono-processus). Pour multi-process, utiliser Redis.
CHANNEL_LAYERS = {'default': {'BACKEND': 'channels.layers.InMemoryChannelLayer'}}
"""


# -----------------------------------------------------------------------------
# 4) VUE d'historique (optionnelle)  ->  decoder/views.py
# -----------------------------------------------------------------------------
"""
from django.http import JsonResponse
from .models import AprsContact

def aprs_recent(request):
    # Renvoie les 500 derniers objets pour peupler la carte au chargement.
    rows = AprsContact.objects.all()[:500]
    return JsonResponse({'contacts': [
        {
            'name': r.name, 'lat': r.latitude, 'lon': r.longitude,
            'speed': r.speed, 'course': r.course, 'altitude_m': r.altitude_m,
            'symbol': r.symbol, 'info': r.info,
            'type': r.kind, 'time': r.timestamp.isoformat(),
            'temp_c': r.temp_c, 'humidity': r.humidity,
            'wind_dir': r.wind_dir, 'wind_kmh': r.wind_kmh,
            'gust_kmh': r.gust_kmh, 'pressure_hpa': r.pressure_hpa,
            'rain_mm_1h': r.rain_mm_1h,
        } for r in rows
    ]})
"""
# urls.py :  path('api/aprs/recent/', views.aprs_recent, name='aprs_recent')


# -----------------------------------------------------------------------------
# 5) CÔTÉ CARTE (Leaflet)  ->  template JS
# -----------------------------------------------------------------------------
LEAFLET_JS = r"""
// Carte centrée sur les Alpes-Maritimes (ADRASEC 06)
const map = L.map('map').setView([43.70, 7.25], 9);
L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
    attribution: '© OpenStreetMap'
}).addTo(map);

// --- Rendu des symboles APRS (jeu open-source hessu/aprs-symbols) -----------
// Téléchargez les 3 planches PNG 24 px et placez-les dans  static/aprs/ :
//   aprs-symbols-24-0.png   (table primaire  '/')
//   aprs-symbols-24-1.png   (table secondaire '\' + base des overlays)
//   aprs-symbols-24-2.png   (caractères d'overlay)
// Source : https://github.com/hessu/aprs-symbols  (licence CC BY-SA 4.0)
// Disposition : grille de 16 colonnes ; index = code('!'=0) ; le champ JSON
// "symbol" contient 2 caractères = identifiant de table + code symbole.
const APRS_SPRITE_BASE = '/static/aprs/';
const APRS_CELL = 24; // px (utilisez les planches @2x pour les écrans Retina)

function aprsSymbolDivIcon(symbol) {
    const table = (symbol && symbol.length >= 1) ? symbol[0] : '/';
    const code  = (symbol && symbol.length >= 2) ? symbol[1] : '>';
    const idx = code.charCodeAt(0) - 33;           // '!' -> 0
    if (idx < 0 || idx > 93) return null;          // hors plage -> marqueur défaut
    const col = idx % 16, row = Math.floor(idx / 16);
    const bx = -(col * APRS_CELL), by = -(row * APRS_CELL);
    const isPrimary   = (table === '/');
    const isSecondary = (table === '\\');
    const sheet = isPrimary ? 0 : 1;               // overlay -> planche secondaire

    let html =
        `<div style="width:${APRS_CELL}px;height:${APRS_CELL}px;` +
        `background:url('${APRS_SPRITE_BASE}aprs-symbols-24-${sheet}.png') ${bx}px ${by}px no-repeat;"></div>`;

    // Overlay : si la table n'est ni '/' ni '\', c'est un caractère d'overlay
    // dessiné par-dessus la base (planche 2).
    if (!isPrimary && !isSecondary) {
        const oidx = table.charCodeAt(0) - 33;
        if (oidx >= 0 && oidx <= 93) {
            const ocol = oidx % 16, orow = Math.floor(oidx / 16);
            const obx = -(ocol * APRS_CELL), oby = -(orow * APRS_CELL);
            html =
                `<div style="position:relative;width:${APRS_CELL}px;height:${APRS_CELL}px;` +
                `background:url('${APRS_SPRITE_BASE}aprs-symbols-24-1.png') ${bx}px ${by}px no-repeat;">` +
                `<div style="position:absolute;inset:0;` +
                `background:url('${APRS_SPRITE_BASE}aprs-symbols-24-2.png') ${obx}px ${oby}px no-repeat;"></div>` +
                `</div>`;
        }
    }
    return L.divIcon({
        html, className: 'aprs-symbol',
        iconSize: [APRS_CELL, APRS_CELL],
        iconAnchor: [APRS_CELL / 2, APRS_CELL / 2],
    });
}

const aprsMarkers = {};   // name -> marker (un marqueur par station, mis à jour)

function upsertAprs(d) {
    const key = d.name;
    const icon = aprsSymbolDivIcon(d.symbol || '/>');
    let popup = `<b>${d.name}</b><br>${d.lat.toFixed(5)}, ${d.lon.toFixed(5)}`;
    if (d.symbol) popup += `<br>Symbole : <code>${d.symbol}</code>`;

    if (d.type === 'APRS Meteo') {
        // Station météo : afficher les paramètres disponibles.
        const w = [];
        if (d.temp_c !== null && d.temp_c !== undefined)        w.push(`🌡️ ${d.temp_c.toFixed(1)} °C`);
        if (d.humidity !== null && d.humidity !== undefined)    w.push(`💧 ${d.humidity} %`);
        if (d.wind_kmh !== null && d.wind_kmh !== undefined) {
            let s = `🌬️ ${d.wind_kmh.toFixed(0)} km/h`;
            if (d.wind_dir !== null && d.wind_dir !== undefined) s += ` @ ${d.wind_dir}°`;
            w.push(s);
        }
        if (d.gust_kmh !== null && d.gust_kmh !== undefined)    w.push(`rafales ${d.gust_kmh.toFixed(0)} km/h`);
        if (d.pressure_hpa !== null && d.pressure_hpa !== undefined) w.push(`${d.pressure_hpa.toFixed(1)} hPa`);
        if (d.rain_mm_1h !== null && d.rain_mm_1h !== undefined) w.push(`🌧️ ${d.rain_mm_1h.toFixed(1)} mm/h`);
        if (w.length) popup += `<br>` + w.join('<br>');
    } else {
        if (d.speed !== null && d.speed !== undefined) popup += `<br>Vitesse : ${d.speed.toFixed(0)} kn`;
        if (d.course !== null && d.course !== undefined) popup += `<br>Cap : ${d.course}°`;
        if (d.altitude_m !== null && d.altitude_m !== undefined) popup += `<br>Altitude : ${d.altitude_m.toFixed(0)} m`;
    }

    if (d.info) popup += `<br><small>${d.info}</small>`;
    if (d.time) popup += `<br><small>${d.date || ''} ${d.time} UTC</small>`;

    if (aprsMarkers[key]) {
        aprsMarkers[key].setLatLng([d.lat, d.lon]).setPopupContent(popup);
        if (icon) aprsMarkers[key].setIcon(icon);
    } else {
        aprsMarkers[key] = L.marker([d.lat, d.lon], icon ? { icon } : {})
            .addTo(map).bindPopup(popup);
    }
}

// 1) Charger l'historique récent (optionnel)
fetch('/api/aprs/recent/')
    .then(r => r.json())
    .then(j => (j.contacts || []).forEach(upsertAprs))
    .catch(() => {});

// 2) Temps réel via WebSocket
const proto = (location.protocol === 'https:') ? 'wss' : 'ws';
const ws = new WebSocket(`${proto}://${location.host}/ws/aprs/`);
ws.onmessage = (ev) => upsertAprs(JSON.parse(ev.data));
ws.onclose = () => setTimeout(() => location.reload(), 5000); // reconnexion simple
"""


# -----------------------------------------------------------------------------
# 6) LANCEMENT
# -----------------------------------------------------------------------------
"""
# Terminal 1 — serveur web ASGI (Daphne) :
daphne -b 0.0.0.0 -p 8000 <projet>.asgi:application

# Terminal 2 — collecteur TCP APRS :
python manage.py listen_aprs --host 0.0.0.0 --port 10111

# Dans SDR++ : module aprs_decoder -> TCP output -> Host = IP du serveur Django,
# Port = 10111, cocher "Send decoded positions".
"""
