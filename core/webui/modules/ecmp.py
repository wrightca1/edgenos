"""ECMP — which interfaces participate in multipath routes, and the ECMP routes."""
import util

ID = "ecmp"
TITLE = "ECMP"
ICON = "⑂"
ORDER = 20


def _ecmp():
    """Return (ifaces_in_ecmp:set, routes:list[(dst,[(via,dev)...])])."""
    routes = util.json_cmd(["ip", "-j", "route"]) or []
    ifaces, ecmp = set(), []
    for r in routes:
        nh = r.get("nexthops")
        if nh and len(nh) > 1:
            legs = [(n.get("gateway", "-"), n.get("dev", "-")) for n in nh]
            ecmp.append((r.get("dst", "?"), legs))
            for _, dev in legs:
                ifaces.add(dev)
    return ifaces, ecmp


def page(req):
    ecmp_ifaces, routes = _ecmp()
    links = util.json_cmd(["ip", "-j", "addr"]) or []
    if_rows = []
    for d in links:
        name = d.get("ifname")
        if name == "lo":
            continue
        badge = '<span class="badge ecmp">ECMP</span>' if name in ecmp_ifaces else '<span class=sub>—</span>'
        if_rows.append("<tr><td><b>%s</b></td><td>%s</td></tr>" % (util.h(name), badge))

    r_rows = []
    for dst, legs in routes:
        r_rows.append("<tr><td>%s</td><td>%d</td><td>%s</td></tr>" % (
            util.h(dst), len(legs),
            util.h(", ".join("%s via %s" % (dev, via) for via, dev in legs))))

    return """
<h1>ECMP</h1><p class=sub>Equal-cost multipath: interfaces carrying load-balanced routes, and the multipath routes themselves.</p>
<div class=card><h2 style="margin-top:0">Interfaces in ECMP</h2>
<table><tr><th>Interface</th><th>ECMP</th></tr>%s</table></div>
<div class=card><h2 style="margin-top:0">ECMP routes</h2>
<table><tr><th>Destination</th><th>Paths</th><th>Next hops</th></tr>%s</table></div>
""" % ("".join(if_rows),
       "".join(r_rows) or "<tr><td colspan=3 class=sub>no multipath routes</td></tr>")
