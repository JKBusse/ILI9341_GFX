#!/usr/bin/env python3
"""Erzeugt die Kartendaten für die Navi-Szene der Demo (extras/demo/nav_map.h).

Route mit Abbiegehinweisen: OSRM (router.project-osrm.org)
Karte: OpenStreetMap über die Overpass-API, © OpenStreetMap-Mitwirkende (ODbL)

  mapgen.py               Daten holen (zwischengespeichert in extras/tools/.mapcache) und Header schreiben
  mapgen.py --refresh     Daten neu holen

Koordinaten: lokale Meter um den Kartenmittelpunkt (x nach Osten, y nach Süden),
als int16 in halben Metern. Linien und Flächen werden mit Douglas-Peucker vereinfacht.
"""
import argparse
import json
import math
import os
import sys
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
CACHE = os.path.join(HERE, ".mapcache")
OUT = os.path.join(ROOT, "extras", "demo", "nav_map.h")
UA = "ILI9341_GFX-demo-mapgen/1.0 (https://github.com/JKBusse/ILI9341_GFX)"

# Fahrt vom Steintor zur HDI Arena: Leineufer, Altstadt, Waterlooplatz, Maschsee (lon, lat)
WAYPOINTS = [
    (9.7338, 52.3756),   # Steintor (Kurt-Schumacher-Straße)
    (9.7313, 52.3600),   # HDI Arena (Robert-Enke-Straße)
]
DEST_NAME = "HDI Arena"
# Gefahrener Abschnitt der OSRM-Route in Metern: ohne die Wendeschleife am Start
# und die Wende vor der Arena
TRIM = (320.0, 3200.0)
MARGIN_M = 350

ROAD_CLASSES = {
    "motorway": "PRIMARY", "trunk": "PRIMARY", "primary": "PRIMARY",
    "motorway_link": "PRIMARY", "trunk_link": "PRIMARY", "primary_link": "PRIMARY",
    "secondary": "SECONDARY", "secondary_link": "SECONDARY",
    "tertiary": "TERTIARY", "tertiary_link": "TERTIARY",
    "unclassified": "MINOR", "residential": "MINOR", "living_street": "MINOR",
    "pedestrian": "PEDESTRIAN",
}
LINE_ORDER = ["RAIL", "RIVER", "PEDESTRIAN", "MINOR", "TERTIARY", "SECONDARY", "PRIMARY"]
AREA_ORDER = ["PARK", "WATER", "BUILDING"]
TOL = {"line": 1.0, "BUILDING": 0.8, "WATER": 2.0, "PARK": 2.0, "route": 0.5}
UNIT = 0.5  # Meter pro Koordinateneinheit


def fetch(name, url, data=None, refresh=False):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, name)
    if os.path.exists(path) and not refresh:
        return json.load(open(path))
    print(f"lade {name} ...", file=sys.stderr)
    req = urllib.request.Request(url, data=data, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=180) as r:
        body = r.read()
    open(path, "wb").write(body)
    return json.loads(body)


class Proj:
    def __init__(self, lat0, lon0):
        self.lat0, self.lon0 = lat0, lon0
        self.kx = math.cos(math.radians(lat0)) * 111320.0
        self.ky = 110540.0

    def __call__(self, lon, lat):
        return ((lon - self.lon0) * self.kx, -(lat - self.lat0) * self.ky)


def rdp(pts, tol):
    if len(pts) < 3:
        return pts
    keep = [False] * len(pts)
    keep[0] = keep[-1] = True
    stack = [(0, len(pts) - 1)]
    while stack:
        a, b = stack.pop()
        ax, ay = pts[a]
        bx, by = pts[b]
        dx, dy = bx - ax, by - ay
        L = math.hypot(dx, dy)
        best, idx = -1.0, -1
        for i in range(a + 1, b):
            px, py = pts[i]
            d = abs(dy * (px - ax) - dx * (py - ay)) / L if L > 1e-9 else math.hypot(px - ax, py - ay)
            if d > best:
                best, idx = d, i
        if best > tol:
            keep[idx] = True
            stack += [(a, idx), (idx, b)]
    return [p for p, k in zip(pts, keep) if k]


