"""OSPFv3 (IPv6) — only present when ospf6d is. Shows neighbors/routes; configures
interfaces into the area via the vty (live, no daemon restart). Mirrors ospf.py."""
import os
import re
import util

ID = "ospf6"
TITLE = "OSPFv3"
ICON = "◇"
ORDER = 31
VTY = 2606


def detect(ctx):
    if util.vty_available(VTY):
        return True
    return any(os.path.exists(p) for p in
               ("/etc/quagga/ospf6d.conf", "/etc/frr/ospf6d.conf"))


def _pw(req):
    return req.ctx.conf.get("ospf_vty_password", "zebra")


_CACHE = {"t": 0, "v": {}}


def _show(req, cmds):
    import time
    if time.time() - _CACHE["t"] <= 6 and all(c in _CACHE["v"] for c in cmds):
        return _CACHE["v"]
    out, err = util.vty(VTY, _pw(req), cmds)
    if err:
        return {c: "(%s)" % err for c in cmds}
    data = {c: _clean(o) for c, o in zip(cmds, out)}
    _CACHE["v"] = data
    _CACHE["t"] = time.time()
    return data


def _clean(s):
    lines = [l for l in s.replace("\r", "").split("\n")]
    lines = [l for l in lines if not re.match(r"^\S+(\(config[^)]*\))?[>#]\s*$", l.strip())]
    return "\n".join(l for l in lines).strip()


def _neighbors(text):
    """Parse `show ipv6 ospf6 neighbor`:
       Neighbor ID  Pri  DeadTime  State/IfState  Duration  I/F[State]
       10.101.1.241  1   00:00:37  Full/DR        00:00:41  swp1[BDR]"""
    rows = []
    for line in text.splitlines():
        m = re.match(r"^(\d+\.\d+\.\d+\.\d+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)", line.strip())
        if m:
            rows.append(m.groups())   # rid, pri, dead, state, dur, iface
    return rows


def _routes(text):
    """Parse `show ipv6 ospf6 route`:
       *N IA 2001:db8::/64   fe80::...   swp1 00:24:12
       (type=IA/E1/E2/...; nexthop '::' = directly connected)."""
    rows = []
    for line in text.splitlines():
        m = re.match(r"^\*?[NRD]\s+(\S+)\s+(\S+)\s+(\S+)\s+(\S+)\s+\S+", line.strip())
        if m:
            typ, prefix, nh, iface = m.groups()
            rows.append({"type": typ, "net": prefix, "nh": nh, "iface": iface})
    return rows


def page(req):
    d = _show(req, ["show ipv6 ospf6", "show ipv6 ospf6 route",
                    "show ipv6 ospf6 neighbor", "show running-config"])
    summary = d.get("show ipv6 ospf6", "")
    rid = ""
    m = re.search(r"Router\s*ID:?\s*(\d+\.\d+\.\d+\.\d+)", summary)
    if m:
        rid = m.group(1)

    nbrs = _neighbors(d.get("show ipv6 ospf6 neighbor", ""))
    nb_rows = "".join(
        "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>" %
        (util.h(n[0]), util.h(n[3]), util.h(n[2]), util.h(n[5]), util.h(n[4])) for n in nbrs
    ) or "<tr><td colspan=5 class=sub>no neighbors</td></tr>"

    rc = d.get("show running-config", "")
    block, grab = [], False
    for l in rc.splitlines():
        if l.startswith("router ospf6"):
            grab = True
        elif grab and l and not l.startswith(" "):
            grab = False
        if grab:
            block.append(l)
    ospf_cfg = "\n".join(block) or "(no router ospf6 block)"

    routes = _routes(d.get("show ipv6 ospf6 route", ""))
    rt_rows = []
    for r in routes:
        via = "<span class=sub>(connected)</span>" if r["nh"] == "::" \
            else "%s via %s" % (util.h(r["iface"]), util.h(r["nh"]))
        rt_rows.append("<tr><td>%s</td><td>%s</td><td>%s</td></tr>" %
                       (util.h(r["net"]), util.h(r["type"]), via))
    rt_html = "".join(rt_rows) or "<tr><td colspan=3 class=sub>no OSPFv3 routes</td></tr>"

    return """
<h1>OSPFv3 <span class=sub style="font-size:14px">(IPv6)</span></h1>
<p class=sub>Router ID: <b>%s</b> · live via ospf6d vty (changes apply without restart).
v6 routes learned here are programmed into the chip's L3_DEFIP_128 table.</p>

<div class=card><h2 style="margin-top:0">Neighbors</h2>
<table><tr><th>Neighbor</th><th>State</th><th>Dead</th><th>Interface</th><th>Duration</th></tr>%s</table></div>

<div class=card><h2 style="margin-top:0">Learned routes</h2>
<table><tr><th>Prefix</th><th>Type</th><th>Learned via (interface / next-hop)</th></tr>%s</table></div>

<div class=card><h2 style="margin-top:0">Add interface to OSPFv3</h2>
<form method=post action="/m/ospf6">
  <input type=hidden name=action value=add_interface>
  interface <input name=iface placeholder="swp1" size=10>
  area <input name=area value="0.0.0.0" size=8>
  <button>add</button>
</form>
<p class=sub style="margin:8px 0 0">OSPFv3 assigns whole <em>interfaces</em> to an area
(it runs over each link's IPv6 link-local). Applies live; no daemon restart.</p>
</div>

<div class=card><h2 style="margin-top:0">Running OSPFv3 config</h2><pre>%s</pre></div>
<div class=card><h2 style="margin-top:0">Status</h2><pre>%s</pre></div>
""" % (util.h(rid or "?"), nb_rows, rt_html, util.h(ospf_cfg), util.h(summary or "(no data)"))


def handle(req):
    f = req.form
    if f.get("action") == "add_interface":
        iface = f.get("iface", "").strip()
        area = f.get("area", "0.0.0.0").strip() or "0.0.0.0"
        if not re.match(r"^[a-zA-Z0-9.]+$", iface):
            return "msg=invalid interface name"
        out, err = util.vty(VTY, _pw(req),
                            ["enable", "configure terminal", "router ospf6",
                             "interface %s area %s" % (iface, area), "end", "write memory"])
        if err:
            return "msg=%s" % err
        _CACHE["t"] = 0
        return "msg=added interface %s to area %s (live)" % (iface, area)
    return "msg=unknown action"
