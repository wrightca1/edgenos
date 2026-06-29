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


def _show(req, cmds):
    out, err = util.vty(VTY, _pw(req), cmds)
    if err:
        return {c: "(%s)" % err for c in cmds}
    return {c: _clean(o) for c, o in zip(cmds, out)}


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


def page(req):
    d = _show(req, ["show ip ospf", "show ip ospf neighbor",
                    "show ip ospf interface", "show running-config"])
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

    return """
<h1>OSPF</h1><p class=sub>Router ID: <b>%s</b> · live via ospfd vty (config changes apply without restart)</p>

<div class=card><h2 style="margin-top:0">Neighbors</h2>
<table><tr><th>Neighbor</th><th>State</th><th>Dead</th><th>Address</th><th>Interface</th></tr>%s</table></div>

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
""" % (util.h(rid or "?"), nb_rows, util.h(ospf_cfg), util.h(summary or "(no data)"))


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
        return "msg=added network %s area %s (live)" % (net, area)
    return "msg=unknown action"