def quant(pts):
    out = []
    for x, y in pts:
        q = (int(round(x / UNIT)), int(round(y / UNIT)))
        assert -32768 <= q[0] <= 32767 and -32768 <= q[1] <= 32767
        if not out or out[-1] != q:
            out.append(q)
    return out


def area(pts):
    s = 0.0
    for (x0, y0), (x1, y1) in zip(pts, pts[1:] + pts[:1]):
        s += x0 * y1 - x1 * y0
    return abs(s) / 2


def ascii_name(s):
    return s.replace('"', "'")


ANGLES = {"straight": 0, "slight right": 40, "right": 90, "sharp right": 135,
          "slight left": -40, "left": -90, "sharp left": -135, "uturn": 180}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--refresh", action="store_true")
    args = ap.parse_args()

    coords = ";".join(f"{lon},{lat}" for lon, lat in WAYPOINTS)
    route = fetch("route.json", "https://router.project-osrm.org/route/v1/driving/" + coords +
                  "?overview=full&geometries=geojson&steps=true", refresh=args.refresh)
    r = route["routes"][0]
    geo = r["geometry"]["coordinates"]
    lons = [p[0] for p in geo]
    lats = [p[1] for p in geo]
    lat0, lon0 = (min(lats) + max(lats)) / 2, (min(lons) + max(lons)) / 2
    P = Proj(lat0, lon0)
    dlat = MARGIN_M / 110540.0
    dlon = MARGIN_M / P.kx
    bbox = (min(lats) - dlat, min(lons) - dlon, max(lats) + dlat, max(lons) + dlon)
    bb = ",".join(f"{v:.6f}" for v in bbox)

    q = f"""[out:json][timeout:120];
(
  way["highway"~"^(motorway|trunk|primary|secondary|tertiary|unclassified|residential|living_street|motorway_link|trunk_link|primary_link|secondary_link|tertiary_link|pedestrian)$"]({bb});
  way["railway"~"^(rail|light_rail|tram)$"]({bb});
  way["waterway"~"^(river|canal)$"]({bb});
  way["natural"="water"]({bb}); relation["natural"="water"]({bb});
  way["leisure"="park"]({bb}); relation["leisure"="park"]({bb});
  way["landuse"~"^(grass|forest|recreation_ground|cemetery|meadow)$"]({bb});
  way["building"]({bb});
);
out geom;"""
    osm = fetch("osm.json", "https://overpass-api.de/api/interpreter",
                data=urllib.parse.urlencode({"data": q}).encode(), refresh=args.refresh)

    lines = {c: [] for c in LINE_ORDER}
    areas = {c: [] for c in AREA_ORDER}

    def geom_pts(g):
        return [P(p["lon"], p["lat"]) for p in g]

    for el in osm["elements"]:
        t = el.get("tags", {})
        if el["type"] == "way":
            if "geometry" not in el:
                continue
            pts = geom_pts(el["geometry"])
            closed = len(pts) > 3 and el["nodes"][0] == el["nodes"][-1]
            if t.get("tunnel") in ("yes", "building_passage") or t.get("layer", "0").startswith("-"):
                continue
            if "highway" in t:
                if t.get("area") == "yes":
                    continue
                lines[ROAD_CLASSES[t["highway"]]].append(pts)
            elif "railway" in t:
                lines["RAIL"].append(pts)
            elif "waterway" in t:
                lines["RIVER"].append(pts)
            elif closed and t.get("natural") == "water":
                areas["WATER"].append([pts])
            elif closed and ("leisure" in t or "landuse" in t):
                areas["PARK"].append([pts])
            elif closed and "building" in t:
                if area(pts) >= 40:
                    areas["BUILDING"].append([pts])
        elif el["type"] == "relation":
            parts = [geom_pts(m["geometry"]) for m in el.get("members", [])
                     if m["type"] == "way" and m.get("geometry") and m.get("role") in ("outer", "inner")]
            if not parts:
                continue
            cls = "WATER" if t.get("natural") == "water" else "PARK"
            areas[cls].append(parts)

    # Route und Abbiegehinweise
    full = [P(lon, lat) for lon, lat in geo]
    cumf = [0.0]
    for (x0, y0), (x1, y1) in zip(full, full[1:]):
        cumf.append(cumf[-1] + math.hypot(x1 - x0, y1 - y0))

    def at(sv):  # Punkt auf der vollen Route bei Strecke sv
        for i in range(1, len(full)):
            if cumf[i] >= sv:
                t = (sv - cumf[i - 1]) / max(1e-9, cumf[i] - cumf[i - 1])
                return (full[i - 1][0] + t * (full[i][0] - full[i - 1][0]),
                        full[i - 1][1] + t * (full[i][1] - full[i - 1][1]))
        return full[-1]

    a_s, b_s = TRIM
    rpts = [at(a_s)] + [p for p, c in zip(full, cumf) if a_s < c < b_s] + [at(b_s)]
    rpts = rdp(rpts, TOL["route"])
    cum = [0.0]
    for (x0, y0), (x1, y1) in zip(rpts, rpts[1:]):
        cum.append(cum[-1] + math.hypot(x1 - x0, y1 - y0))
    length = cum[-1]

    def s_full(lon, lat, start_i):
        x, y = P(lon, lat)
        best, bi = 1e18, start_i
        for i in range(start_i, min(len(full), start_i + 600)):
            d = math.hypot(full[i][0] - x, full[i][1] - y)
            if d < best:
                best, bi = d, i
        return bi

    names, name_idx = [], {}

    def nid(s):
        s = ascii_name(s or "")
        if s not in name_idx:
            name_idx[s] = len(names)
            names.append(s)
        return name_idx[s]

    streets, maneuvers = [], []
    fi = 0
    steps = [st for leg in r["legs"] for st in leg["steps"]]
    for st in steps:
        m = st["maneuver"]
        fi = s_full(m["location"][0], m["location"][1], fi)
        s = cumf[fi] - a_s
        name = st.get("name") or st.get("ref") or ""
        typ, mod = m["type"], m.get("modifier", "straight")
        if s >= length - 1:
            break
        if typ in ("arrive", "depart"):
            if s <= 0:
                streets = [(0.0, nid(name))]
            continue
        if s <= 5:
            streets = [(0.0, nid(name))]  # Straße, auf der die Fahrt beginnt
            continue
        streets.append((s, nid(name)))
        if typ in ("roundabout", "rotary", "roundabout turn"):
            maneuvers.append((s, "ROUNDABOUT", ANGLES.get(mod, 0), m.get("exit", 1), nid(name)))
        elif typ in ("new name", "notification", "exit roundabout", "exit rotary") or \
                (mod == "straight" and typ not in ("fork", "end of road")):
            continue
        else:
            maneuvers.append((s, "TURN", ANGLES.get(mod, 0), 0, nid(name)))
    maneuvers.append((length, "ARRIVE", 0, 0, nid(DEST_NAME)))
    # Hinweise, die zu dicht aufeinander folgen, zusammenfassen (der erste gewinnt)
    merged = []
    for mv in maneuvers:
        if merged and mv[0] - merged[-1][0] < 8 and mv[1] != "ARRIVE":
            continue
        merged.append(mv)
    maneuvers = merged

    # Ausgabe: alle Objekte nach 128-m-Rasterzellen sortiert (auch ihre Punkte),
    # damit die Firmware pro Bild nur die Zellen um das Auto liest und die
    # Daten im Flash beieinander liegen (der Flash-Cache des ESP32 hat 32 KB).
    # Lange Straßen werden in kurze Stücke zerlegt; große Flächen kommen in
    # Zelle -1, die immer geprüft wird.
    CELL = int(128 / UNIT)
    pts_all, line_rec, area_rec, part_rec = [], [], [], []

    def bbox_of(ps):
        xs = [p[0] for p in ps]
        ys = [p[1] for p in ps]
        return min(xs), min(ys), max(xs), max(ys)

    objs = []  # (art, klasse, punkte/teile, bbox)
    for ci, cls in enumerate(LINE_ORDER):
        for pts in lines[cls]:
            qp = quant(rdp(pts, TOL["line"]))
            start = 0
            while start < len(qp) - 1:  # Stücke von höchstens ~120 m bzw. 24 Punkten
                end, seg_len = start, 0.0
                while end < len(qp) - 1 and end - start < 24 and seg_len < 120 / UNIT:
                    seg_len += math.hypot(qp[end + 1][0] - qp[end][0], qp[end + 1][1] - qp[end][1])
                    end += 1
                chunk = qp[start:end + 1]
                objs.append(("L", cls, chunk, bbox_of(chunk)))
                start = end
    for cls in AREA_ORDER:
        for parts in areas[cls]:
            qparts = [quant(rdp(p, TOL[cls])) for p in parts]
            qparts = [p for p in qparts if len(p) >= 2]
            if qparts:
                objs.append(("A", cls, qparts, bbox_of([p for qp in qparts for p in qp])))

    gx0 = min(o[3][0] for o in objs)
    gy0 = min(o[3][1] for o in objs)
    gx1 = max(o[3][2] for o in objs)
    gy1 = max(o[3][3] for o in objs)
    GX = (gx1 - gx0) // CELL + 1
    GY = (gy1 - gy0) // CELL + 1

    def cell_of(o):
        x0, y0, x1, y1 = o[3]
        if x1 - x0 > CELL or y1 - y0 > CELL:
            return -1
        return ((y0 + y1) // 2 - gy0) // CELL * GX + ((x0 + x1) // 2 - gx0) // CELL

    objs.sort(key=lambda o: cell_of(o))  # stabil: Klassenreihenfolge bleibt je Zelle erhalten
    line_start = [0] * (GX * GY + 2)  # Index -1 -> Eintrag 0
    area_start = [0] * (GX * GY + 2)
    for o in objs:
        c = cell_of(o) + 1
        if o[0] == "L":
            line_start[c + 1] += 1
            line_rec.append((o[1], len(pts_all), len(o[2])) + o[3])
            pts_all += o[2]
        else:
            area_start[c + 1] += 1
            area_rec.append((o[1], len(part_rec), len(o[2])) + o[3])
            for qp in o[2]:
                part_rec.append((len(pts_all), len(qp)))
                pts_all += qp
    for i in range(1, len(line_start)):
        line_start[i] += line_start[i - 1]
        area_start[i] += area_start[i - 1]
    route_q = quant(rpts)

    with open(OUT, "w") as f:
        w = f.write
        w("// Automatisch erzeugt von extras/tools/mapgen.py - nicht von Hand bearbeiten.\n")
        w("// Kartendaten (c) OpenStreetMap-Mitwirkende, ODbL (openstreetmap.org/copyright).\n")
        w("// Route und Abbiegehinweise: OSRM (project-osrm.org).\n")
        w("#pragma once\n#include <stdint.h>\n\nnamespace navmap {\n")
        w(f"const float UNIT = {UNIT}f; // Meter pro Koordinateneinheit\n")
        w("enum LineCls : uint8_t { " + ", ".join(f"L_{c}" for c in LINE_ORDER) + " };\n")
        w("enum AreaCls : uint8_t { " + ", ".join(f"A_{c}" for c in AREA_ORDER) + " };\n")
        w("enum ManKind : uint8_t { M_TURN, M_ROUNDABOUT, M_ARRIVE };\n")
        w("struct Line { uint8_t cls; uint32_t pt; uint16_t n; int16_t x0, y0, x1, y1; };\n")
        w("struct Area { uint8_t cls; uint32_t part; uint16_t nparts; int16_t x0, y0, x1, y1; };\n")
        w("struct Part { uint32_t pt; uint16_t n; };\n")
        w("struct Street { float s; uint16_t name; };\n")
        w("struct Maneuver { float s; uint8_t kind; int16_t angle; uint8_t exit; uint16_t name; };\n\n")
        w(f"const int16_t PTS[{len(pts_all)}][2] = {{\n")
        for i in range(0, len(pts_all), 8):
            w("  " + " ".join(f"{{{x},{y}}}," for x, y in pts_all[i:i + 8]) + "\n")
        w("};\n")
        w(f"const Line LINES[{len(line_rec)}] = {{\n")
        for c, p, n, x0, y0, x1, y1 in line_rec:
            w(f"  {{L_{c},{p},{n},{x0},{y0},{x1},{y1}}},\n")
        w("};\n")
        w(f"const Area AREAS[{len(area_rec)}] = {{\n")
        for c, p, n, x0, y0, x1, y1 in area_rec:
            w(f"  {{A_{c},{p},{n},{x0},{y0},{x1},{y1}}},\n")
        w("};\n")
        w(f"const Part PARTS[{len(part_rec)}] = {{\n")
        for i in range(0, len(part_rec), 8):
            w("  " + " ".join(f"{{{p},{n}}}," for p, n in part_rec[i:i + 8]) + "\n")
        w("};\n")
        w(f"// Raster: Zelle (cx, cy) -> Index 1 + cy * GX + cx, Index 0 = große Objekte\n")
        w(f"const int16_t GRID_X0 = {gx0}, GRID_Y0 = {gy0}, CELL = {CELL}, GX = {GX}, GY = {GY};\n")
        w(f"const uint16_t CELL_LINES[{len(line_start)}] = {{" + ",".join(map(str, line_start)) + "};\n")
        w(f"const uint16_t CELL_AREAS[{len(area_start)}] = {{" + ",".join(map(str, area_start)) + "};\n")
        w(f"const int16_t ROUTE[{len(route_q)}][2] = {{\n")
        for i in range(0, len(route_q), 8):
            w("  " + " ".join(f"{{{x},{y}}}," for x, y in route_q[i:i + 8]) + "\n")
        w("};\n")
        w(f"const char *const NAMES[{len(names)}] = {{\n")
        for nm in names:
            w(f"  \"{nm}\",\n")
        w("};\n")
        w(f"const Street STREETS[{len(streets)}] = {{\n")
        for s, n in streets:
            w(f"  {{{s:.1f}f,{n}}},\n")
        w("};\n")
        w(f"const Maneuver MANEUVERS[{len(maneuvers)}] = {{\n")
        for s, kind, ang, ex, n in maneuvers:
            w(f"  {{{s:.1f}f,M_{kind},{ang},{ex},{n}}},  // {names[n]}\n")
        w("};\n")
        w("} // namespace navmap\n")

    size = len(pts_all) * 4 + len(line_rec) * 16 + len(area_rec) * 16 + len(part_rec) * 8 + len(route_q) * 4
    print(f"Route {length:.0f} m, {len(route_q)} Punkte, {len(maneuvers)} Hinweise, {len(streets)} Straßenabschnitte")
    print(f"Raster {GX}x{GY}, {line_start[1]} große Linien, {area_start[1]} große Flächen")
    print(f"{len(line_rec)} Linien, {len(area_rec)} Flächen, {len(pts_all)} Punkte, ca. {size / 1024:.0f} KB")
    for c in LINE_ORDER:
        print(f"  {c:10s} {len(lines[c])}")
    for c in AREA_ORDER:
        print(f"  {c:10s} {len(areas[c])}")


if __name__ == "__main__":
    main()
