"""Interfaces — show state and configure IPv4 addresses. (ECMP lives on its own page.)"""
import util

ID = "interfaces"
TITLE = "Interfaces"
ICON = "⇄"
ORDER = 10

PERSIST = "/etc/edgenos/addrs.conf"     # simple record: "<iface> <cidr>" per line


def page(req):
    links = util.json_cmd(["ip", "-j", "addr"]) or []
    rows = []
    for d in links:
        name = d.get("ifname")
        if name == "lo":
            continue
        up = d.get("operstate") == "UP" or "UP" in d.get("flags", [])
        st = '<span class="badge up">up</span>' if up else '<span class="badge muted">down</span>'
        addr_html = []
        for a in d.get("addr_info", []):
            if a.get("family") != "inet":
                continue
            cidr = a["local"] + "/" + str(a["prefixlen"])
            addr_html.append(
                '%s <form class=inline method=post action="/m/interfaces">'
                '<input type=hidden name=action value=addr_del>'
                '<input type=hidden name=iface value="%s"><input type=hidden name=cidr value="%s">'
                '<button class="sec" style="padding:1px 6px;font-size:11px">×</button></form>'
                % (util.h(cidr), util.h(name), util.h(cidr)))
        rows.append(
            "<tr><td><b>%s</b></td><td>%s</td><td>%s</td>"
            "<td><form class=inline method=post action=\"/m/interfaces\">"
            "<input type=hidden name=action value=addr_add>"
            "<input type=hidden name=iface value=\"%s\">"
            "<input name=cidr placeholder=\"10.0.0.1/24\" size=14> <button>add</button></form></td></tr>"
            % (util.h(name), st, "<br>".join(addr_html) or "<span class=sub>—</span>", util.h(name)))

    return """
<h1>Interfaces</h1><p class=sub>Configure IPv4 addresses. Changes apply live and are recorded to %s.</p>
<div class=card><table>
  <tr><th>Interface</th><th>State</th><th>IPv4 addresses</th><th>Add address</th></tr>%s</table></div>
""" % (PERSIST, "".join(rows))


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
        return "msg=%s %s on %s" % ("added" if op == "add" else "removed", cidr, iface)
    return "msg=failed: %s" % (out.strip().splitlines()[-1] if out.strip() else "error")
