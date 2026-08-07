"""DoveBox Aggregator — panel JSON para DoveBox (ESP32).

Consolida:
  - Noticias RSS (Marca, Mundo Deportivo, El Español, Xataka) + Besoccer scrape
  - Sensores Home Assistant (temperatura/humedad)
  - Lista ToDo propia de DoveBox (gestionada por voz)

Endpoints:
  GET /api/dashboard  -> todo consolidado
  GET /api/news       -> titulares RSS
  GET /api/home       -> sensores HA
  GET /api/todos      -> lista ToDo
  POST /api/todos     -> añadir tarea  {"text": "..."}
  PATCH /api/todos/{idx} -> marcar done/undone {"done": bool}
  DELETE /api/todos/{idx} -> borrar tarea
  GET /preview        -> página HTML del simulador del dispositivo
  GET /health         -> healthcheck
"""

import json
import html
import os
import re
import threading
import time
import urllib.request

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import xml.etree.ElementTree as ET

# ---------------------------------------------------------------- config
HA_URL = os.environ.get("HA_URL", "http://192.168.100.73:8123")
HA_TOKEN = os.environ.get("HASS_TOKEN", "")

NEWS_FEEDS = {
    "marca": "https://e00-marca.uecdn.es/rss/portada.xml",
    "mundodeportivo": "https://www.mundodeportivo.com/rss/portada.xml",
    "elespanol": "https://www.elespanol.com/rss",
    "xataka": "https://feeds.weblogssl.com/xataka2",
    "google": "https://news.google.com/rss?hl=es&gl=ES&ceid=ES:es",
}
# Nombres visibles por feed (fallback cuando un item no trae <source> propio)
FEED_LABELS = {
    "marca": "MARCA",
    "mundodeportivo": "Mundo Deportivo",
    "elespanol": "El Español",
    "xataka": "Xataka",
    "besoccer": "BeSoccer",
    "google": "Google Noticias",
}
# Besoccer no tiene RSS — se scrapea (ver fetch_besoccer)
BESOCCER_URL = "https://es.besoccer.com/noticias"
CHROME_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
             "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36")
MAX_ITEMS_PER_FEED = int(os.environ.get("MAX_ITEMS_PER_FEED", "5"))
CACHE_TTL = int(os.environ.get("CACHE_TTL", "300"))  # 5 min

# Sensores HA: (nombre mostrado, entity temp, entity hum)
HOME_SENSORS = [
    ("Balcón Salón",
     "sensor.0x00158d0002e98447_temperature",
     "sensor.0x00158d0002e98447_humidity"),
    ("Hab. Principal",
     "sensor.0x00158d0002f820a3_temperature",
     "sensor.0x00158d0002f820a3_humidity"),
]

TODOS_FILE = os.environ.get("TODOS_FILE", "/data/todos.json")

# ---------------------------------------------------------------- estado
_cache = {"news": None, "news_ts": 0, "home": None, "home_ts": 0}
_lock = threading.Lock()


def load_todos():
    try:
        with open(TODOS_FILE) as f:
            return json.load(f)
    except Exception:
        return []


def save_todos(todos):
    with open(TODOS_FILE, "w") as f:
        json.dump(todos, f, ensure_ascii=False, indent=2)


# ---------------------------------------------------------------- RSS
def fetch_rss(url, limit=5):
    req = urllib.request.Request(url, headers={
        "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) DoveBox-Aggregator/1.0",
        "Accept": "application/rss+xml, application/xml, text/xml, */*",
    })
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = resp.read()

    root = ET.fromstring(data)
    items = []
    # RSS 2.0: rss/channel/item ; Atom: feed/entry
    channel = root.find("channel")
    if channel is not None:
        for item in channel.findall("item"):
            title = (item.findtext("title") or "").strip()
            link = (item.findtext("link") or "").strip()
            # Google News pone el medio en <source> y lo repite como sufijo
            # del título ("Noticia - El País"). Lo extraemos como provider.
            source_el = item.find("source")
            provider = (source_el.text or "").strip() if source_el is not None else ""
            if provider:
                suffix = " - " + provider
                if title.endswith(suffix):
                    title = title[: -len(suffix)]
            if title:
                items.append({"title": html.unescape(title), "link": link, "provider": provider})
            if len(items) >= limit:
                break
    else:
        for entry in root.findall("{http://www.w3.org/2005/Atom}entry"):
            title = (entry.findtext("{http://www.w3.org/2005/Atom}title") or "").strip()
            link_el = entry.find("{http://www.w3.org/2005/Atom}link")
            link = link_el.get("href", "") if link_el is not None else ""
            source_el = entry.find("{http://www.w3.org/2005/Atom}source/{http://www.w3.org/2005/Atom}title")
            provider = (source_el.text or "").strip() if source_el is not None else ""
            if provider:
                suffix = " - " + provider
                if title.endswith(suffix):
                    title = title[: -len(suffix)]
            if title:
                items.append({"title": html.unescape(title), "link": link, "provider": provider})
            if len(items) >= limit:
                break
    return items


