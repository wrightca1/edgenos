"""L2 Switch — bridge selected ports into one L2 broadcast domain (dumb switch),
or leave them as their own L3 interfaces. Drives the `edgenos l2` CLI (which writes
the daemon's l2-groups.conf and SIGHUPs it for live apply)."""
import util

ID = "vlan"
TITLE = "L2 Switch"
ICON = "⧉"
ORDER = 25


def detect(ctx):
    # Only when a datapath daemon that supports L2 groups is running.
    for d in ("edged", "bcmd"):
        if util.run(["pgrep", "-x", d]).strip():
            return True
    return False


def _ports():
    links = util.json_cmd(["ip", "-j", "link"]) or []
    return [d["ifname"] for d in links
            if d.get("ifname", "").startswith(("swp", "ge", "xe"))]


def _groups():
    """Parse `edgenos l2 show` -> {vid: [ports]} and the in-group port set."""
    groups, in_group = {}, set()
    for line in util.run(["edgenos", "l2", "show"]).splitlines():
        line = line.strip()
        if line.startswith("VLAN"):
            # "VLAN 100  ⇄  swp10 swp11"
            parts = line.replace("⇄", " ").split()
            if len(parts) >= 2:
                vid = parts[1]
                ports = parts[2:]
                groups[vid] = ports
                in_group.update(ports)
    return groups, in_group


def page(req):
    ports = _ports()
    groups, in_group = _groups()

    grp_rows = "".join(
        "<tr><td>VLAN %s</td><td>%s</td>"
        "<td><form class=inline method=post action=\"/m/vlan\">"
        "<input type=hidden name=action value=del><input type=hidden name=vid value=\"%s\">"
        "<button class=sec>remove</button></form></td></tr>"
        % (util.h(v), util.h(" ".join(p)), util.h(v)) for v, p in sorted(groups.items())
    ) or "<tr><td colspan=3 class=sub>none — every port is its own L3 interface</td></tr>"

    checks = "".join(
        '<label style="display:inline-block;margin:2px 10px 2px 0">'
        '<input type=checkbox name="port_%s"%s> %s%s</label>'
        % (util.h(p), "", util.h(p),
           ' <span class="badge ecmp">grouped</span>' if p in in_group else "")
        for p in ports) or "<span class=sub>no datapath ports</span>"

    return """
<h1>L2 Switch</h1><p class=sub>Bridge a set of ports into one L2 broadcast domain
(they switch directly, like a dumb switch) instead of each being its own L3 interface.
Applied live via the datapath daemon — no reboot.</p>

<div class=card><h2 style="margin-top:0">Current L2 groups</h2>
<table><tr><th>VLAN</th><th>Bridged ports</th><th></th></tr>%s</table>
<form class=inline method=post action="/m/vlan" style="margin-top:10px">
  <input type=hidden name=action value=clear>
  <button class=danger>Clear all (every port back to L3)</button></form></div>

<div class=card><h2 style="margin-top:0">Create / replace an L2 group</h2>
<form method=post action="/m/vlan">
  <input type=hidden name=action value=set>
  VLAN <input name=vid placeholder="100" size=5> &nbsp; pick ports:<br>
  <div style="margin:10px 0">%s</div>
  <button>Bridge selected ports</button>
</form>
<p class=sub style="margin:8px 0 0">Selected ports join VLAN N untagged and L2-bridge
to each other; they're pulled off their per-port L3 service VLANs until cleared.</p>
</div>
""" % (grp_rows, checks)


def handle(req):
    f = req.form
    action = f.get("action")
    if action == "clear":
        util.run_rc(["edgenos", "l2", "clear"])
        return "msg=cleared all L2 groups — every port back to L3"
    if action == "del":
        vid = f.get("vid", "").strip()
        if vid:
            util.run_rc(["edgenos", "l2", "del", vid])
        return "msg=removed L2 group VLAN %s" % vid
    if action == "set":
        vid = f.get("vid", "").strip()
        ports = [k[len("port_"):] for k in f if k.startswith("port_")]
        if not vid or not ports:
            return "msg=pick a VLAN id and at least one port"
        rc, out = util.run_rc(["edgenos", "l2", "set", vid] + ports)
        return "msg=bridged %s into VLAN %s" % (" ".join(ports), vid) if rc == 0 \
            else "msg=failed: %s" % out.strip()
    return "msg=unknown action"
