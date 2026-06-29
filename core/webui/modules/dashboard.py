"""Dashboard — identity, sensors (HAL), and what this image is made of."""
import util

ID = "dashboard"
TITLE = "Dashboard"
ICON = "▣"
ORDER = 0


_HAL_CACHE = {"t": 0, "v": {}}


def _hal():
    """HAL can be slow on boxes with flaky i2c; cache it briefly."""
    import time
    if time.time() - _HAL_CACHE["t"] > 20:
        _HAL_CACHE["v"] = util.json_cmd(["edgenos", "platform", "hal"], timeout=20) or {}
        _HAL_CACHE["t"] = time.time()
    return _HAL_CACHE["v"]


def page(req):
    ident = req.ctx.identity
    hal = _hal()
    pkgs = [l for l in util.run(["edgenos", "pkg", "list"]).splitlines() if l.strip()
            and "no packages" not in l]
    hostname = util.run(["hostname"]).strip()
    uptime = util.run(["uptime"]).strip()

    kv = "".join("<div>%s</div><div>%s</div>" % (k, util.h(v)) for k, v in [
        ("Hostname", hostname),
        ("Platform", ident.get("pretty_name") or ident.get("platform")),
        ("Version", ident.get("version_string") or ident.get("version")),
        ("Arch / ASIC", "%s / %s" % (ident.get("arch"), ident.get("asic"))),
        ("Kernel", ident.get("kernel")),
        ("Datapath", ident.get("datapath")),
    ])

    def sensor_rows():
        out = []
        for t in (hal.get("thermals") or [])[:12]:
            out.append("<tr><td>%s</td><td>%s °C</td></tr>" % (util.h(t.get("name")), util.h(t.get("celsius"))))
        return "".join(out) or "<tr><td colspan=2 class=sub>none</td></tr>"

    def psu_rows():
        out = []
        for p in (hal.get("psus") or []):
            if isinstance(p, dict):
                ok = p.get("ok")
                b = '<span class="badge up">OK</span>' if ok else '<span class="badge down">no power</span>'
                pr = "yes" if p.get("present") else "no"
                out.append("<tr><td>PSU%s</td><td>%s</td><td>%s</td></tr>" % (p.get("id"), pr, b))
        return "".join(out) or "<tr><td colspan=3 class=sub>n/a</td></tr>"

    fans = hal.get("fans")
    fan_txt = ""
    if isinstance(fans, list):
        fan_txt = ", ".join("fan%s %s%%/%srpm" % (f.get("id"), f.get("duty_pct"), f.get("rpm"))
                            for f in fans if isinstance(f, dict))
    sfps = hal.get("sfps")
    sfp_n = len(sfps) if isinstance(sfps, list) else 0

    return """
<h1>Dashboard</h1><p class=sub>%s</p>
<div class=card><div class=kv>%s</div></div>
<div class=card><h2 style="margin-top:0">Sensors</h2>
  <table><tr><th>Thermal</th><th>Temp</th></tr>%s</table>
  <p class=sub style="margin:10px 0 0">Fans: %s &nbsp;·&nbsp; Optics present: %d</p></div>
<div class=card><h2 style="margin-top:0">Power</h2>
  <table><tr><th>PSU</th><th>Present</th><th>Status</th></tr>%s</table></div>
<div class=card><h2 style="margin-top:0">Installed packages (%d)</h2>
  <pre>%s</pre></div>
""" % (util.h(uptime), kv, sensor_rows(), util.h(fan_txt or "n/a"), sfp_n,
       psu_rows(), len(pkgs), util.h("\n".join(pkgs)))
