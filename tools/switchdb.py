#!/usr/bin/env python3
"""
EdgeNOS switch database — loader, validator, resolver.

The switch DB (switchdb/) is the single source of truth. A *platform* entry
references an *arch* entry and an *asic* entry by id; resolve() merges the three
into one view that the version system, package system, and image builder consume.

Library use:
    from switchdb import SwitchDB
    db = SwitchDB()
    db.validate()                       # raises on schema / referential errors
    view = db.resolve("arm-accton-as4610-54-r0")
    print(view["arch"]["triple"], view["asic"]["sdk"])
"""
import os, sys, glob, json
import yaml
try:
    import jsonschema
except ImportError:
    jsonschema = None

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                       # edgenos/
DB   = os.path.join(ROOT, "switchdb")


class DBError(Exception):
    pass


def _load_yaml(path):
    with open(path, encoding="utf-8") as f:
        doc = yaml.safe_load(f)
    if not isinstance(doc, dict):
        raise DBError(f"{path}: expected a YAML mapping")
    return doc


class SwitchDB:
    def __init__(self, db_dir=DB):
        self.db_dir = db_dir
        self.arch, self.asic, self.platforms = {}, {}, {}
        self._schema = {}
        self._load()

    # ---------------------------------------------------------------- load
    def _load(self):
        for kind, store, key in (("arch", self.arch, "id"),
                                 ("asic", self.asic, "id"),
                                 ("platforms", self.platforms, "onie_platform")):
            for path in sorted(glob.glob(os.path.join(self.db_dir, kind, "*.yml"))):
                doc = _load_yaml(path)
                doc["_path"] = os.path.relpath(path, ROOT)
                k = doc.get(key)
                if not k:
                    raise DBError(f"{path}: missing required key '{key}'")
                if k in store:
                    raise DBError(f"duplicate {kind} '{k}' ({path} and {store[k]['_path']})")
                store[k] = doc
        for s in ("arch", "asic", "platform"):
            p = os.path.join(self.db_dir, "schema", f"{s}.schema.json")
            if os.path.exists(p):
                with open(p, encoding="utf-8") as f:
                    self._schema[s] = json.load(f)

    # ------------------------------------------------------------ validate
    def validate(self):
        """Schema-validate every entry and check referential integrity."""
        errors = []
        if jsonschema:
            for kind, store in (("arch", self.arch), ("asic", self.asic),
                                ("platform", self.platforms)):
                schema = self._schema.get(kind)
                if not schema:
                    continue
                for k, doc in store.items():
                    payload = {x: v for x, v in doc.items() if x != "_path"}
                    try:
                        jsonschema.validate(payload, schema)
                    except jsonschema.ValidationError as e:
                        errors.append(f"{doc['_path']}: {e.message} (at {'/'.join(map(str, e.path)) or '<root>'})")
        else:
            errors.append("WARNING: jsonschema not installed; skipped schema validation")
        # referential integrity: every platform's arch/asic must exist
        for k, p in self.platforms.items():
            if p.get("arch") not in self.arch:
                errors.append(f"{p['_path']}: arch '{p.get('arch')}' not found in switchdb/arch/")
            if p.get("asic") not in self.asic:
                errors.append(f"{p['_path']}: asic '{p.get('asic')}' not found in switchdb/asic/")
        if errors:
            raise DBError("switch DB validation failed:\n  - " + "\n  - ".join(errors))
        return True

    # ------------------------------------------------------------- resolve
    def resolve(self, onie_platform):
        """Return a merged view: {platform, arch, asic, datapath, kernel_modules}."""
        p = self.platforms.get(onie_platform)
        if not p:
            raise DBError(f"unknown platform '{onie_platform}'. Known: {', '.join(sorted(self.platforms))}")
        arch = self.arch.get(p["arch"])
        asic = self.asic.get(p["asic"])
        if not arch or not asic:
            raise DBError(f"{p['_path']}: dangling arch/asic reference; run validate()")
        return {
            "platform": p,
            "arch": arch,
            "asic": asic,
            # platform.datapath overrides asic.datapath default
            "datapath": p.get("datapath") or asic.get("datapath"),
            "kernel_modules": asic.get("kernel_modules", []),
        }

    def list_platforms(self):
        return sorted(self.platforms)


# ----------------------------------------------------------------- CLI glue
def _cmd_list(db, _args):
    for k in db.list_platforms():
        p = db.platforms[k]
        print(f"{k:34} {p.get('arch'):8} {p.get('asic'):10} {p.get('status','?')}")

def _cmd_validate(db, _args):
    db.validate()
    print(f"OK — {len(db.platforms)} platforms, {len(db.arch)} archs, {len(db.asic)} asics validated.")

def _cmd_show(db, args):
    if not args:
        raise SystemExit("usage: switchdb show <onie_platform>")
    view = db.resolve(args[0])
    print(yaml.safe_dump({k: {x: v for x, v in d.items() if x != "_path"} if isinstance(d, dict) else d
                          for k, d in view.items()}, sort_keys=False))

def main(argv):
    cmds = {"list": _cmd_list, "validate": _cmd_validate, "show": _cmd_show}
    if len(argv) < 2 or argv[1] not in cmds:
        raise SystemExit(f"usage: switchdb {{{'|'.join(cmds)}}} [args]")
    db = SwitchDB()
    try:
        cmds[argv[1]](db, argv[2:])
    except DBError as e:
        raise SystemExit(f"error: {e}")

if __name__ == "__main__":
    main(sys.argv)
