/* =========================================================================
   ADRASEC 06 · SDR MAP
   Real-time map of AIS / ADS-B / APRS / LRRP / radiosonde objects from SDR++ modules.
   ========================================================================= */
(function () {
  "use strict";

  const cfg = (id, def) => {
    const el = document.getElementById(id);
    try { return el ? JSON.parse(el.textContent) : def; } catch { return def; }
  };
  const WS_SCHEME = cfg("cfg-ws-scheme", "ws");
  const CENTER = [cfg("cfg-lat", 43.70), cfg("cfg-lon", 7.25)];
  const ZOOM = cfg("cfg-zoom", 9);

  const STATIC = document.querySelector('link[href*="leaflet.css"]')
    .getAttribute("href").replace("vendor/leaflet/leaflet.css", "");

  // --- Corrige les chemins d'icônes Leaflet vers nos images locales -------
  L.Icon.Default.prototype.options.imagePath = STATIC + "vendor/leaflet/images/";

  // ---------------------------------------------------------------- Carte --
  const map = L.map("map", {
    center: CENTER, zoom: ZOOM,
    zoomControl: false,          // recréé en bas à droite (voir plus bas)
    attributionControl: true,
  });

  // --- Fonds de carte (plusieurs types) -----------------------------------
  const OSM_ATTR = "&copy; OpenStreetMap";
  const baseLayers = {
    "Street (OSM)": L.tileLayer(
      "https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png",
      { maxZoom: 19, attribution: OSM_ATTR }),
    "Humanitarian (HOT)": L.tileLayer(
      "https://{s}.tile.openstreetmap.fr/hot/{z}/{x}/{y}.png",
      { maxZoom: 19, attribution: OSM_ATTR + " · HOT" }),
    "Topographic (OpenTopo)": L.tileLayer(
      "https://{s}.tile.opentopomap.org/{z}/{x}/{y}.png",
      { maxZoom: 17, attribution: OSM_ATTR + " · OpenTopoMap" }),
    "Satellite (Esri)": L.tileLayer(
      "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}",
      { maxZoom: 19, attribution: "Imagery &copy; Esri" }),
    "Light (Carto)": L.tileLayer(
      "https://{s}.basemaps.cartocdn.com/light_all/{z}/{x}/{y}{r}.png",
      { maxZoom: 20, subdomains: "abcd", attribution: OSM_ATTR + " · CARTO" }),
    "Dark (Carto)": L.tileLayer(
      "https://{s}.basemaps.cartocdn.com/dark_all/{z}/{x}/{y}{r}.png",
      { maxZoom: 20, subdomains: "abcd", attribution: OSM_ATTR + " · CARTO" }),
  };
  let activeBase = baseLayers["Dark (Carto)"];
  activeBase.addTo(map);

  // Names of basemaps that have a light/pale background. When one of them is
  // active we add `light-bg` on <body> so the CSS swaps marker colours to a
  // deeper, more saturated palette (kept-as-is on dark basemaps).
  const LIGHT_BG_NAMES = new Set([
    "Street (OSM)",
    "Humanitarian (HOT)",
    "Topographic (OpenTopo)",
    "Light (Carto)",
  ]);
  function applyBgPalette(name) {
    document.body.classList.toggle("light-bg", LIGHT_BG_NAMES.has(name));
  }
  applyBgPalette("Dark (Carto)");      // initial palette = dark
  map.on("baselayerchange", (e) => {
    activeBase = e.layer;
    applyBgPalette(e.name);
  });

  // Après un zoom, Leaflet peut régénérer les éléments d'icônes : on
  // re-applique l'orientation de chaque marqueur (et de ses ghosts à ±360°
  // de longitude) pour qu'aucun avion ne « retombe » sur un ancien cap.
  map.on("zoomend", () => {
    markers.forEach((m) => {
      if (m._sdrData) updateMarkerDom(m, m._sdrData);
      if (m._ghostL && m._sdrData) updateMarkerDom(m._ghostL, m._sdrData);
      if (m._ghostR && m._sdrData) updateMarkerDom(m._ghostR, m._sdrData);
    });
  });

  // Contrôles repositionnés en bas à droite pour ne pas être masqués par la
  // barre supérieure ni par le panneau de gauche.
  L.control.zoom({ position: "bottomright" }).addTo(map);
  // Basemap selector: collapsed by default so it shows as a small map icon
  // in the bottom-right corner and expands on hover/click. The previous
  // `collapsed: false` permanently expanded list was eating a noticeable
  // chunk of the map viewport with 6+ basemaps listed.
  L.control.layers(baseLayers, null, {
    position: "bottomright", collapsed: true,
  }).addTo(map);

  // --- Couches par type (filtrage) ----------------------------------------
  const layers = {
    "AIS": L.layerGroup().addTo(map),
    "ADSB": L.layerGroup().addTo(map),
    "APRS": L.layerGroup().addTo(map),
    "APRS Meteo": L.layerGroup().addTo(map),
    "lrrp": L.layerGroup().addTo(map),
    "radiosonde": L.layerGroup().addTo(map),
    "TETRA": L.layerGroup().addTo(map),
    "SARSAT": L.layerGroup().addTo(map),
    "satellite": L.layerGroup().addTo(map),
  };
  const markers = new Map();        // id -> L.marker
  const objectsById = new Map();    // id -> data
  const histories = new Map();      // id -> [[lat,lon], ...] (trajet)
  const trails = new Map();         // id -> L.polyline (trace)

  const CLASS = { "AIS": "t-AIS", "ADSB": "t-ADSB", "APRS": "t-APRS", "APRS Meteo": "t-METEO", "lrrp": "t-LRRP", "radiosonde": "t-SONDE", "TETRA": "t-TETRA", "SARSAT": "t-SARSAT", "satellite": "t-SAT" };
  // Libellé lisible affiché dans les popups (le champ "type" reste la clé).
  const TYPE_LABEL = { "lrrp": "LRRP", "radiosonde": "SONDE", "TETRA": "TETRA", "SARSAT": "SARSAT", "satellite": "SAT" };
  // Couleurs des traces (alignées sur le CSS par type).
  const TRAIL_COLOR = {
    "AIS": "#22d3ee", "ADSB": "#f6a609", "APRS": "#34d399",
    "APRS Meteo": "#a78bfa", "lrrp": "#f472b6", "radiosonde": "#facc15",
    "TETRA": "#fb923c", "SARSAT": "#dc2626", "satellite": "#818cf8",
  };
  const MAX_TRAIL = 200;            // nombre max de positions conservées par objet
  let showTrails = true;            // piloté par la case « Afficher les traces »

  // Family of aircraft SVGs for ADS-B markers. All face north (heading 0°)
  // by construction, so rotate(heading) gives the correct orientation.
  // The variant used at runtime is selected by adsbVariant(o) based on the
  // wake-vortex category emitted by the aircraft itself in the ADS-B
  // Aircraft Identification message (Type Code 1-4).
  //
  // Category mapping (ICAO Annex 10 Volume IV / DO-260B):
  //   A1 light (<7t)        -> light       (Cessna, PA-28, …)
  //   A2 small (7-34t)      -> bizjet
  //   A3-A5 large/heavy     -> airliner    (default, A320/B737/A380/B747)
  //   A6 high performance   -> fighter     (Rafale, F-16, military jets)
  //   A7 rotorcraft         -> helicopter
  //   B1 glider             -> glider
  //   B4 ultralight         -> light       (visually similar to GA prop)
  //   B6 UAV                -> drone
  //   anything else / none  -> airliner    (safe default, current behaviour)

  // Default airliner (also used when no category is transmitted, so no
  // behavioural regression for modules that don't emit `category` yet).
  const PLANE_SVG =
    '<svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    '<path d="M12 2 L13.2 9 L21 13 L21 15 L13.2 12.6 L13 19 L16 21 L16 22 ' +
    'L12 21 L8 22 L8 21 L11 19 L10.8 12.6 L3 15 L3 13 L10.8 9 Z"/></svg>';

  // Light GA / ultralight: straight wings, short fuselage, propeller line.
  const PLANE_LIGHT_SVG =
    '<svg viewBox="0 0 24 28" width="22" height="24" fill="currentColor" ' +
    'stroke="currentColor" xmlns="http://www.w3.org/2000/svg">' +
    '<line x1="9" y1="3" x2="15" y2="3" stroke-width="1.2"/>' +
    '<rect x="11" y="4" width="2" height="14"/>' +
    '<rect x="3" y="11" width="18" height="2.5"/>' +
    '<rect x="8" y="18" width="8" height="2"/>' +
    '<rect x="11" y="20" width="2" height="3"/></svg>';

  // Business jet: swept wings, narrower than airliner.
  const PLANE_BIZJET_SVG =
    '<svg viewBox="0 0 24 28" width="22" height="24" fill="currentColor" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    '<path d="M12 2 L11.2 7 L11.2 11 L4 15 L4 16.5 L11.2 14.5 L11.2 19 ' +
    'L9 21 L9 22 L11.2 21.5 L12 23 L12.8 21.5 L15 22 L15 21 L12.8 19 ' +
    'L12.8 14.5 L20 16.5 L20 15 L12.8 11 L12.8 7 Z"/></svg>';

  // Fighter jet: highly-swept delta wings, pointed nose, military feel.
  const PLANE_FIGHTER_SVG =
    '<svg viewBox="0 0 24 28" width="22" height="24" fill="currentColor" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    '<path d="M12 1 L10.5 6 L10.5 11 L3 19 L4 21 L10.5 17.5 L10.5 22 ' +
    'L8.5 25 L8.5 25.5 L10.5 25 L12 26 L13.5 25 L15.5 25.5 L15.5 25 ' +
    'L13.5 22 L13.5 17.5 L20 21 L21 19 L13.5 11 L13.5 6 Z"/></svg>';

  // Helicopter: oval fuselage, faint disc for the main rotor, tail boom.
  const PLANE_HELI_SVG =
    '<svg viewBox="0 0 24 28" width="22" height="24" fill="currentColor" ' +
    'stroke="currentColor" xmlns="http://www.w3.org/2000/svg">' +
    '<ellipse cx="12" cy="14" rx="3.5" ry="6"/>' +
    '<circle cx="12" cy="14" r="11" fill="none" stroke-width="1.2" ' +
    'stroke-opacity="0.5"/>' +
    '<rect x="11.4" y="19" width="1.2" height="6"/>' +
    '<rect x="9.5" y="24.5" width="5" height="1.2"/></svg>';

  // Drone: quadcopter top-down view, four rotors at corners.
  const PLANE_DRONE_SVG =
    '<svg viewBox="0 0 24 28" width="22" height="24" fill="currentColor" ' +
    'stroke="currentColor" xmlns="http://www.w3.org/2000/svg">' +
    '<rect x="9" y="11" width="6" height="6"/>' +
    '<line x1="6" y1="8" x2="9" y2="11" stroke-width="1.5"/>' +
    '<line x1="18" y1="8" x2="15" y2="11" stroke-width="1.5"/>' +
    '<line x1="6" y1="20" x2="9" y2="17" stroke-width="1.5"/>' +
    '<line x1="18" y1="20" x2="15" y2="17" stroke-width="1.5"/>' +
    '<circle cx="5" cy="7" r="2.5" fill="none" stroke-width="1.3"/>' +
    '<circle cx="19" cy="7" r="2.5" fill="none" stroke-width="1.3"/>' +
    '<circle cx="5" cy="21" r="2.5" fill="none" stroke-width="1.3"/>' +
    '<circle cx="19" cy="21" r="2.5" fill="none" stroke-width="1.3"/></svg>';

  // Glider / sailplane: very long thin wings, no engine.
  const PLANE_GLIDER_SVG =
    '<svg viewBox="0 0 24 28" width="22" height="24" fill="currentColor" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    '<rect x="11.3" y="4" width="1.4" height="16"/>' +
    '<rect x="1" y="13" width="22" height="1.5"/>' +
    '<rect x="8" y="19" width="8" height="1.4"/>' +
    '<rect x="11.3" y="20.5" width="1.4" height="2.5"/></svg>';

  // Map a wake-vortex category code (string like "A1", "A7", "B6") to the
  // SVG and a CSS class. Tolerant: accepts upper/lower case, optional
  // separator, and a few aliases ("HELI", "DRONE", …) some module
  // implementations may use instead of the raw ICAO code.
  function adsbVariant(o) {
    const raw = (o.adsb_category || "").toString().toUpperCase().trim();
    switch (raw) {
      case "A1": case "LIGHT":
      case "B4": case "ULTRALIGHT": case "ULM":
        return { svg: PLANE_LIGHT_SVG,   cls: "plane plane-light" };
      case "A2": case "BIZJET":
        return { svg: PLANE_BIZJET_SVG,  cls: "plane plane-bizjet" };
      case "A6": case "FIGHTER": case "MILITARY":
        return { svg: PLANE_FIGHTER_SVG, cls: "plane plane-fighter" };
      case "A7": case "ROTORCRAFT": case "HELI": case "HELICOPTER":
        return { svg: PLANE_HELI_SVG,    cls: "plane plane-heli" };
      case "B1": case "GLIDER":
        return { svg: PLANE_GLIDER_SVG,  cls: "plane plane-glider" };
      case "B6": case "UAV": case "DRONE":
        return { svg: PLANE_DRONE_SVG,   cls: "plane plane-drone" };
      // A3/A4/A5 — large/heavy airliners — and any unknown/missing
      // category fall back to the classic airliner silhouette.
      default:
        return { svg: PLANE_SVG,         cls: "plane" };
    }
  }

  // Decode an ICAO Doc 8643 class designator (3 chars) into a tooltip.
  // Format: <vehicle><engines><engine-type>
  //   vehicle:     L=Land  S=Sea  A=Amphibian  G=Glider
  //                H=Helicopter  T=Tilt-rotor  W=Weight-shift
  // After the digit:  P=Piston  T=Turboprop  J=Jet
  //                   E=Electric  R=Rotor (helicopters)
  // Returns "" if the class can't be decoded (and the popup falls back
  // to just showing the raw code).
  function decodeIcaoClass(cls) {
    if (!cls || cls.length < 3) return "";
    const vehicle = {
      L: "Land plane", S: "Seaplane", A: "Amphibian", G: "Glider",
      H: "Helicopter", T: "Tilt-rotor", W: "Weight-shift",
    }[cls[0].toUpperCase()];
    const engines = cls[1];
    const engType = {
      P: "piston", T: "turboprop", J: "jet",
      E: "electric", R: "rotor",
    }[cls[2].toUpperCase()];
    if (!vehicle || !engType || !/[0-9]/.test(engines)) return "";
    const e = engines === "1" ? "engine" : "engines";
    return `${vehicle}, ${engines} ${engType} ${e}`;
  }

  // SVG de bateau vu de dessus, PROUE VERS LE HAUT par construction.
  // Mêmes propriétés que l'avion : indépendant de la police, rotation = COG.
  const SHIP_SVG =
    '<svg viewBox="0 0 24 24" width="20" height="20" fill="currentColor" ' +
    'stroke="currentColor" stroke-linejoin="round" stroke-width="0.5" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    '<path d="M12 2 L16 9 L17 17 Q17 21 12 21 Q7 21 7 17 L8 9 Z"/>' +
    '<rect x="10.5" y="11.5" width="3" height="4.5" fill="rgba(7,11,18,0.85)" stroke="none"/>' +
    '</svg>';

  // AIS sub-category SVGs. Each is monochrome (fill="currentColor") so the
  // CSS color of the marker drives it. AtoN / coast station / SAR / distress
  // markers don't rotate (no meaningful heading for a buoy or a beacon).
  //
  // AtoN: diamond (IALA buoy-like) with inner dot.
  const ATON_SVG =
    '<svg viewBox="0 0 24 24" width="18" height="18" fill="currentColor" ' +
    'stroke="currentColor" stroke-width="0.5" xmlns="http://www.w3.org/2000/svg">' +
    '<path d="M12 2 L22 12 L12 22 L2 12 Z"/>' +
    '<circle cx="12" cy="12" r="2.2" fill="rgba(7,11,18,0.85)" stroke="none"/>' +
    '</svg>';

  // SAR aircraft (MMSI 111…): plane silhouette with a red cross overlay.
  const SAR_AC_SVG =
    '<svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    '<path d="M12 2 L13.2 9 L21 13 L21 15 L13.2 12.6 L13 19 L16 21 L16 22 ' +
    'L12 21 L8 22 L8 21 L11 19 L10.8 12.6 L3 15 L3 13 L10.8 9 Z"/>' +
    '<rect x="10.8" y="6" width="2.4" height="6" fill="#ff4d6d"/>' +
    '<rect x="9" y="7.8" width="6" height="2.4" fill="#ff4d6d"/>' +
    '</svg>';

  // Distress beacons (SART / EPIRB / MOB): pulsing red triangle with "!".
  const DISTRESS_SVG =
    '<svg viewBox="0 0 24 24" width="20" height="20" fill="currentColor" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    '<path d="M12 2 L22 21 L2 21 Z"/>' +
    '<rect x="11" y="9" width="2" height="6" fill="rgba(7,11,18,0.9)"/>' +
    '<rect x="11" y="16.5" width="2" height="2" fill="rgba(7,11,18,0.9)"/>' +
    '</svg>';

  // Coast station: stylised antenna mast.
  const COAST_SVG =
    '<svg viewBox="0 0 24 24" width="20" height="20" fill="none" ' +
    'stroke="currentColor" stroke-width="2" stroke-linecap="round" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    '<path d="M12 22 L12 8"/>' +
    '<path d="M6 22 L12 4 L18 22"/>' +
    '<circle cx="12" cy="4" r="1.5" fill="currentColor"/>' +
    '</svg>';

  // Weather balloon (radiosonde): clearly recognisable balloon — inflated
  // sphere on top, a small pinched neck, a tether, then the sonde payload
  // hanging below.
  const SONDE_SVG =
    '<svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    '<ellipse cx="12" cy="7.5" rx="6.5" ry="7"/>' +
    '<path d="M10.5 13.8 L12 16 L13.5 13.8 Z"/>' +
    '<line x1="12" y1="16" x2="12" y2="20" stroke="currentColor" stroke-width="1"/>' +
    '<rect x="9.8" y="19.8" width="4.4" height="3.2" rx="0.4"/>' +
    '</svg>';

  // TETRA terminal (handheld radio): antenna at top, body with speaker grille,
  // LCD strip and 3 keypad dots. Antenna up at 0° -> rotate(heading) gives
  // the direction the user is moving in.
  const TETRA_SVG =
    '<svg viewBox="0 0 24 24" width="20" height="22" fill="currentColor" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    '<rect x="11" y="2" width="2" height="6" rx="0.5"/>' +
    '<rect x="6" y="8" width="12" height="14" rx="1.5"/>' +
    '<rect x="8.5" y="10" width="7" height="1.2" fill="rgba(7,11,18,0.85)"/>' +
    '<rect x="8.5" y="12" width="7" height="1.2" fill="rgba(7,11,18,0.85)"/>' +
    '<rect x="8.5" y="14.5" width="7" height="3" fill="rgba(7,11,18,0.85)"/>' +
    '<circle cx="9.5" cy="19.5" r="0.8" fill="rgba(7,11,18,0.85)"/>' +
    '<circle cx="12" cy="19.5" r="0.8" fill="rgba(7,11,18,0.85)"/>' +
    '<circle cx="14.5" cy="19.5" r="0.8" fill="rgba(7,11,18,0.85)"/>' +
    '</svg>';

  // Cospas-Sarsat 406 MHz distress beacon: circle (filled with currentColor)
  // crossed by 8 short radio-wave dashes (N, S, E, W + diagonals), with a
  // bright white dot in the centre. Designed to read as "SOS / radio source"
  // at a glance. Real beacons get a pulsing red; test transmissions get a
  // steady amber (handled in CSS).
  const SARSAT_SVG =
    '<svg viewBox="0 0 32 32" width="22" height="22" fill="currentColor" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    '<circle cx="16" cy="16" r="13" fill="currentColor" ' +
    'stroke="rgba(255,255,255,0.7)" stroke-width="1.5"/>' +
    '<path d="M16 9 v3 M16 20 v3 M9 16 h3 M20 16 h3 ' +
            'M11 11 l2 2 M21 21 l-2 -2 M11 21 l2 -2 M21 11 l-2 2" ' +
            'stroke="rgba(255,255,255,0.95)" stroke-width="2" ' +
            'stroke-linecap="round" fill="none"/>' +
    '<circle cx="16" cy="16" r="2.5" fill="rgba(255,255,255,0.95)"/>' +
    '</svg>';

  // Satellite (orbital tracker): top-down view, central body with antenna +
  // two solar panels with their characteristic grid pattern. Doesn't rotate
  // (the sub-satellite point on the ground moves along the ground track,
  // there's no meaningful "heading" for a top-down satellite icon).
  const SAT_SVG =
    '<svg viewBox="0 0 24 24" width="24" height="22" fill="currentColor" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    // Left solar panel with grid lines
    '<rect x="1" y="9" width="7" height="6" rx="0.3"/>' +
    '<line x1="3.3" y1="9" x2="3.3" y2="15" stroke="rgba(7,11,18,0.85)" stroke-width="0.5"/>' +
    '<line x1="5.6" y1="9" x2="5.6" y2="15" stroke="rgba(7,11,18,0.85)" stroke-width="0.5"/>' +
    '<line x1="1" y1="12" x2="8" y2="12" stroke="rgba(7,11,18,0.85)" stroke-width="0.5"/>' +
    // Central body
    '<rect x="9" y="7" width="6" height="10" rx="0.8"/>' +
    // Antenna on top
    '<rect x="11.5" y="4" width="1" height="3"/>' +
    '<circle cx="12" cy="3.5" r="0.8" fill="rgba(7,11,18,0.85)" ' +
    'stroke="currentColor" stroke-width="0.4"/>' +
    // Right solar panel with grid lines
    '<rect x="16" y="9" width="7" height="6" rx="0.3"/>' +
    '<line x1="18.3" y1="9" x2="18.3" y2="15" stroke="rgba(7,11,18,0.85)" stroke-width="0.5"/>' +
    '<line x1="20.6" y1="9" x2="20.6" y2="15" stroke="rgba(7,11,18,0.85)" stroke-width="0.5"/>' +
    '<line x1="16" y1="12" x2="23" y2="12" stroke="rgba(7,11,18,0.85)" stroke-width="0.5"/>' +
    '</svg>';

  // Glyphes pour les types NON orientés (pas de cap).
  const GLYPH = {
    "APRS": "✚", "APRS Meteo": "☂", "lrrp": "⌖",
  };

  // Pick the SVG and CSS class for an AIS object based on its MMSI category.
  // Falls back to the regular ship icon when the kind is unknown.
  function aisVariant(o) {
    switch (o.mmsi_kind) {
      case "aton":           return { svg: ATON_SVG,     cls: "ais-aton",    rotate: false };
      case "sar_aircraft":   return { svg: SAR_AC_SVG,   cls: "ais-sar",     rotate: true  };
      case "sart":
      case "epirb":
      case "mob":            return { svg: DISTRESS_SVG, cls: "ais-distress",rotate: false };
      case "coast_station":  return { svg: COAST_SVG,    cls: "ais-coast",   rotate: false };
      case "group":          return { svg: SHIP_SVG,     cls: "ais-group",   rotate: true  };
      case "aux_craft":      return { svg: SHIP_SVG,     cls: "ais-aux",     rotate: true  };
      case "ship":
      default:               return { svg: SHIP_SVG,     cls: "ship",        rotate: true  };
    }
  }

  function iconRotation(o) {
    // Aircraft: oriented by heading (SVG points north at 0°).
    if (o.type === "ADSB") return (o.heading != null ? o.heading : 0);
    // AIS: only rotate variants that represent something with a course
    // (ships, group, auxiliary, SAR aircraft). Static infrastructure (AtoN,
    // distress beacons, coast stations) is never rotated.
    if (o.type === "AIS" && o.heading != null && aisVariant(o).rotate) return o.heading;
    // TETRA terminals: LIP messages carry a direction (dir=…deg). When
    // present, rotate the handheld so the antenna points the way the user
    // is moving.
    if (o.type === "TETRA" && o.heading != null) return o.heading;
    return null;
  }

  function glyphHtml(o, rotDeg) {
    const rot = rotDeg != null ? `transform:rotate(${rotDeg}deg);` : "";
    if (o.type === "ADSB") {
      const v = adsbVariant(o);
      return `<span class="glyph ${v.cls}" style="${rot}">${v.svg}</span>`;
    }
    if (o.type === "AIS") {
      const v = aisVariant(o);
      return `<span class="glyph ${v.cls}" style="${rot}">${v.svg}</span>`;
    }
    if (o.type === "radiosonde") {
      // Radiosondes drift with the wind; no meaningful "heading" to rotate to.
      return `<span class="glyph sonde">${SONDE_SVG}</span>`;
    }
    if (o.type === "TETRA") {
      return `<span class="glyph tetra" style="${rot}">${TETRA_SVG}</span>`;
    }
    if (o.type === "SARSAT") {
      // Distress beacons don't move (or barely do): no rotation. Real
      // beacons pulse, test beacons stay steady — handled by the
      // `sarsat-test` class added to the parent .sdr-marker by makeIcon.
      return `<span class="glyph sarsat">${SARSAT_SVG}</span>`;
    }
    if (o.type === "satellite") {
      // Top-down satellite: no rotation. If the satellite is below the
      // horizon for the local observer (el < 0), the marker is dimmed via
      // the `below-horizon` class added by makeIcon — still on the map
      // (sub-satellite point is meaningful even when not visible from the
      // QTH), but visually softer.
      return `<span class="glyph satellite">${SAT_SVG}</span>`;
    }
    return `<span class="glyph" style="${rot}">${GLYPH[o.type] || "•"}</span>`;
  }

  function makeIcon(o) {
    const r = iconRotation(o);
    const label = o.name || o.ident || "";
    // For SARSAT beacons in test mode, append a class so the CSS swaps to
    // amber and disables the distress pulse animation. Real distress
    // beacons keep the default red colour and pulse.
    // For satellites below the local horizon (el<0), dim the icon so the
    // operator instantly sees the bird is not in view from the QTH.
    let extraCls = "";
    if (o.type === "SARSAT" && o.sarsat_test) extraCls += " sarsat-test";
    if (o.type === "satellite" && typeof o.sat_el === "number" && o.sat_el < 0) {
      extraCls += " below-horizon";
    }
    return L.divIcon({
      className: "",
      iconSize: [24, 24], iconAnchor: [12, 12],
      html: `<div class="sdr-marker ${CLASS[o.type]}${extraCls}">
               ${glyphHtml(o, r)}
               <span class="label">${escapeHtml(label)}</span>
             </div>`,
    });
  }

  function escapeHtml(s) {
    return String(s).replace(/[&<>"']/g, (c) => (
      { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
  }

  function row(k, v) {
    return (v === null || v === undefined || v === "") ? "" :
      `<span class="k">${k}</span><span class="v">${escapeHtml(v)}</span>`;
  }

  // Country flag for a 2-letter ISO code. Returns an inline <img> pointing at
  // a local SVG (works offline once cached by the service worker), with an
  // emoji fallback rendered via onerror in case the SVG is missing. Returns
  // empty string when the ISO code is not provided.
  function flagHtml(iso) {
    if (!iso) return "";
    const code = String(iso).toLowerCase();
    // Emoji flag = pair of regional-indicator letters (e.g. F + R -> 🇫🇷).
    // Used as text fallback if the SVG fails to load.
    const emoji = String.fromCodePoint(
      ...iso.toUpperCase().split("").map((c) => 0x1F1E6 + (c.charCodeAt(0) - 65))
    );
    const safeEmoji = escapeHtml(emoji);
    return (
      `<img class="flag" src="${STATIC}flags/${code}.svg" ` +
      `alt="${escapeHtml(iso)}" title="${escapeHtml(iso)}" ` +
      `onerror="this.outerHTML='<span class=&quot;flag flag-emoji&quot;>${safeEmoji}</span>'">`
    );
  }

  function popupHtml(o) {
    const tag = `<span class="popup-tag ${CLASS[o.type]}">${TYPE_LABEL[o.type] || o.type}</span>`;
    let grid = "";
    grid += row("Lat", o.lat.toFixed(5));
    grid += row("Lon", o.lon.toFixed(5));
    if (o.altitude_ft != null) grid += row("Altitude", o.altitude_ft + " ft");
    if (o.heading != null) grid += row("Heading", Math.round(o.heading) + "°");
    if (o.mmsi) {
      grid += row("MMSI", o.mmsi);
      if (o.mmsi_label && o.mmsi_kind && o.mmsi_kind !== "ship") {
        grid += row("AIS type", o.mmsi_label);
      }
    }
    // Country + flag: shared between AIS (derived from MMSI MID) and SARSAT
    // (parsed from the beacon's country field). The flagHtml helper falls
    // back to an emoji if the SVG can't be found.
    if (o.country_name || o.country_iso) {
      const flag = flagHtml(o.country_iso);
      const cc = o.country_iso ? ` (${o.country_iso})` : "";
      const txt = `${flag}${o.country_name || o.country_iso}${cc}`;
      grid += `<span class="k">Flag</span><span class="v">${txt}</span>`;
    } else if (o.mid) {
      grid += row("MID", o.mid);
    }
    if (o.icao) grid += row("ICAO", o.icao);
    // Phase B aircraft database enrichment (from local Mictronics lookup).
    // Surface registration, type code and a "Military" badge when known.
    if (o.type === "ADSB" && o.aircraft_reg) {
      grid += row("Registration", o.aircraft_reg);
    }
    // Country of registration: drawn from the ICAO 24-bit allocation table
    // (always available, doesn't need the SQLite DB).
    if (o.type === "ADSB" && o.aircraft_country_iso) {
      const flag = flagHtml(o.aircraft_country_iso);
      const cc = ` (${o.aircraft_country_iso})`;
      const txt = `${flag}${o.aircraft_country_name || o.aircraft_country_iso}${cc}`;
      grid += `<span class="k">Reg. country</span><span class="v">${txt}</span>`;
    }
    // Airline operator from the callsign prefix (e.g. AFR -> Air France).
    if (o.type === "ADSB" && o.aircraft_operator) {
      let opTxt = o.aircraft_operator;
      if (o.aircraft_operator_country) {
        opTxt += ` — ${o.aircraft_operator_country}`;
      }
      grid += row("Operator", opTxt);
    }
    // Aircraft model: prefer the long human-readable description from
    // types.json over the bare type code. Fallback to just the code.
    if (o.type === "ADSB" && o.aircraft_type_desc) {
      // Capitalisation in types.json is upper-case; titlecase looks nicer.
      const titled = o.aircraft_type_desc.replace(/\b\w[\w-]*/g, (w) =>
        w[0] + w.slice(1).toLowerCase()
      );
      grid += row("Model", `${titled} (${o.aircraft_type || "?"})`);
    } else if (o.type === "ADSB" && o.aircraft_type) {
      grid += row("Aircraft type", o.aircraft_type);
    }
    // Optional ICAO class designator (L2J = Land/2-engine/Jet, H2T =
    // Helicopter/2-turbine, etc.). Decoded into a tooltip for non-techies.
    if (o.type === "ADSB" && o.aircraft_icao_class) {
      const cls = o.aircraft_icao_class;
      const decoded = decodeIcaoClass(cls);
      const tip = decoded ? ` title="${decoded}"` : "";
      grid += `<span class="k">ICAO class</span>` +
              `<span class="v"${tip}><code>${cls}</code></span>`;
    }
    if (o.type === "ADSB" && o.aircraft_military) {
      // The "v" cell HTML is built ourselves so we can colourise the badge.
      grid += `<span class="k">Operator</span><span class="v">` +
              `<span class="badge-military">⚔ Military</span></span>`;
    }
    // ADS-B wake-vortex category, shown in human-readable form. The raw
    // code (A1, A7, B6, …) is also kept in parens for technical users.
    if (o.type === "ADSB" && o.adsb_category) {
      const human = {
        "A1": "Light",     "A2": "Small",        "A3": "Large",
        "A4": "High-vortex large",                "A5": "Heavy",
        "A6": "High performance / military",
        "A7": "Rotorcraft",
        "B1": "Glider",    "B2": "Lighter-than-air",
        "B3": "Parachutist","B4": "Ultralight",
        "B6": "UAV (drone)","B7": "Space vehicle",
        "C1": "Surface emergency",
        "C2": "Surface service",
        "C3": "Fixed obstruction",
      }[o.adsb_category.toUpperCase()] || o.adsb_category;
      grid += row("Category", `${human} (${o.adsb_category})`);
    }
    if (o.ssi)  grid += row("SSI",  o.ssi);
    if (o.source) grid += row("RID DMR", o.source);
    // TETRA LIP carries GPS accuracy in metres ("±20 m"): we surface it
    // so the operator can judge how trustworthy the position is.
    if (o.type === "TETRA" && o.gps_acc_m != null) {
      grid += row("GPS accuracy", "±" + Math.round(o.gps_acc_m) + " m");
    }
    // Cospas-Sarsat beacon details. We've already parsed the `info` field
    // on the server side, so render its components as proper rows here.
    if (o.type === "SARSAT") {
      grid += row("Hex ID", o.name);
      if (o.sarsat_beacon) {
        const t = o.sarsat_test ? " (TEST)" : "";
        grid += row("Beacon", o.sarsat_beacon + t);
      }
      if (o.sarsat_protocol)  grid += row("Protocol", o.sarsat_protocol);
      if (o.sarsat_aircraft)  grid += row("Aircraft", o.sarsat_aircraft);
      if (o.sarsat_callsign)  grid += row("Callsign", o.sarsat_callsign);
      if (o.sarsat_serial)    grid += row("Serial",   o.sarsat_serial);
      if (o.sarsat_operator)  grid += row("Operator", o.sarsat_operator);
      if (o.sarsat_src) {
        grid += row("Position src",
          o.sarsat_src === "external" ? "External GPS" : "Internal");
      }
      if (o.sarsat_homing121) {
        grid += row("121.5 MHz", o.sarsat_homing121 === "yes" ? "✓ homing" : "—");
      }
      if (o.sarsat_bch1 || o.sarsat_bch2) {
        const fmt = (b) => b === "ok" ? "✓" : (b === "err" ? "✗" : (b || "—"));
        grid += row("BCH", fmt(o.sarsat_bch1) + " / " + fmt(o.sarsat_bch2));
      }
    }
    // Satellite tracker: look angles + Doppler + footprint. Sub-satellite
    // point is already shown as Lat/Lon at the top of the popup.
    if (o.type === "satellite") {
      if (o.sat_norad != null) grid += row("NORAD", o.sat_norad);
      if (o.sat_alt_km != null) grid += row("Altitude", o.sat_alt_km + " km");
      if (o.sat_az != null) {
        // Compass-style hint next to the bearing for quick orientation.
        const dirs = ["N","NE","E","SE","S","SW","W","NW","N"];
        const compass = dirs[Math.round(((o.sat_az % 360) + 360) % 360 / 45)];
        grid += row("Azimuth", `${o.sat_az.toFixed(1)}° (${compass})`);
      }
      if (o.sat_el != null) {
        const below = o.sat_el < 0 ? " (below horizon)" : "";
        grid += row("Elevation", `${o.sat_el.toFixed(1)}°${below}`);
      }
      if (o.sat_range_km != null) grid += row("Range", o.sat_range_km + " km");
      if (o.sat_doppler_hz != null) {
        // Above 1 kHz, show kHz with 2 decimals; otherwise raw Hz. Always
        // signed so + means approaching, − means receding.
        const d = o.sat_doppler_hz;
        const txt = Math.abs(d) >= 1000
          ? `${(d / 1000).toFixed(2)} kHz`
          : `${d} Hz`;
        const sign = d > 0 ? "+" : "";
        grid += row("Doppler", sign + txt);
      }
      if (o.sat_footprint_km != null) {
        grid += row("Footprint", o.sat_footprint_km + " km Ø");
      }
    }
    if (o.type === "radiosonde") {
      if (o.altitude_m != null) grid += row("Altitude", Math.round(o.altitude_m) + " m");
      if (o.climb_rate != null) {
        const cr = o.climb_rate;
        const dir = cr >= 0 ? "↑ climbing" : "↓ descending";
        grid += row("Vert.", `${Math.abs(cr).toFixed(1)} m/s ${dir}`);
      }
      if (o.speed != null) grid += row("Speed", o.speed.toFixed(1) + " m/s");
    } else if (o.type === "TETRA") {
      if (o.speed != null) grid += row("Speed", o.speed + " m/s");
    } else if (o.speed != null) {
      grid += row("Speed", o.speed + " kn");
    }
    if (o.date || o.time) grid += row("UTC", (o.date + " " + o.time).trim());
    const w = o.weather || {};
    if (w.temp_c != null) grid += row("Temp.", w.temp_c + " °C");
    if (w.humidity != null) grid += row("Humidity", w.humidity + " %");
    if (w.wind_speed != null) grid += row("Wind", w.wind_speed + " km/h");
    if (w.wind_gust != null) grid += row("Gust", w.wind_gust + " km/h");
    if (w.wind_dir != null) grid += row("Wind dir.", Math.round(w.wind_dir) + "°");
    if (w.pressure_hpa != null) grid += row("Pressure", w.pressure_hpa + " hPa");
    if (w.rain_mm != null) grid += row("Rain", w.rain_mm + " mm");
    // For SARSAT, satellite and ADS-B we already parsed `info` into
    // structured rows above (Altitude, Heading, Category, etc.), so the
    // raw "key=value;..." dump becomes a useless duplicate at the bottom
    // of the popup — hide it for these types.
    const showRawInfo = o.info
      && o.type !== "SARSAT"
      && o.type !== "satellite"
      && o.type !== "ADSB";
    const info = showRawInfo ? `<div class="popup-info">${escapeHtml(o.info)}</div>` : "";
    const ds = `data-type="${escapeHtml(o.type)}" data-ident="${escapeHtml(o.ident)}" data-id="${o.id}"`;
    const actions = `<div class="popup-actions">
        <button class="popup-btn" data-act="replay" ${ds}>▷ Replay</button>
        <button class="popup-btn" data-act="gpx" ${ds}>⤓ GPX</button>
      </div>`;
    return `<div class="popup-title">${escapeHtml(o.name || o.ident)} ${tag}</div>
            <div class="popup-grid">${grid}</div>${info}${actions}`;
  }

  // Met à jour l'apparence d'un marqueur déjà affiché sans le recréer :
  // rotation du glyphe (cap) et libellé. Si l'élément DOM n'est pas encore
  // monté (marqueur hors écran), on régénère l'icône en repli.
  function updateMarkerDom(m, o) {
    const el = m.getElement();
    if (!el) { m.setIcon(makeIcon(o)); return; }
    const glyph = el.querySelector(".glyph");
    const label = el.querySelector(".label");
    if (glyph) {
      const r = iconRotation(o);
      // Rotation appliquée immédiatement (la transition est désactivée en CSS,
      // donc pas d'effet « retard d'une position » au changement de cap).
      const next = r != null ? `rotate(${r}deg)` : "";
      if (glyph.style.transform !== next) glyph.style.transform = next;
    }
    if (label) {
      const txt = o.name || o.ident || "";
      if (label.textContent !== txt) label.textContent = txt;
    }
  }

  function upsert(o) {
    if (!(o.type in layers)) return;
    objectsById.set(o.id, o);
    let m = markers.get(o.id);
    if (m) {
      // Mise à jour « vivante » : on déplace le marqueur principal ET ses
      // deux ghosts (copies adjacentes à ±360° de longitude), ainsi le
      // marqueur reste visible peu importe la "copie" du monde affichée
      // par l'utilisateur, et un objet qui traverse l'antiméridien apparaît
      // immédiatement de l'autre côté de la carte au lieu de "disparaître".
      m.setLatLng([o.lat, o.lon]);
      if (m._ghostL) m._ghostL.setLatLng([o.lat, o.lon - 360]);
      if (m._ghostR) m._ghostR.setLatLng([o.lat, o.lon + 360]);
      m._sdrData = o;
      updateMarkerDom(m, o);
      if (m._ghostL) updateMarkerDom(m._ghostL, o);
      if (m._ghostR) updateMarkerDom(m._ghostR, o);
      // Re-applique la rotation au tick suivant : si Leaflet a (re)généré
      // l'élément pendant ce cycle (zoom/pan), le transform est repositionné.
      requestAnimationFrame(() => {
        updateMarkerDom(m, o);
        if (m._ghostL) updateMarkerDom(m._ghostL, o);
        if (m._ghostR) updateMarkerDom(m._ghostR, o);
      });
      if (m.getPopup()) m.setPopupContent(popupHtml(o));
      if (m._ghostL && m._ghostL.getPopup()) m._ghostL.setPopupContent(popupHtml(o));
      if (m._ghostR && m._ghostR.getPopup()) m._ghostR.setPopupContent(popupHtml(o));
    } else {
      m = L.marker([o.lat, o.lon], {
        icon: makeIcon(o),
        // Transition CSS douce du déplacement (voir .leaflet-marker-icon).
        riseOnHover: true,
      }).bindPopup(popupHtml(o));
      // Ghost copies at ±360° longitude: visually duplicate the marker on
      // the world copies left and right of the main one. Without them, an
      // object near the antimeridian (Bering Strait / Kamchatka-Alaska line)
      // would appear to vanish when its longitude wraps from +179° to -179°
      // because the marker stays at its literal coordinate, which falls
      // outside the user's current viewport. With them, the satellite
      // entering the antimeridian from the west keeps being rendered on the
      // east side too (and vice-versa) — it visually "wraps around".
      const ghostL = L.marker([o.lat, o.lon - 360], {
        icon: makeIcon(o), riseOnHover: true,
      }).bindPopup(popupHtml(o));
      const ghostR = L.marker([o.lat, o.lon + 360], {
        icon: makeIcon(o), riseOnHover: true,
      }).bindPopup(popupHtml(o));
      m._ghostL = ghostL;
      m._ghostR = ghostR;
      m._sdrType = o.type;
      m._sdrData = o;
      ghostL._sdrType = o.type;
      ghostR._sdrType = o.type;
      ghostL._sdrData = o;
      ghostR._sdrData = o;
      markers.set(o.id, m);
      layers[o.type].addLayer(m);
      layers[o.type].addLayer(ghostL);
      layers[o.type].addLayer(ghostR);
    }
    // --- Historique des positions + trace de déplacement ---
    let h = histories.get(o.id);
    if (!h) {
      // Premier passage : amorcer depuis l'historique serveur si fourni
      // (positions récentes) → la trace survit au rechargement de page.
      h = (Array.isArray(o.track) && o.track.length)
        ? o.track.map((p) => [p[0], p[1]]) : [];
      if (h.length > MAX_TRAIL) h = h.slice(h.length - MAX_TRAIL);
      histories.set(o.id, h);
    }
    const last = h[h.length - 1];
    if (!last || last[0] !== o.lat || last[1] !== o.lon) {
      h.push([o.lat, o.lon]);
      if (h.length > MAX_TRAIL) h.shift();
    }
    if (showTrails) rebuildTrail(o);

    refreshCounts();
  }

  // Trace = polyligne reliant les positions successives ; l'icône reste sur la
  // dernière (point B). La trace est dans la couche du type → masquée par les
  // filtres en même temps que le marqueur.
  // Split a list of [lat, lon] points into multiple segments wherever two
  // consecutive points jump by more than 180° in longitude (i.e. the object
  // crossed the antimeridian, ±180°). Without this, Leaflet would draw a
  // single straight line going all the way across the map — visually wrong
  // and obscuring half the basemap.
  // Returns an array of segments, suitable for L.polyline(multi) which
  // natively renders disjoint pieces in one polyline object.
  function splitAntimeridian(points) {
    if (!points || points.length < 2) return [points || []];
    const segments = [];
    let current = [points[0]];
    for (let i = 1; i < points.length; i++) {
      const [lat0, lon0] = points[i - 1];
      const [lat1, lon1] = points[i];
      if (Math.abs(lon1 - lon0) > 180) {
        // Jump detected: close the current segment, open a new one.
        if (current.length >= 2) segments.push(current);
        current = [points[i]];
      } else {
        current.push(points[i]);
      }
    }
    if (current.length >= 2) segments.push(current);
    return segments;
  }

  // Shift every point of a polyline path by a longitude offset (±360°).
  // Used to draw "ghost" copies of a trail on adjacent world copies so the
  // trace stays visually continuous around the antimeridian.
  function shiftLon(points, delta) {
    return points.map(([lat, lon]) => [lat, lon + delta]);
  }

  // Build / refresh the polyline trace of an object. Multi-segment shape is
  // used so antimeridian crossings render as two disjoint pieces rather than
  // one straight line slashing the whole map. Two ghost polylines mirror
  // the trace on adjacent world copies (same logic as the marker ghosts),
  // so the trace remains visible wherever the user is looking.
  function rebuildTrail(o) {
    const h = histories.get(o.id);
    let t = trails.get(o.id);
    if (!h || h.length < 2) {                 // pas (encore) de trajet
      if (t) {
        layers[o.type].removeLayer(t);
        if (t._ghostL) layers[o.type].removeLayer(t._ghostL);
        if (t._ghostR) layers[o.type].removeLayer(t._ghostR);
        trails.delete(o.id);
      }
      return;
    }
    const segments  = splitAntimeridian(h);
    const segmentsL = splitAntimeridian(shiftLon(h, -360));
    const segmentsR = splitAntimeridian(shiftLon(h, +360));
    if (t) {
      t.setLatLngs(segments);
      if (t._ghostL) t._ghostL.setLatLngs(segmentsL);
      if (t._ghostR) t._ghostR.setLatLngs(segmentsR);
    } else {
      const style = {
        color: TRAIL_COLOR[o.type], weight: 2, opacity: 0.65,
        lineCap: "round", lineJoin: "round",
      };
      t = L.polyline(segments, style);
      t._ghostL = L.polyline(segmentsL, style);
      t._ghostR = L.polyline(segmentsR, style);
      trails.set(o.id, t);
      layers[o.type].addLayer(t);
      layers[o.type].addLayer(t._ghostL);
      layers[o.type].addLayer(t._ghostR);
      t.bringToBack();
      t._ghostL.bringToBack();
      t._ghostR.bringToBack();
    }
  }

  function remove(id) {
    const m = markers.get(id);
    const type = m ? m._sdrType : (objectsById.get(id) || {}).type;
    if (m) {
      layers[m._sdrType].removeLayer(m);
      // Clean up the two ghost copies that mirror the marker on adjacent
      // world copies (±360° longitude). Without this they'd stay on the map.
      if (m._ghostL) layers[m._sdrType].removeLayer(m._ghostL);
      if (m._ghostR) layers[m._sdrType].removeLayer(m._ghostR);
      markers.delete(id);
    }
    const t = trails.get(id);
    if (t && type) {
      layers[type].removeLayer(t);
      if (t._ghostL) layers[type].removeLayer(t._ghostL);
      if (t._ghostR) layers[type].removeLayer(t._ghostR);
      trails.delete(id);
    }
    histories.delete(id);
    objectsById.delete(id);
    refreshCounts();
  }

  function refreshCounts() {
    const c = { "AIS": 0, "ADSB": 0, "APRS": 0, "APRS Meteo": 0, "lrrp": 0, "radiosonde": 0, "TETRA": 0, "SARSAT": 0, "satellite": 0 };
    objectsById.forEach((o) => { if (o.type in c) c[o.type]++; });
    document.getElementById("count-AIS").textContent = c["AIS"];
    document.getElementById("count-ADSB").textContent = c["ADSB"];
    document.getElementById("count-APRS").textContent = c["APRS"];
    document.getElementById("count-METEO").textContent = c["APRS Meteo"];
    document.getElementById("count-LRRP").textContent = c["lrrp"];
    document.getElementById("count-SONDE").textContent = c["radiosonde"];
    document.getElementById("count-TETRA").textContent = c["TETRA"];
    document.getElementById("count-SARSAT").textContent = c["SARSAT"];
    document.getElementById("count-SAT").textContent = c["satellite"];
  }

  // ----------------------------------------------------- Filtres (UI) ------
  const fAll = document.getElementById("f-all");
  const fTypes = Array.from(document.querySelectorAll(".f-type"));

  function applyFilter(type, on) {
    if (on) { if (!map.hasLayer(layers[type])) map.addLayer(layers[type]); }
    else { if (map.hasLayer(layers[type])) map.removeLayer(layers[type]); }
  }
  fTypes.forEach((cb) => cb.addEventListener("change", () => {
    applyFilter(cb.value, cb.checked);
    fAll.checked = fTypes.every((c) => c.checked);
  }));
  fAll.addEventListener("change", () => {
    fTypes.forEach((cb) => { cb.checked = fAll.checked; applyFilter(cb.value, cb.checked); });
  });

  // Case « Afficher les traces » : crée ou retire les polylignes de trajet.
  const fTrails = document.getElementById("f-trails");
  if (fTrails) {
    fTrails.addEventListener("change", () => {
      showTrails = fTrails.checked;
      if (showTrails) {
        objectsById.forEach((o) => rebuildTrail(o));
      } else {
        trails.forEach((t, id) => {
          const o = objectsById.get(id);
          if (o) {
            layers[o.type].removeLayer(t);
            // Ghosts polylines added by rebuildTrail() at ±360° longitude.
            if (t._ghostL) layers[o.type].removeLayer(t._ghostL);
            if (t._ghostR) layers[o.type].removeLayer(t._ghostR);
          }
        });
        trails.clear();
      }
    });
  }

  // ----------------------------------------------- Chargement initial ------
  fetch("api/objects/")
    .then((r) => r.json())
    .then((d) => { (d.objects || []).forEach(upsert); })
    .catch(() => {});

  // ------------------------------------------------- WebSocket live --------
  const connEl = document.getElementById("conn");
  const connTxt = document.getElementById("conn-text");
  let ws, wsRetry = 0;

  // Disconnected overlay: when the WebSocket has been closed for more than
  // a couple of seconds AND the server can't be reached over HTTP, we cover
  // the map with a clear "server unreachable" message. Without this, the
  // service worker keeps serving the cached page after the server is
  // stopped, so the map looks live when it isn't.
  let disconnectedSince = 0;
  let pingTimer = null;
  const overlay = (() => {
    const el = document.createElement("div");
    el.id = "disconnect-overlay";
    el.innerHTML =
      '<div class="disc-card">' +
      '<div class="disc-title">Server unreachable</div>' +
      '<div class="disc-sub">The SDR Map server is not responding.</div>' +
      '<div class="disc-sub">Real-time updates are paused.</div>' +
      '<button id="disc-reload">Refresh</button>' +
      '</div>';
    el.style.display = "none";
    document.body.appendChild(el);
    el.querySelector("#disc-reload").addEventListener("click", () => location.reload());
    return el;
  })();

  function showOverlay() { overlay.style.display = "flex"; }
  function hideOverlay() { overlay.style.display = "none"; }

  // Probe the server with a tiny HTTP fetch (bypassing the service worker
  // cache via a cache-buster). If it fails OR returns a non-200, the server
  // is really down and we surface the overlay.
  function pingServer() {
    return fetch("api/retention/?_=" + Date.now(), {
      method: "GET",
      cache: "no-store",
      // Make the SW network-first treat this as a real network call.
      headers: { "X-Ping": "1" },
    }).then((r) => r.ok).catch(() => false);
  }

  function startPingLoop() {
    if (pingTimer) return;
    pingTimer = setInterval(async () => {
      // Only ping if we believe we're disconnected long enough.
      if (!disconnectedSince) return;
      const downSec = (Date.now() - disconnectedSince) / 1000;
      if (downSec < 3) return;
      const ok = await pingServer();
      if (ok) {
        // Server replied: keep overlay hidden; WS will reconnect on its own.
        hideOverlay();
      } else {
        showOverlay();
      }
    }, 2000);
  }
  function stopPingLoop() {
    if (pingTimer) { clearInterval(pingTimer); pingTimer = null; }
  }

  function connectWS() {
    const url = `${WS_SCHEME}://${location.host}/ws/objects/`;
    ws = new WebSocket(url);
    ws.onopen = () => {
      wsRetry = 0;
      disconnectedSince = 0;
      hideOverlay();
      stopPingLoop();
      connEl.className = "link online"; connTxt.textContent = "live";
    };
    ws.onmessage = (ev) => {
      const msg = JSON.parse(ev.data);
      if (msg.event === "object") upsert(msg.object);
      else if (msg.event === "remove") remove(msg.id);
    };
    ws.onclose = () => {
      connEl.className = "link offline"; connTxt.textContent = "reconnecting…";
      if (!disconnectedSince) disconnectedSince = Date.now();
      startPingLoop();
      wsRetry = Math.min(wsRetry + 1, 6);
      setTimeout(connectWS, 1000 * wsRetry);
    };
    ws.onerror = () => ws.close();
  }
  connectWS();

  // Per-type retention enforcement is set up further below (Retention block).

  // ============================ OFFLINE / SERVICE WORKER ==================
  let sw = null;
  if ("serviceWorker" in navigator) {
    navigator.serviceWorker.register("/sw.js", { scope: "/" })
      .then((reg) => { sw = reg; updateCacheInfo(); })
      .catch((e) => console.warn("SW non enregistré :", e));
  }

  function swPost(message) {
    return new Promise((resolve) => {
      const ctrl = navigator.serviceWorker && navigator.serviceWorker.controller;
      if (!ctrl) return resolve(null);
      const ch = new MessageChannel();
      ch.port1.onmessage = (e) => resolve(e.data);
      ctrl.postMessage(message, [ch.port2]);
    });
  }

  // --- Calcul des tuiles d'une zone pour le fond actif --------------------
  function lon2x(lon, z) { return Math.floor((lon + 180) / 360 * Math.pow(2, z)); }
  function lat2y(lat, z) {
    const r = lat * Math.PI / 180;
    return Math.floor((1 - Math.log(Math.tan(r) + 1 / Math.cos(r)) / Math.PI) / 2 * Math.pow(2, z));
  }

  function tileUrlsForView(layer, extraZoom) {
    const tpl = layer._url;
    const subs = layer.options.subdomains || "abc";
    const sub = Array.isArray(subs) ? subs[0] : subs[0];
    const b = map.getBounds();
    const z0 = map.getZoom();
    const maxZ = layer.options.maxZoom || 19;
    const urls = [];
    for (let z = z0; z <= Math.min(z0 + extraZoom, maxZ); z++) {
      const xMin = lon2x(b.getWest(), z), xMax = lon2x(b.getEast(), z);
      const yMin = lat2y(b.getNorth(), z), yMax = lat2y(b.getSouth(), z);
      for (let x = xMin; x <= xMax; x++) {
        for (let y = yMin; y <= yMax; y++) {
          urls.push(tpl
            .replace("{s}", sub).replace("{z}", z)
            .replace("{x}", x).replace("{y}", y).replace("{r}", ""));
        }
      }
    }
    return urls;
  }

  const btnCache = document.getElementById("btn-cache");
  const progWrap = document.getElementById("cache-progress");
  const progBar = document.getElementById("cache-bar");
  const cacheInfo = document.getElementById("cache-info");

  btnCache.addEventListener("click", async () => {
    if (!navigator.serviceWorker || !navigator.serviceWorker.controller) {
      alert("Service worker not active: reload the page once online.");
      return;
    }
    const urls = tileUrlsForView(activeBase, 2);
    if (urls.length > 4000 &&
        !confirm(`${urls.length} tiles to download. Continue?`)) return;
    progWrap.hidden = false; progBar.style.width = "0%";
    btnCache.disabled = true;
    const res = await swPost({ type: "PREFETCH", urls });
    progBar.style.width = "100%";
    setTimeout(() => { progWrap.hidden = true; }, 800);
    btnCache.disabled = false;
    updateCacheInfo();
    if (res) console.log("Préchargement :", res);
  });

  // Progression poussée par le SW pendant le préchargement.
  if ("serviceWorker" in navigator) {
    navigator.serviceWorker.addEventListener("message", (e) => {
      if (e.data && e.data.type === "PREFETCH_PROGRESS") {
        progBar.style.width = Math.round(e.data.done / e.data.total * 100) + "%";
      }
    });
  }

  async function updateCacheInfo() {
    const res = await swPost({ type: "CACHE_INFO" });
    if (res && typeof res.count === "number") {
      cacheInfo.textContent = res.count + " tuiles en cache";
    }
  }

  document.getElementById("btn-clear").addEventListener("click", async () => {
    if (!confirm("Clear the offline tile cache?")) return;
    await swPost({ type: "CLEAR_TILES" });
    updateCacheInfo();
  });

  // --- Online / offline indicator -----------------------------------------
  const offlineBadge = document.getElementById("offline-badge");
  function netStatus() {
    offlineBadge.hidden = navigator.onLine;
    if (!navigator.onLine) { connEl.className = "link offline"; connTxt.textContent = "offline"; }
  }
  window.addEventListener("online", netStatus);
  window.addEventListener("offline", netStatus);
  netStatus();

  // ============================ AIRCRAFT DB UPDATE ========================
  // Drives the "Aircraft DB" panel section: shows current stats fetched
  // from /api/aircraft_db/status, and triggers a Mictronics refresh via
  // POST to /api/aircraft_db/update when the user clicks the button.
  const acftDbStatusEl = document.getElementById("acft-db-status");
  const acftDbBtn      = document.getElementById("btn-acft-db-update");
  const acftDbLog      = document.getElementById("acft-db-log");

  // Format a Unix timestamp (seconds) as a relative age string:
  // "12 min ago", "3 h ago", "5 days ago". Coarse on purpose — we don't
  // need second-level precision for "when did the DB get refreshed".
  function formatAge(unixSeconds) {
    const ageSec = Date.now() / 1000 - unixSeconds;
    if (ageSec < 60)            return "just now";
    if (ageSec < 3600)          return Math.floor(ageSec / 60) + " min ago";
    if (ageSec < 86400)         return Math.floor(ageSec / 3600) + " h ago";
    const days = Math.floor(ageSec / 86400);
    return days + " day" + (days > 1 ? "s" : "") + " ago";
  }

  function formatMb(bytes) { return (bytes / 1024 / 1024).toFixed(1) + " MB"; }
  function formatThousands(n) { return n.toLocaleString("en-US"); }

  function refreshAircraftDbStatus() {
    fetch("api/aircraft_db/status").then(r => r.json()).then(d => {
      if (!d.exists) {
        acftDbStatusEl.innerHTML =
          "<em>Not installed.</em> Click below to download.";
        return;
      }
      const c = d.counts || {};
      acftDbStatusEl.innerHTML =
        `<div class="acft-db-line"><span>Updated</span><span>${formatAge(d.mtime)}</span></div>` +
        `<div class="acft-db-line"><span>Aircraft</span><span>${formatThousands(c.aircraft || 0)} ` +
        `(${formatThousands(c.military || 0)} mil)</span></div>` +
        `<div class="acft-db-line"><span>Operators</span><span>${formatThousands(c.operators || 0)}</span></div>` +
        `<div class="acft-db-line"><span>Types</span><span>${formatThousands(c.types || 0)}</span></div>` +
        `<div class="acft-db-line"><span>Size</span><span>${formatMb(d.size_bytes)}</span></div>`;
    }).catch(() => {
      acftDbStatusEl.innerHTML = "<em>Status unavailable.</em>";
    });
  }

  if (acftDbBtn) {
    acftDbBtn.addEventListener("click", () => {
      // Lock the UI: the request is synchronous server-side (~3-5 s) and
      // we don't want the user to spam-click it. Visual feedback only.
      const original = acftDbBtn.textContent;
      acftDbBtn.disabled = true;
      acftDbBtn.textContent = "⟳ Updating…";
      acftDbLog.hidden = true;
      acftDbLog.textContent = "";

      fetch("api/aircraft_db/update", { method: "POST" })
        .then(r => r.json())
        .then(d => {
          acftDbLog.hidden = false;
          acftDbLog.textContent = d.log || "(no output)";
          acftDbLog.classList.toggle("acft-db-log-error", !d.ok);
          if (d.ok) refreshAircraftDbStatus();
        })
        .catch(err => {
          acftDbLog.hidden = false;
          acftDbLog.textContent = "Update failed: " + err;
          acftDbLog.classList.add("acft-db-log-error");
        })
        .finally(() => {
          acftDbBtn.disabled = false;
          acftDbBtn.textContent = original;
        });
    });
  }

  refreshAircraftDbStatus();

  // ============================ TRACK REPLAY / GPX EXPORT =================
  const replay = { marker: null, line: null, timer: null };
  const replayStop = document.getElementById("replay-stop");

  function stopReplay() {
    if (replay.timer) { clearInterval(replay.timer); replay.timer = null; }
    if (replay.marker) { map.removeLayer(replay.marker); replay.marker = null; }
    if (replay.line) { map.removeLayer(replay.line); replay.line = null; }
    if (replayStop) replayStop.hidden = true;
  }

  // Build the icon shown by the replay marker as it moves along the track.
  // Reuses glyphHtml(o) so that the SVG matches what was on the map — a
  // helicopter ADS-B object replays with a helicopter icon, an EPIRB with
  // the distress SVG, a satellite with the satellite SVG, etc.
  //
  // Falls back to a neutral "•" if the object has been purged from the
  // client cache (rare: the user clicked Replay then the retention timer
  // fired before the GET /api/track/ completed).
  function replayIcon(o, type) {
    const inner = o ? glyphHtml(o, null) : `<span class="glyph">•</span>`;
    const cls = CLASS[type] || "";
    return L.divIcon({
      className: "", iconSize: [26, 26], iconAnchor: [13, 13],
      html: `<div class="sdr-marker ${cls} replaying">${inner}</div>`,
    });
  }

  function startReplay(type, ident, id) {
    stopReplay();
    // Resolve the live object so we can reuse its icon during replay.
    // id is the internal numeric SdrObject.id passed via data-id on the
    // popup button. If it's still in objectsById we get full enrichment
    // (heading, adsb_category, mmsi_kind...); otherwise null and we use
    // a generic icon.
    const o = id != null ? objectsById.get(parseInt(id, 10)) : null;
    const url = `api/track/?type=${encodeURIComponent(type)}&ident=${encodeURIComponent(ident)}`;
    fetch(url).then((r) => r.json()).then((d) => {
      const pts = (d.points || []).map((p) => [p.lat, p.lon]);
      if (pts.length < 2) { alert("Not enough recorded points to replay this track."); return; }
      map.closePopup();
      replay.line = L.polyline([], { color: "#ff4d6d", weight: 3, opacity: 0.9 }).addTo(map);
      replay.marker = L.marker(pts[0], { icon: replayIcon(o, type), zIndexOffset: 1000 }).addTo(map);
      map.fitBounds(L.latLngBounds(pts).pad(0.2));
      if (replayStop) replayStop.hidden = false;
      let i = 0;
      const accumulated = [];
      replay.timer = setInterval(() => {
        if (i >= pts.length) { clearInterval(replay.timer); replay.timer = null; return; }
        replay.marker.setLatLng(pts[i]);
        accumulated.push(pts[i]);
        // Rebuild the polyline with antimeridian-aware splitting, so the
        // replay trace doesn't slash across the whole map on world-orbiting
        // satellites.
        replay.line.setLatLngs(splitAntimeridian(accumulated));
        i++;
      }, 180);
    }).catch(() => alert("Could not load track."));
  }

  function downloadGpx(type, ident) {
    const url = `api/track/?type=${encodeURIComponent(type)}&ident=${encodeURIComponent(ident)}&format=gpx`;
    window.open(url, "_blank");
  }

  // Délégation : boutons d'action dans les popups.
  document.addEventListener("click", (e) => {
    const btn = e.target.closest(".popup-btn");
    if (!btn) return;
    const type = btn.getAttribute("data-type");
    const ident = btn.getAttribute("data-ident");
    const id = btn.getAttribute("data-id");        // internal numeric id
    if (btn.dataset.act === "replay") startReplay(type, ident, id);
    else if (btn.dataset.act === "gpx") downloadGpx(type, ident);
  });

  if (replayStop) replayStop.addEventListener("click", stopReplay);

  // ----------------------------- Effacer la carte -------------------------
  // Retire de l'affichage tous les marqueurs, traces et historiques. Les
  // objets ne sont PAS supprimés côté serveur : la prochaine position reçue
  // (WebSocket) recréera l'objet à jour. Utile pour repartir « propre ».
  function clearAllObjects() {
    stopReplay();
    markers.forEach((m, id) => {
      const o = objectsById.get(id);
      if (o && layers[o.type]) {
        layers[o.type].removeLayer(m);
        // Ghost copies (±360° lon) added by upsert() — must be removed too,
        // otherwise the duplicates stay on the map after Clear.
        if (m._ghostL) layers[o.type].removeLayer(m._ghostL);
        if (m._ghostR) layers[o.type].removeLayer(m._ghostR);
      }
    });
    trails.forEach((t, id) => {
      const o = objectsById.get(id);
      if (o && layers[o.type]) {
        layers[o.type].removeLayer(t);
        if (t._ghostL) layers[o.type].removeLayer(t._ghostL);
        if (t._ghostR) layers[o.type].removeLayer(t._ghostR);
      }
    });
    markers.clear();
    trails.clear();
    histories.clear();
    objectsById.clear();
    refreshCounts();
  }

  const btnClearObjects = document.getElementById("btn-clear-objects");
  if (btnClearObjects) {
    btnClearObjects.addEventListener("click", clearAllObjects);
  }

  // ============================ RETENTION (per-type TTL) ===================
  // Editable from the side panel: each type has a "minutes" field. Saving
  // POSTs to /api/retention/ and the server immediately purges anything that
  // exceeds the new windows (objects AND tracks). We also drop stale items
  // from the current view so the UI matches what the server kept.
  const RETENTION_DEFAULTS = cfg("cfg-retention-defaults", {});
  const RETENTION_LABELS = {
    "AIS": "AIS — ships",
    "ADSB": "ADS-B — aircraft",
    "APRS": "APRS — stations",
    "APRS Meteo": "APRS — weather",
    "lrrp": "LRRP — DMR GPS",
    "radiosonde": "Radiosondes",
    "TETRA": "TETRA — handhelds",
    "SARSAT": "Cospas-Sarsat beacons",
    "satellite": "Satellites (orbital)",
  };
  const retentionList = document.getElementById("retention-list");
  const retentionStatus = document.getElementById("retention-status");
  let retentionCurrent = Object.assign({}, RETENTION_DEFAULTS);

  function renderRetentionRows(values) {
    retentionList.innerHTML = "";
    Object.keys(RETENTION_LABELS).forEach((t) => {
      const row = document.createElement("label");
      row.className = "retention-row";
      row.innerHTML =
        `<span class="swatch" data-type="${t}"></span>` +
        `<span class="retention-label">${RETENTION_LABELS[t]}</span>` +
        `<input type="number" min="1" step="1" class="retention-input" ` +
        `data-type="${t}" value="${values[t] != null ? values[t] : RETENTION_DEFAULTS[t]}">` +
        `<span class="retention-unit">min</span>`;
      retentionList.appendChild(row);
    });
  }

  function readRetentionInputs() {
    const out = {};
    retentionList.querySelectorAll(".retention-input").forEach((inp) => {
      const v = parseInt(inp.value, 10);
      if (Number.isFinite(v) && v >= 1) out[inp.dataset.type] = v;
    });
    return out;
  }

  function dropStalePerRetention() {
    const now = Date.now();
    const toDrop = [];
    objectsById.forEach((o, id) => {
      const mins = retentionCurrent[o.type];
      if (!mins) return;
      if ((now - new Date(o.last_seen).getTime()) > mins * 60000) toDrop.push(id);
    });
    toDrop.forEach((id) => remove(id));
  }

  fetch("api/retention/").then((r) => r.json()).then((data) => {
    retentionCurrent = Object.assign({}, RETENTION_DEFAULTS, data);
    renderRetentionRows(retentionCurrent);
  }).catch(() => renderRetentionRows(RETENTION_DEFAULTS));

  function setStatus(msg, ok) {
    retentionStatus.textContent = msg;
    retentionStatus.className = "retention-status " + (ok ? "ok" : "ko");
    setTimeout(() => { retentionStatus.textContent = ""; retentionStatus.className = "retention-status"; }, 3500);
  }

  document.getElementById("btn-retention-save").addEventListener("click", () => {
    const payload = readRetentionInputs();
    fetch("api/retention/", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    }).then((r) => r.ok ? r.json() : Promise.reject(r.statusText))
      .then((data) => {
        retentionCurrent = Object.assign({}, RETENTION_DEFAULTS, data);
        renderRetentionRows(retentionCurrent);
        dropStalePerRetention();
        setStatus("Saved.", true);
      })
      .catch((e) => setStatus("Save failed: " + e, false));
  });

  document.getElementById("btn-retention-reset").addEventListener("click", () => {
    renderRetentionRows(RETENTION_DEFAULTS);
  });

  // Periodic client-side enforcement (the server is authoritative; this is a
  // safety net so the UI doesn't keep showing items beyond their retention
  // between two server purges).
  setInterval(dropStalePerRetention, 30000);

  document.getElementById("panel-toggle").addEventListener("click", () => {
    const isCollapsed = panel.classList.toggle("collapsed");
    // Mirror the state on <body> so the CSS can position the toggle
    // button — which is now a sibling of .panel, not a child — without
    // resorting to JS-driven inline styles.
    document.body.classList.toggle("panel-collapsed", isCollapsed);
  });
})();
