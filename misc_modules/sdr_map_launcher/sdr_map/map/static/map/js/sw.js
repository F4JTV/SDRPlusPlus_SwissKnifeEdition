/* =========================================================================
   Service Worker — SDR MAP
   - Cache de l'« app shell » (Leaflet, CSS, JS) pour un démarrage hors-ligne.
   - Cache des tuiles cartographiques (cache-first) + préchargement de zone.
   Servi depuis '/sw.js' (scope '/') pour intercepter les tuiles cross-origin.
   ========================================================================= */
const VERSION = "sdrmap-v22";
const SHELL_CACHE = "shell-" + VERSION;
const TILE_CACHE = "tiles-" + VERSION;

// Ressources de base de l'application (chemins servis par Django/staticfiles).
const SHELL_ASSETS = [
  "/",
  "/static/map/css/style.css",
  "/static/map/js/map.js",
  "/static/map/vendor/leaflet/leaflet.js",
  "/static/map/vendor/leaflet/leaflet.css",
  "/static/map/vendor/leaflet/images/marker-icon.png",
  "/static/map/vendor/leaflet/images/marker-icon-2x.png",
  "/static/map/vendor/leaflet/images/marker-shadow.png",
  "/static/map/vendor/leaflet/images/layers.png",
  "/static/map/vendor/leaflet/images/layers-2x.png",
];

// Hôtes de tuiles connus (cache-first).
const TILE_HOSTS = [
  "tile.openstreetmap.org",
  "tile.openstreetmap.fr",
  "tile.opentopomap.org",
  "server.arcgisonline.com",
  "basemaps.cartocdn.com",
];

function isTile(url) {
  try {
    const u = new URL(url);
    if (TILE_HOSTS.some((h) => u.hostname.endsWith(h))) return true;
    // Repli générique : chemins de tuiles /{z}/{x}/{y}
    return /\/\d{1,2}\/\d+\/\d+(@2x)?\.(png|jpg|jpeg)/i.test(u.pathname)
      || /MapServer\/tile\/\d+\/\d+\/\d+/i.test(u.pathname);
  } catch { return false; }
}

// ------------------------------------------------------------ install -----
self.addEventListener("install", (event) => {
  event.waitUntil(
    caches.open(SHELL_CACHE)
      .then((c) => c.addAll(SHELL_ASSETS).catch(() => {}))
      .then(() => self.skipWaiting())
  );
});

// ------------------------------------------------------------ activate -----
self.addEventListener("activate", (event) => {
  event.waitUntil(
    caches.keys().then((keys) => Promise.all(
      keys.filter((k) => k !== SHELL_CACHE && k !== TILE_CACHE)
          .map((k) => caches.delete(k))
    )).then(() => self.clients.claim())
  );
});

// ------------------------------------------------------------- fetch -------
self.addEventListener("fetch", (event) => {
  const req = event.request;
  if (req.method !== "GET") return;

  // 1) Tuiles : cache d'abord, sinon réseau puis mise en cache.
  if (isTile(req.url)) {
    event.respondWith(
      caches.open(TILE_CACHE).then((cache) =>
        cache.match(req).then((hit) => {
          if (hit) return hit;
          return fetch(req).then((resp) => {
            if (resp && (resp.ok || resp.type === "opaque")) {
              cache.put(req, resp.clone());
            }
            return resp;
          }).catch(() => hit); // hors-ligne et non caché : échec silencieux
        })
      )
    );
    return;
  }

  // 2) Ne pas interférer avec les WebSocket / l'API dynamique.
  const url = new URL(req.url);
  if (url.pathname.startsWith("/ws/") || url.pathname.startsWith("/api/")) return;

  // 3) App shell (page, /static/, polices) : RÉSEAU D'ABORD, cache en repli.
  //    Ainsi les mises à jour de map.js/style.css sont prises immédiatement
  //    quand on est en ligne, tout en restant consultable hors-ligne.
  const isShell = req.mode === "navigate"
    || url.pathname.startsWith("/static/")
    || url.hostname.endsWith("fonts.googleapis.com")
    || url.hostname.endsWith("fonts.gstatic.com");

  if (isShell) {
    event.respondWith(
      fetch(req).then((resp) => {
        if (resp && resp.ok) {
          const copy = resp.clone();
          caches.open(SHELL_CACHE).then((c) => c.put(req, copy));
        }
        return resp;
      }).catch(() => caches.match(req).then((hit) => hit || caches.match("/")))
    );
    return;
  }

  // 4) Reste : cache d'abord, repli réseau.
  event.respondWith(
    caches.match(req).then((hit) => hit || fetch(req).catch(() => caches.match("/")))
  );
});

// ---------------------------------------------------------- messages -------
self.addEventListener("message", (event) => {
  const msg = event.data || {};
  const reply = (data) => { if (event.ports[0]) event.ports[0].postMessage(data); };

  if (msg.type === "PREFETCH") {
    prefetchTiles(msg.urls || []).then((r) => reply(r));
  } else if (msg.type === "CACHE_INFO") {
    caches.open(TILE_CACHE)
      .then((c) => c.keys())
      .then((keys) => reply({ count: keys.length }));
  } else if (msg.type === "CLEAR_TILES") {
    caches.delete(TILE_CACHE).then(() => reply({ cleared: true }));
  }
});

async function prefetchTiles(urls) {
  const cache = await caches.open(TILE_CACHE);
  let done = 0, ok = 0, fail = 0;
  const total = urls.length;
  const CONC = 6;            // téléchargements parallèles
  let idx = 0;

  async function worker() {
    while (idx < urls.length) {
      const url = urls[idx++];
      try {
        const cached = await cache.match(url);
        if (!cached) {
          const resp = await fetch(url, { mode: "no-cors" });
          if (resp && (resp.ok || resp.type === "opaque")) {
            await cache.put(url, resp.clone());
          }
        }
        ok++;
      } catch (_) { fail++; }
      done++;
      if (done % 8 === 0 || done === total) broadcast(done, total);
    }
  }
  await Promise.all(Array.from({ length: CONC }, worker));
  broadcast(total, total);
  return { total, ok, fail };
}

async function broadcast(done, total) {
  const clients = await self.clients.matchAll();
  clients.forEach((c) => c.postMessage({ type: "PREFETCH_PROGRESS", done, total }));
}
