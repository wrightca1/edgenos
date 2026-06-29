#!/usr/bin/env python3
"""
EdgeNOS web UI — lightweight, modular, stdlib-only.

Modular: each feature is a module under modules/ that declares detect(); the nav
only shows modules whose detect() is true on THIS box (OSPF page appears only if
OSPF is present, etc.). The Apps module lets you install features not present yet.

Security: binds to the management interface's IP only. Set allow_all=1 (or list
interfaces) in /etc/edgenos/webui.conf to expose it elsewhere.

Config (/etc/edgenos/webui.conf, key=value):
    port=8090
    interface=            # mgmt iface name; empty = auto-detect
    allow_all=0           # 1 => bind 0.0.0.0 (explicit opt-in)
    ospf_vty_password=zebra
"""
import os
import sys
import glob
import importlib.util
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import util  # noqa: E402

CONF_PATH = os.environ.get("EDGENOS_WEBUI_CONF", "/etc/edgenos/webui.conf")
MGMT_CANDIDATES = ["ma1", "eth0", "end0", "mgmt0", "eth1"]


def load_conf():
    conf = {"port": "8090", "interface": "", "allow_all": "0",
            "ospf_vty_password": "zebra"}
    try:
        for line in open(CONF_PATH, encoding="utf-8"):
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                k, _, v = line.partition("=")
                conf[k.strip()] = v.strip()
    except OSError:
        pass
    return conf


def iface_ipv4(iface):
    data = util.json_cmd(["ip", "-j", "-4", "addr", "show", "dev", iface])
    if not data:
        return None
    for d in data:
        for a in d.get("addr_info", []):
            if a.get("family") == "inet":
                return a.get("local")
    return None


def mgmt_bind(conf):
    """Return (bind_ip, mgmt_iface). Honors allow_all and an explicit interface."""
    if conf.get("allow_all") == "1":
        return "0.0.0.0", "(all interfaces)"
    cand = [conf["interface"]] if conf.get("interface") else MGMT_CANDIDATES
    for iface in cand:
        ip = iface_ipv4(iface)
        if ip:
            return ip, iface
    return "127.0.0.1", "(no mgmt iface found — localhost only)"


# ----------------------------------------------------------------- modules
class Module:
    def __init__(self, mod, path):
        self.mod = mod
        self.id = getattr(mod, "ID")
        self.title = getattr(mod, "TITLE", self.id)
        self.order = getattr(mod, "ORDER", 100)
        self.icon = getattr(mod, "ICON", "•")
        self.path = path

    def detect(self, ctx):
        fn = getattr(self.mod, "detect", None)
        try:
            return True if fn is None else bool(fn(ctx))
        except Exception:                                   # noqa: BLE001
            return False


def load_modules():
    mods = []
    for f in sorted(glob.glob(os.path.join(HERE, "modules", "*.py"))):
        if os.path.basename(f).startswith("_"):
            continue
        spec = importlib.util.spec_from_file_location("mod_" + os.path.basename(f)[:-3], f)
        m = importlib.util.module_from_spec(spec)
        try:
            spec.loader.exec_module(m)
            if hasattr(m, "ID"):
                mods.append(Module(m, f))
        except Exception as e:                              # noqa: BLE001
            sys.stderr.write("webui: failed to load %s: %s\n" % (f, e))
    mods.sort(key=lambda x: (x.order, x.title))
    return mods


# ----------------------------------------------------------------- request ctx
class Ctx:
    def __init__(self, conf):
        self.conf = conf
        self.util = util
        self.identity = util.json_cmd(["cat", "/etc/edgenos/version.json"]) or {}

    # convenience proxies
    def run(self, *a, **k):       return util.run(*a, **k)
    def run_rc(self, *a, **k):    return util.run_rc(*a, **k)
    def json_cmd(self, *a, **k):  return util.json_cmd(*a, **k)


class Req:
    def __init__(self, ctx, method, path, query, form):
        self.ctx = ctx
        self.method = method
        self.path = path
        self.query = query
        self.form = form


# ----------------------------------------------------------------- HTML shell
CSS = """
*{box-sizing:border-box} body{margin:0;font:14px/1.5 system-ui,Segoe UI,Roboto,sans-serif;color:#1c2330;background:#f4f6fb}
a{color:#2563eb;text-decoration:none} a:hover{text-decoration:underline}
.layout{display:flex;min-height:100vh}
.side{width:210px;background:#0f172a;color:#cbd5e1;flex:0 0 210px;padding:0}
.brand{padding:16px 18px;font-weight:700;color:#fff;font-size:16px;border-bottom:1px solid #1e293b}
.brand small{display:block;font-weight:400;color:#64748b;font-size:11px;margin-top:2px}
.nav a{display:block;padding:10px 18px;color:#cbd5e1;border-left:3px solid transparent}
.nav a:hover{background:#1e293b;text-decoration:none}
.nav a.active{background:#1e293b;color:#fff;border-left-color:#3b82f6}
.main{flex:1;padding:24px 30px;max-width:1100px}
h1{font-size:22px;margin:0 0 4px} h2{font-size:16px;margin:24px 0 8px}
.sub{color:#64748b;margin:0 0 20px}
.card{background:#fff;border:1px solid #e2e8f0;border-radius:8px;padding:16px 18px;margin:0 0 16px}
table{border-collapse:collapse;width:100%} th,td{text-align:left;padding:7px 10px;border-bottom:1px solid #eef2f7;font-size:13px}
th{color:#64748b;font-weight:600;font-size:12px;text-transform:uppercase;letter-spacing:.03em}
.badge{display:inline-block;padding:1px 8px;border-radius:10px;font-size:12px;font-weight:600}
.up{background:#dcfce7;color:#166534}.down{background:#fee2e2;color:#991b1b}.muted{background:#e2e8f0;color:#475569}
.ecmp{background:#dbeafe;color:#1e40af}
input,select{padding:6px 8px;border:1px solid #cbd5e1;border-radius:6px;font:inherit}
button{padding:6px 12px;border:0;border-radius:6px;background:#2563eb;color:#fff;font:inherit;cursor:pointer}
button:hover{background:#1d4ed8} button.sec{background:#64748b} button.danger{background:#dc2626}
pre{background:#0f172a;color:#e2e8f0;padding:12px;border-radius:6px;overflow:auto;font-size:12px}
.flash{background:#ecfdf5;border:1px solid #a7f3d0;color:#065f46;padding:10px 14px;border-radius:6px;margin:0 0 16px}
.kv{display:grid;grid-template-columns:160px 1fr;gap:4px 12px} .kv div:nth-child(odd){color:#64748b}
form.inline{display:inline}
"""


