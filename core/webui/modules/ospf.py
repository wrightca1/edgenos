"""OSPF — only present when ospfd is. Shows neighbors/routes; configures via the
vty (live, no daemon restart — restarting ospfd is known to break egress)."""
import os
import re
import util

ID = "ospf"
TITLE = "OSPF"
ICON = "◆"
ORDER = 30
VTY = 2604


def detect(ctx):
    if util.vty_available(VTY):
        return True
    return any(os.path.exists(p) for p in
               ("/etc/quagga/ospfd.conf", "/etc/frr/ospfd.conf"))


def _pw(req):
    return req.ctx.conf.get("ospf_vty_password", "zebra")


_CACHE = {"t": 0, "v": {}}


def _show(req, cmds):
    # vty round-trips are slow; cache the parsed output briefly so reloads are snappy.
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
    # strip the echoed command + the trailing prompt line
    lines = [l for l in s.replace("\r", "").split("\n")]
    lines = [l for l in lines if not re.match(r"^\S+(\(config[^)]*\))?[>#]\s*$", l.strip())]
    return "\n".join(l for l in lines).strip()


def _neighbors(text):
    """Parse `show ip ospf neighbor` into rows."""
    rows = []
    for line in text.splitlines():
        # Neighbor ID  Pri  State        Dead Time  Address     Interface
        m = re.match(r"^(\d+\.\d+\.\d+\.\d+)\s+(\d+)\s+(\S+)\s+(\S+)\s+(\d+\.\d+\.\d+\.\d+)\s+(\S+)", line.strip())
        if m:
            rows.append(m.groups())
    return rows


def _routes(text):
    """Parse `show ip ospf route` -> [{net,cost,area,vias:[(nexthop,iface)]}]."""
    rows, cur = [], None
    for line in text.splitlines():
        m = re.match(r"^([NR])\s+(\S+)\s+\[(\d+)\]\s+area:\s+(\S+)", line.strip())
        if m:
            cur = {"net": m.group(2), "cost": m.group(3), "area": m.group(4), "vias": []}
            rows.append(cur)
            continue
        v = re.match(r"^via\s+(\S+),\s+(\S+)", line.strip())
        if v and cur is not None:
            cur["vias"].append((v.group(1), v.group(2)))
    return rows


def page(req):
    d = _show(req, ["show ip ospf", "show ip ospf route", "show ip ospf neighbor",
                    "show running-config"])
    summary = d.get("show ip ospf", "")
    rid = ""
    m = re.search(r"Router ID:?\s*(\d+\.\d+\.\d+\.\d+)", summary)
    if m:
        rid = m.group(1)

    nbrs = _neighbors(d.get("show ip ospf neighbor", ""))
    nb_rows = "".join(
        "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>" %
        (util.h(n[0]), util.h(n[2]), util.h(n[3]), util.h(n[4]), util.h(n[5])) for n in nbrs
    ) or "<tr><td colspan=5 class=sub>no neighbors</td></tr>"

    # running ospf config (just the 'router ospf' block)
    rc = d.get("show running-config", "")
    block = []
    grab = False
    for l in rc.splitlines():
        if l.startswith("router ospf"):
            grab = True
        elif grab and l and not l.startswith(" "):
            grab = False
        if grab:
            block.append(l)
    ospf_cfg = "\n".join(block) or "(no router ospf block)"

    # learned routes (which router/nexthop each network was learned from)
    routes = _routes(d.get("show ip ospf route", ""))
    rt_rows = []
    for r in routes:
        paths = "<br>".join("%s via %s" % (util.h(dev), util.h(nh)) for nh, dev in r["vias"]) \
            or "<span class=sub>(local)</span>"
        ecmp = ' <span class="badge ecmp">ECMP</span>' if len(r["vias"]) > 1 else ""
        rt_rows.append("<tr><td>%s%s</td><td>%s</td><td>%s</td><td>%s</td></tr>" %
                       (util.h(r["net"]), ecmp, util.h(r["cost"]), util.h(r["area"]), paths))
    rt_html = "".join(rt_rows) or "<tr><td colspan=4 class=sub>no OSPF routes</td></tr>"

    return """
<h1>OSPF</h1><p class=sub>Router ID: <b>%s</b> · live via ospfd vty (config changes apply without restart)</p>

<div class=card><h2 style="margin-top:0">Neighbors</h2>
<table><tr><th>Neighbor</th><th>State</th><th>Dead</th><th>Address</th><th>Interface</th></tr>%s</table></div>

<div class=card><h2 style="margin-top:0">Learned routes</h2>
<table><tr><th>Network</th><th>Cost</th><th>Area</th><th>Learned via (router / interface)</th></tr>%s</table></div>

<div class=card><h2 style="margin-top:0">Add network to OSPF</h2>
<form method=post action="/m/ospf">
  <input type=hidden name=action value=add_network>
  network <input name=network placeholder="10.101.101.0/29" size=16>
  area <input name=area value="0" size=4>
  <button>add</button>
</form>
<p class=sub style="margin:8px 0 0">Applies live (<code>configure terminal → network … area … → write memory</code>); no daemon restart.</p>
</div>

<div class=card><h2 style="margin-top:0">Running OSPF config</h2><pre>%s</pre></div>
<div class=card><h2 style="margin-top:0">Status</h2><pre>%s</pre></div>
""" % (util.h(rid or "?"), nb_rows, rt_html, util.h(ospf_cfg), util.h(summary or "(no data)"))


def handle(req):
    f = req.form
    if f.get("action") == "add_network":
        net = f.get("network", "").strip()
        area = f.get("area", "0").strip() or "0"
        if not re.match(r"^\d+\.\d+\.\d+\.\d+/\d+$", net):
            return "msg=invalid network (use CIDR e.g. 10.0.0.0/24)"
        out, err = util.vty(VTY, _pw(req),
                            ["enable", "configure terminal", "router ospf",
                             "network %s area %s" % (net, area), "end", "write memory"])
        if err:
            return "msg=%s" % err
        _CACHE["t"] = 0     # force re-query so the change shows immediately
        return "msg=added network %s area %s (live)" % (net, area)
    return "msg=unknown action"
