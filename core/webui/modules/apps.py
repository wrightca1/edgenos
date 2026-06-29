"""Apps — what's installed, and install features not present yet (epkg .epk).
Installing a feature package makes its module/page appear in the nav."""
import os
import glob
import json
import util

ID = "apps"
TITLE = "Apps"
ICON = "＋"
ORDER = 90

CATALOG = "/var/lib/edgenos/available"      # drop .epk here to offer them for install
INSTALLED_DB = "/var/lib/edgenos/epkg/installed"


def _installed():
    names = {}
    try:
        for fn in os.listdir(INSTALLED_DB):
            if fn.endswith(".json"):
                try:
                    m = json.load(open(os.path.join(INSTALLED_DB, fn)))
                    names[m["name"]] = m
                except Exception:                           # noqa: BLE001
                    pass
    except OSError:
        pass
    return names


def page(req):
    inst = _installed()
    inst_rows = "".join(
        "<tr><td><b>%s</b></td><td>%s</td><td>%s-%s</td><td>%s</td></tr>" %
        (util.h(m["name"]), util.h(m["version"]), util.h(m["arch"]), util.h(m["asic"]),
         util.h(m.get("type", "")))
        for m in sorted(inst.values(), key=lambda x: x["name"])
    ) or "<tr><td colspan=4 class=sub>none</td></tr>"

    # available = catalog .epk whose package name isn't installed
    avail = []
    for p in sorted(glob.glob(os.path.join(CATALOG, "*.epk"))):
        nm = os.path.basename(p).split("_")[0]
        if nm not in inst:
            avail.append((nm, p))
    if avail:
        av_rows = "".join(
            "<tr><td><b>%s</b></td><td>%s</td><td>"
            "<form class=inline method=post action=\"/m/apps\">"
            "<input type=hidden name=action value=install><input type=hidden name=file value=\"%s\">"
            "<button>install</button></form></td></tr>"
            % (util.h(nm), util.h(os.path.basename(p)), util.h(p)) for nm, p in avail)
    else:
        av_rows = ('<tr><td colspan=3 class=sub>No additional packages in %s. '
                   'Copy feature .epk files there (e.g. a future <code>bgp</code> package) '
                   'to offer them here — installing one makes its page appear.</td></tr>' % util.h(CATALOG))

    return """
<h1>Apps</h1><p class=sub>Modular features. Pages appear automatically when their package is installed.</p>
<div class=card><h2 style="margin-top:0">Installed</h2>
<table><tr><th>Package</th><th>Version</th><th>Tag</th><th>Type</th></tr>%s</table></div>
<div class=card><h2 style="margin-top:0">Available to install</h2>
<table><tr><th>Package</th><th>File</th><th></th></tr>%s</table></div>
""" % (inst_rows, av_rows)


def handle(req):
    f = req.form
    if f.get("action") == "install":
        path = f.get("file", "")
        if not path or not os.path.exists(path):
            return "msg=package not found"
        rc, out = util.run_rc(["edgenos", "pkg", "install", path], timeout=60)
        last = out.strip().splitlines()[-1] if out.strip() else ""
        return "msg=%s" % (("installed: " + last) if rc == 0 else ("install failed: " + last))
    return "msg=unknown action"