def page_html(ctx, mods, active_id, title, body, flash=None):
    nav = []
    for m in mods:
        cls = "active" if m.id == active_id else ""
        nav.append('<a class="%s" href="/m/%s">%s %s</a>' % (cls, m.id, m.icon, util.h(m.title)))
    ident = ctx.identity
    brand_sub = util.h("%s · %s" % (ident.get("platform", "EdgeNOS"), ident.get("version", "")))
    fl = '<div class="flash">%s</div>' % util.h(flash) if flash else ""
    return """<!doctype html><html><head><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>EdgeNOS — %s</title><style>%s</style></head><body><div class=layout>
<div class=side><div class=brand>EdgeNOS<small>%s</small></div><div class=nav>%s</div></div>
<div class=main>%s%s</div></div></body></html>""" % (
        util.h(title), CSS, brand_sub, "".join(nav), fl, body)


# ----------------------------------------------------------------- HTTP handler
class Handler(BaseHTTPRequestHandler):
    server_version = "EdgeNOS-webui"

    def log_message(self, *a):
        pass

    def _modules(self):
        ctx = self.server.ctx
        all_mods = self.server.modules
        shown = [m for m in all_mods if m.detect(ctx)]
        return ctx, all_mods, shown

    def _send(self, body, code=200, ctype="text/html; charset=utf-8"):
        b = body.encode("utf-8") if isinstance(body, str) else body
        try:
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)
        except (BrokenPipeError, ConnectionError):
            pass        # client navigated away / gave up before we finished

    def _redirect(self, path):
        self.send_response(303)
        self.send_header("Location", path)
        self.end_headers()

    def do_GET(self):
        ctx, _all, shown = self._modules()
        parsed = urllib.parse.urlparse(self.path)
        query = dict(urllib.parse.parse_qsl(parsed.query))
        path = parsed.path
        if path in ("/", ""):
            if shown:
                return self._redirect("/m/" + shown[0].id)
            return self._send("no modules")
        if path.startswith("/m/"):
            mid = path[3:].strip("/")
            m = next((x for x in shown if x.id == mid), None)
            if not m:
                return self._send(page_html(ctx, shown, None, "Not found",
                                            "<h1>Not available</h1><p class=sub>No such module on this box.</p>"), 404)
            req = Req(ctx, "GET", path, query, {})
            try:
                body = m.mod.page(req)
            except Exception as e:                          # noqa: BLE001
                body = "<h1>%s</h1><pre>error: %s</pre>" % (util.h(m.title), util.h(e))
            flash = query.get("msg")
            return self._send(page_html(ctx, shown, m.id, m.title, body, flash))
        return self._send("not found", 404)

    def do_POST(self):
        ctx, _all, shown = self._modules()
        parsed = urllib.parse.urlparse(self.path)
        path = parsed.path
        length = int(self.headers.get("Content-Length", 0) or 0)
        raw = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        form = dict(urllib.parse.parse_qsl(raw))
        if path.startswith("/m/"):
            mid = path[3:].strip("/")
            m = next((x for x in shown if x.id == mid), None)
            if m and hasattr(m.mod, "handle"):
                req = Req(ctx, "POST", path, {}, form)
                try:
                    res = m.mod.handle(req)
                except Exception as e:                      # noqa: BLE001
                    res = "msg=error: %s" % e
                # handle() returns a query string for the flash/redirect
                q = ("?" + res) if res else ""
                return self._redirect("/m/%s%s" % (mid, q))
        return self._redirect("/")


def main(argv):
    conf = load_conf()
    ctx = Ctx(conf)
    mods = load_modules()
    bind_ip, mgmt = mgmt_bind(conf)
    port = int(conf.get("port", "8090"))
    httpd = ThreadingHTTPServer((bind_ip, port), Handler)
    httpd.ctx = ctx
    httpd.modules = mods
    shown = [m.id for m in mods if m.detect(ctx)]
    sys.stderr.write("EdgeNOS web UI on http://%s:%d  (mgmt iface: %s)\n" % (bind_ip, port, mgmt))
    sys.stderr.write("modules present: %s\n" % ", ".join(shown))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main(sys.argv)