def fetch_besoccer(url, limit=5):
    """Scrape de es.besoccer.com/noticias — no tienen RSS y bloquean curl simple.

    Con headers completos de Chrome responde 200. Títulos en <h3 itemprop="headline">,
    links en <a class="news" href="...">, categoría en <h4 itemprop="headline">.
    """
    req = urllib.request.Request(url, headers={
        "User-Agent": CHROME_UA,
        "Accept": ("text/html,application/xhtml+xml,application/xml;q=0.9,"
                   "image/avif,image/webp,image/apng,*/*;q=0.8"),
        "Accept-Language": "es-ES,es;q=0.9",
        "Accept-Encoding": "gzip, deflate, br",
        "Connection": "keep-alive",
        "Upgrade-Insecure-Requests": "1",
        "Sec-Fetch-Dest": "document",
        "Sec-Fetch-Mode": "navigate",
        "Sec-Fetch-Site": "none",
        "Sec-Fetch-User": "?1",
    })
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = resp.read()
        if resp.headers.get("Content-Encoding") == "gzip":
            import gzip
            data = gzip.decompress(data)

    html = data.decode("utf-8", errors="ignore")
    items = []
    for m in re.finditer(r'<a[^>]*class="news[^"]*"[^>]*href="([^"]+)"[^>]*>(.*?)</a>', html, re.S):
        href, inner = m.groups()
        tm = re.search(r'<h3[^>]*>(.*?)</h3>', inner, re.S)
        if not tm:
            continue
        title = re.sub(r'<[^>]+>', ' ', tm.group(1))
        title = re.sub(r'\s+', ' ', title).strip()
        if title and title not in [i["title"] for i in items]:
            items.append({"title": title, "link": href})
        if len(items) >= limit:
            break
    return items


def get_news():
    now = time.time()
    with _lock:
        if _cache["news"] and (now - _cache["news_ts"]) < CACHE_TTL:
            return _cache["news"]

    result = {}
    for name, url in NEWS_FEEDS.items():
        try:
            items = fetch_rss(url, MAX_ITEMS_PER_FEED)
        except Exception as e:
            items = [{"title": f"⚠️ Error feed {name}: {e}", "link": ""}]
        for it in items:
            if not it.get("provider"):
                it["provider"] = FEED_LABELS.get(name, name)
        result[name] = items
    try:
        items = fetch_besoccer(BESOCCER_URL, MAX_ITEMS_PER_FEED)
    except Exception as e:
        items = [{"title": f"⚠️ Error besoccer: {e}", "link": ""}]
    for it in items:
        if not it.get("provider"):
            it["provider"] = FEED_LABELS.get("besoccer", "BeSoccer")
    result["besoccer"] = items

    with _lock:
        _cache["news"] = result
        _cache["news_ts"] = now
    return result


# ---------------------------------------------------------------- HA
def ha_get_state(entity_id):
    req = urllib.request.Request(
        f"{HA_URL}/api/states/{entity_id}",
        headers={"Authorization": f"Bearer {HA_TOKEN}"},
    )
    with urllib.request.urlopen(req, timeout=10) as resp:
        data = json.loads(resp.read())
    return {"state": data.get("state"), "unit": (data.get("attributes") or {}).get("unit_of_measurement", "")}


def ha_get_history(entity_id, hours=24, samples=12):
    """Histórico de una entidad (HA history API): N muestras equiespaciadas.

    samples=12 -> una muestra cada 2h en ventana de 24h (para la gráfica del panel).
    Devuelve lista de floats (estado numérico) o [] si no hay datos.
    """
    import datetime
    end = datetime.datetime.now().astimezone()
    start = end - datetime.timedelta(hours=hours)
    url = (f"{HA_URL}/api/history/period/{start.isoformat()}"
           f"?filter_entity_id={entity_id}&minimal_response&no_attributes")
    req = urllib.request.Request(url, headers={"Authorization": f"Bearer {HA_TOKEN}"})
    with urllib.request.urlopen(req, timeout=15) as resp:
        data = json.loads(resp.read())

    # data es lista de series (una por entidad); cada serie es lista de estados
    points = []
    for series in data:
        for st in series:
            try:
                v = float(st.get("state"))
            except (TypeError, ValueError):
                continue
            points.append((st.get("last_changed", ""), v))
    if not points:
        return []

    # Muestreo equiespaciado sobre la ventana temporal
    t0 = datetime.datetime.fromisoformat(points[0][0]).timestamp()
    t1 = datetime.datetime.fromisoformat(points[-1][0]).timestamp()
    span = max(t1 - t0, 1.0)
    out = []
    for i in range(samples):
        target = t0 + span * i / (samples - 1)
        best = min(points, key=lambda p: abs(datetime.datetime.fromisoformat(p[0]).timestamp() - target))
        out.append(round(best[1], 1))
    return out


