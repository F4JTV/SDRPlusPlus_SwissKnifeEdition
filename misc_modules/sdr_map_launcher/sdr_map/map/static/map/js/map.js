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
  // re-applique l'orientation de chaque marqueur pour qu'aucun avion ne
  // « retombe » sur un ancien cap.
  map.on("zoomend", () => {
    markers.forEach((m) => {
      if (m._sdrData) updateMarkerDom(m, m._sdrData);
    });
  });

  // Contrôles repositionnés en bas à droite pour ne pas être masqués par la
  // barre supérieure ni par le panneau de gauche.
  L.control.zoom({ position: "bottomright" }).addTo(map);
  L.control.layers(baseLayers, null, {
    position: "bottomright", collapsed: false,
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
  };
  const markers = new Map();        // id -> L.marker
  const objectsById = new Map();    // id -> data
  const histories = new Map();      // id -> [[lat,lon], ...] (trajet)
  const trails = new Map();         // id -> L.polyline (trace)

  const CLASS = { "AIS": "t-AIS", "ADSB": "t-ADSB", "APRS": "t-APRS", "APRS Meteo": "t-METEO", "lrrp": "t-LRRP", "radiosonde": "t-SONDE", "TETRA": "t-TETRA" };
  // Libellé lisible affiché dans les popups (le champ "type" reste la clé).
  const TYPE_LABEL = { "lrrp": "LRRP", "radiosonde": "SONDE", "TETRA": "TETRA" };
  // Couleurs des traces (alignées sur le CSS par type).
  const TRAIL_COLOR = {
    "AIS": "#22d3ee", "ADSB": "#f6a609", "APRS": "#34d399",
    "APRS Meteo": "#a78bfa", "lrrp": "#f472b6", "radiosonde": "#facc15",
    "TETRA": "#fb923c",
  };
  const MAX_TRAIL = 200;            // nombre max de positions conservées par objet
  let showTrails = true;            // piloté par la case « Afficher les traces »

  // SVG d'avion vu de dessus, NEZ VERS LE HAUT (nord) par construction.
  // Comme il pointe nord à 0°, rotate(heading) donne l'orientation exacte,
  // sans dépendre de la police/OS (contrairement au caractère ✈).
  const PLANE_SVG =
    '<svg viewBox="0 0 24 24" width="22" height="22" fill="currentColor" ' +
    'xmlns="http://www.w3.org/2000/svg">' +
    '<path d="M12 2 L13.2 9 L21 13 L21 15 L13.2 12.6 L13 19 L16 21 L16 22 ' +
    'L12 21 L8 22 L8 21 L11 19 L10.8 12.6 L3 15 L3 13 L10.8 9 Z"/></svg>';

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
      return `<span class="glyph plane" style="${rot}">${PLANE_SVG}</span>`;
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
    return `<span class="glyph" style="${rot}">${GLYPH[o.type] || "•"}</span>`;
  }

  function makeIcon(o) {
    const r = iconRotation(o);
    const label = o.name || o.ident || "";
    return L.divIcon({
      className: "",
      iconSize: [24, 24], iconAnchor: [12, 12],
      html: `<div class="sdr-marker ${CLASS[o.type]}">
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
      if (o.country_name || o.country_iso) {
        const flag = flagHtml(o.country_iso);
        const cc = o.country_iso ? ` (${o.country_iso})` : "";
        const txt = `${flag}${o.country_name || o.country_iso}${cc}`;
        // Country row: don't HTML-escape (we built the flag <img> ourselves)
        grid += `<span class="k">Flag</span><span class="v">${txt}</span>`;
      } else if (o.mid) {
        grid += row("MID", o.mid);
      }
    }
    if (o.icao) grid += row("ICAO", o.icao);
    if (o.ssi)  grid += row("SSI",  o.ssi);
    if (o.source) grid += row("RID DMR", o.source);
    // TETRA LIP carries GPS accuracy in metres ("±20 m"): we surface it
    // so the operator can judge how trustworthy the position is.
    if (o.type === "TETRA" && o.gps_acc_m != null) {
      grid += row("GPS accuracy", "±" + Math.round(o.gps_acc_m) + " m");
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
    const info = o.info ? `<div class="popup-info">${escapeHtml(o.info)}</div>` : "";
    const ds = `data-type="${escapeHtml(o.type)}" data-ident="${escapeHtml(o.ident)}"`;
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
      // Mise à jour « vivante » : on déplace le marqueur et on ajuste
      // seulement la rotation/le libellé sur le DOM existant, sans recréer
      // l'icône (sinon le marqueur clignote et ne « glisse » pas).
      m.setLatLng([o.lat, o.lon]);
      m._sdrData = o;
      updateMarkerDom(m, o);
      // Re-applique la rotation au tick suivant : si Leaflet a (re)généré
      // l'élément pendant ce cycle (zoom/pan), le transform est repositionné.
      requestAnimationFrame(() => updateMarkerDom(m, o));
      if (m.getPopup()) m.setPopupContent(popupHtml(o));
    } else {
      m = L.marker([o.lat, o.lon], {
        icon: makeIcon(o),
        // Transition CSS douce du déplacement (voir .leaflet-marker-icon).
        riseOnHover: true,
      }).bindPopup(popupHtml(o));
      m._sdrType = o.type;
      m._sdrData = o;
      markers.set(o.id, m);
      layers[o.type].addLayer(m);
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
  function rebuildTrail(o) {
    const h = histories.get(o.id);
    let t = trails.get(o.id);
    if (!h || h.length < 2) {                 // pas (encore) de trajet
      if (t) { layers[o.type].removeLayer(t); trails.delete(o.id); }
      return;
    }
    if (t) {
      t.setLatLngs(h);
    } else {
      t = L.polyline(h, {
        color: TRAIL_COLOR[o.type], weight: 2, opacity: 0.65,
        lineCap: "round", lineJoin: "round",
      });
      trails.set(o.id, t);
      layers[o.type].addLayer(t);
      t.bringToBack();                         // trace sous les marqueurs
    }
  }

  function remove(id) {
    const m = markers.get(id);
    const type = m ? m._sdrType : (objectsById.get(id) || {}).type;
    if (m) { layers[m._sdrType].removeLayer(m); markers.delete(id); }
    const t = trails.get(id);
    if (t && type) { layers[type].removeLayer(t); trails.delete(id); }
    histories.delete(id);
    objectsById.delete(id);
    refreshCounts();
  }

  function refreshCounts() {
    const c = { "AIS": 0, "ADSB": 0, "APRS": 0, "APRS Meteo": 0, "lrrp": 0, "radiosonde": 0, "TETRA": 0 };
    objectsById.forEach((o) => { if (o.type in c) c[o.type]++; });
    document.getElementById("count-AIS").textContent = c["AIS"];
    document.getElementById("count-ADSB").textContent = c["ADSB"];
    document.getElementById("count-APRS").textContent = c["APRS"];
    document.getElementById("count-METEO").textContent = c["APRS Meteo"];
    document.getElementById("count-LRRP").textContent = c["lrrp"];
    document.getElementById("count-SONDE").textContent = c["radiosonde"];
    document.getElementById("count-TETRA").textContent = c["TETRA"];
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
          if (o) layers[o.type].removeLayer(t);
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

  // ============================ TRACK REPLAY / GPX EXPORT =================
  const replay = { marker: null, line: null, timer: null };
  const replayStop = document.getElementById("replay-stop");

  function stopReplay() {
    if (replay.timer) { clearInterval(replay.timer); replay.timer = null; }
    if (replay.marker) { map.removeLayer(replay.marker); replay.marker = null; }
    if (replay.line) { map.removeLayer(replay.line); replay.line = null; }
    if (replayStop) replayStop.hidden = true;
  }

  function replayIcon(type) {
    return L.divIcon({
      className: "", iconSize: [26, 26], iconAnchor: [13, 13],
      html: `<div class="sdr-marker ${CLASS[type]} replaying">
               <span class="glyph">${GLYPH[type]}</span></div>`,
    });
  }

  function startReplay(type, ident) {
    stopReplay();
    const url = `api/track/?type=${encodeURIComponent(type)}&ident=${encodeURIComponent(ident)}`;
    fetch(url).then((r) => r.json()).then((d) => {
      const pts = (d.points || []).map((p) => [p.lat, p.lon]);
      if (pts.length < 2) { alert("Not enough recorded points to replay this track."); return; }
      map.closePopup();
      replay.line = L.polyline([], { color: "#ff4d6d", weight: 3, opacity: 0.9 }).addTo(map);
      replay.marker = L.marker(pts[0], { icon: replayIcon(type), zIndexOffset: 1000 }).addTo(map);
      map.fitBounds(L.latLngBounds(pts).pad(0.2));
      if (replayStop) replayStop.hidden = false;
      let i = 0;
      replay.timer = setInterval(() => {
        if (i >= pts.length) { clearInterval(replay.timer); replay.timer = null; return; }
        replay.marker.setLatLng(pts[i]);
        replay.line.addLatLng(pts[i]);
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
    if (btn.dataset.act === "replay") startReplay(type, ident);
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
      if (o && layers[o.type]) layers[o.type].removeLayer(m);
    });
    trails.forEach((t, id) => {
      const o = objectsById.get(id);
      if (o && layers[o.type]) layers[o.type].removeLayer(t);
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
    panel.classList.toggle("collapsed");
  });
})();
