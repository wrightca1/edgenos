"""Interfaces — show state/addresses, configure IPs, and ECMP participation."""
import util

ID = "interfaces"
TITLE = "Interfaces"
ICON = "⇄"
ORDER = 10

PERSIST = "/etc/edgenos/addrs.conf"     # simple record: "<iface> <cidr>" per line


def _ecmp():
    """Return (ifaces_in_ecmp:set, ecmp_routes:list[(dst,[(via,dev)...])])."""
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
    links = util.json_cmd(["ip", "-j", "addr"]) or []
    ecmp_ifaces, ecmp_routes = _ecmp()
    mgmt = util.json_cmd(["cat", "/dev/null"])  # placeholder; mgmt shown via server bind

    rows = []
    for d in links:
        name = d.get("ifname")
        if name == "lo":
            continue
        up = d.get("operstate") == "UP" or "UP" in d.get("flags", [])
        st = '<span class="badge up">up</span>' if up else '<span class="badge muted">down</span>'
        addrs = [a["local"] + "/" + str(a["prefixlen"]) for a in d.get("addr_info", [])
                 if a.get("family") == "inet"]
        addr_html = []
        for a in addrs:
            addr_html.append(
                '%s <form class=inline method=post action="/m/interfaces">'
                '<input type=hidden name=action value=addr_del>'
                '<input type=hidden name=iface value="%s"><input type=hidden name=cidr value="%s">'
                '<button class="sec" style="padding:1px 6px;font-size:11px">×</button></form>'
                % (util.h(a), util.h(name), util.h(a)))
        ec = '<span class="badge ecmp">ECMP</span>' if name in ecmp_ifaces else ""
        rows.append(
            "<tr><td><b>%s</b></td><td>%s %s</td><td>%s</td>"
            "<td><form class=inline method=post action=\"/m/interfaces\">"
            "<input type=hidden name=action value=addr_add>"
            "<input type=hidden name=iface value=\"%s\">"
            "<input name=cidr placeholder=\"10.0.0.1/24\" size=14> <button>add</button></form></td></tr>"
            % (util.h(name), st, ec, "<br>".join(addr_html) or "<span class=sub>—</span>",
               util.h(name)))

    er = []
    for dst, legs in ecmp_routes:
        er.append("<tr><td>%s</td><td>%s</td></tr>" % (
            util.h(dst), util.h(", ".join("%s via %s" % (dev, via) for via, dev in legs))))
    ecmp_card = """
<div class=card><h2 style="margin-top:0">ECMP routes</h2>
  <table><tr><th>Destination</th><th>Paths</th></tr>%s</table></div>""" % (
        "".join(er) or "<tr><td colspan=2 class=sub>no multipath routes</td></tr>")

    return """
<h1>Interfaces</h1><p class=sub>Configure addresses and see ECMP participation. Changes apply live and are recorded to %s.</p>
<div class=card><table>
  <tr><th>Interface</th><th>State</th><th>ECMP</th><th>IPv4 addresses</th></tr>%s</table></div>
%s
""" % (PERSIST, "".join(rows), ecmp_card)


def _persist(iface, cidr, add):
    try:
        lines = []
        try:
            lines = [l.rstrip("\n") for l in open(PERSIST, encoding="utf-8")]
        except OSError:
            pass
        entry = "%s %s" % (iface, cidr)
        lines = [l for l in lines if l.strip() != entry]
        if add:
            lines.append(entry)
        with open(PERSIST, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + ("\n" if lines else ""))
    except OSError:
        pass


def handle(req):
    f = req.form
    action, iface, cidr = f.get("action"), f.get("iface", ""), f.get("cidr", "").strip()
    if action not in ("addr_add", "addr_del") or not iface or not cidr:
        return "msg=invalid request"
    op = "add" if action == "addr_add" else "del"
    rc, out = util.run_rc(["ip", "addr", op, cidr, "dev", iface])
    if rc == 0:
        _persist(iface, cidr, add=(op == "add"))
        return "msg=%s %s on %s" % (op == "add" and "added" or "removed", cidr, iface)
    return "msg=failed: %s" % out.strip().splitlines()[-1] if out.strip() else "msg=failed"