def get_home():
    now = time.time()
    with _lock:
        if _cache["home"] and (now - _cache["home_ts"]) < CACHE_TTL:
            return _cache["home"]

    rooms = []
    for name, t_entity, h_entity in HOME_SENSORS:
        try:
            t = ha_get_state(t_entity)
            h = ha_get_state(h_entity)
            history = ha_get_history(t_entity, hours=24, samples=12)
            rooms.append({
                "name": name,
                "temperature": {"value": t["state"], "unit": t["unit"]},
                "humidity": {"value": h["state"], "unit": h["unit"]},
                "history": history,
            })
        except Exception as e:
            rooms.append({"name": name, "error": str(e), "history": []})

    with _lock:
        _cache["home"] = {"rooms": rooms, "updated": time.strftime("%H:%M:%S")}
        _cache["home_ts"] = now
    return _cache["home"]


# ---------------------------------------------------------------- HTTP
class Handler(BaseHTTPRequestHandler):
    def _send(self, code, obj):
        body = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Access-Control-Allow-Origin", "*")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/health":
            self._send(200, {"status": "ok"})
        elif self.path == "/preview":
            self._serve_preview()
        elif self.path.startswith("/logos/"):
            self._serve_logo(self.path[len("/logos/"):])
        elif self.path == "/api/news":
            self._send(200, {"feeds": get_news(), "updated": time.strftime("%H:%M:%S")})
        elif self.path == "/api/home":
            self._send(200, get_home())
        elif self.path == "/api/todos":
            self._send(200, {"todos": load_todos()})
        elif self.path == "/api/dashboard":
            self._send(200, {
                "todos": load_todos(),
                "home": get_home(),
                "news": {"feeds": get_news(), "updated": time.strftime("%H:%M:%S")},
                "updated": time.strftime("%H:%M:%S"),
            })
        else:
            self._send(404, {"error": "not found"})

    def do_POST(self):
        if self.path == "/api/todos":
            length = int(self.headers.get("Content-Length", 0))
            try:
                body = json.loads(self.rfile.read(length) or b"{}")
                text = (body.get("text") or "").strip()
                if not text:
                    self._send(400, {"error": "text required"})
                    return
                todos = load_todos()
                todos.append({"text": text, "done": False, "created": time.strftime("%Y-%m-%d %H:%M")})
                save_todos(todos)
                self._send(200, {"todos": todos})
            except Exception as e:
                self._send(400, {"error": str(e)})
        else:
            self._send(404, {"error": "not found"})

    def do_PATCH(self):
        if self.path.startswith("/api/todos/"):
            try:
                idx = int(self.path.rsplit("/", 1)[1])
                length = int(self.headers.get("Content-Length", 0))
                body = json.loads(self.rfile.read(length) or b"{}")
                todos = load_todos()
                if 0 <= idx < len(todos):
                    if "done" in body:
                        todos[idx]["done"] = bool(body["done"])
                    save_todos(todos)
                    self._send(200, {"todos": todos})
                else:
                    self._send(404, {"error": "index out of range"})
            except Exception as e:
                self._send(400, {"error": str(e)})
        else:
            self._send(404, {"error": "not found"})

    def do_DELETE(self):
        if self.path.startswith("/api/todos/"):
            try:
                idx = int(self.path.rsplit("/", 1)[1])
                todos = load_todos()
                if 0 <= idx < len(todos):
                    del todos[idx]
                    save_todos(todos)
                    self._send(200, {"todos": todos})
                else:
                    self._send(404, {"error": "index out of range"})
            except Exception as e:
                self._send(400, {"error": str(e)})
        else:
            self._send(404, {"error": "not found"})

    def _serve_preview(self):
        preview_path = os.environ.get("PREVIEW_FILE", "/app/preview.html")
        try:
            with open(preview_path, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
            self.send_header("Pragma", "no-cache")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception:
            self._send(500, {"error": "preview.html not found"})

    def _serve_logo(self, name):
        """Logos de proveedores de noticias (PNG). Ruta: /logos/<feed>.png"""
        import os.path
        safe = os.path.basename(name)
        path = os.path.join(os.environ.get("LOGOS_DIR", "/app/logos"), safe)
        if not safe.endswith(".png") or not os.path.isfile(path):
            self._send(404, {"error": "logo not found"})
            return
        try:
            with open(path, "rb") as f:
                body = f.read()
            self.send_response(200)
            self.send_header("Content-Type", "image/png")
            self.send_header("Cache-Control", "max-age=86400")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except Exception:
            self._send(500, {"error": "logo read failed"})

    def log_message(self, fmt, *args):
        pass  # silenciar logs


if __name__ == "__main__":
    os.makedirs(os.path.dirname(TODOS_FILE), exist_ok=True)
    port = int(os.environ.get("PORT", "18100"))
    server = ThreadingHTTPServer(("0.0.0.0", port), Handler)
    print(f"DoveBox Aggregator listening on :{port}")
    server.serve_forever()
