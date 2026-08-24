#!/usr/bin/env python3
"""
EdgeNOS package format (.epk) — shared library used by both the host-side builder
(pkgbuild.py) and the on-box installer (epkg.py).

An .epk is a plain (uncompressed) tar container with exactly two members, in order:

    manifest.json     metadata (read without touching the payload)
    data.tar.gz       the gzip-compressed file payload (paths relative to rootfs root)

The payload is gzip-compressed AND sha256-hashed. The minimal Buildroot Python on
the switches has NO compression modules (no zlib/lzma), but the `gzip` CLI is present
on host and box — so epk.py compresses/decompresses via the `gzip` command (subprocess),
not a Python module. Builds are reproducible: `gzip -n` (no name/mtime), file
mtime/uid/gid normalized, members sorted, timestamps from an explicit epoch.

manifest.json schema (format_version 1):
    format_version      int
    name                str         package name
    version             str         semver
    arch                str         CPU arch id or "any"
    asic                str         switch-ASIC id or "any"
    type                str         "base" | "overlay"
    runtime_installable bool        may epkg install it on a live box?
    summary             str
    depends             [str]       other package names
    hooks               {preinst,postinst,prerm,postrm: str|null}
    files               [{path, mode, size, sha256}]   target paths (no leading /)
    links               [{path, target}]               symlinks (optional; format_version 1 additive)
    payload_sha256      str         sha256 of the data.tar.gz member
    build               {epoch, timestamp}
"""
import io, os, json, tarfile, hashlib, subprocess

FORMAT_VERSION = 1
MANIFEST_NAME = "manifest.json"
PAYLOAD_NAME = "data.tar.gz"
HOOK_NAMES = ("preinst", "postinst", "prerm", "postrm")


# Compress via the `gzip` CLI (present on host + the switches), not a Python module —
# the Buildroot Python on-box has no zlib/lzma. `-n` => reproducible (no name/mtime).
def _gzip(data):
    return subprocess.run(["gzip", "-nc"], input=data,
                          stdout=subprocess.PIPE, check=True).stdout


def _gunzip(data):
    return subprocess.run(["gzip", "-dc"], input=data,
                          stdout=subprocess.PIPE, check=True).stdout


def sha256_bytes(b):
    return hashlib.sha256(b).hexdigest()


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def _norm(ti, epoch):
    ti.uid = ti.gid = 0
    ti.uname = ti.gname = "root"
    ti.mtime = epoch
    return ti


def pkg_filename(manifest):
    tag = f"{manifest['arch']}-{manifest['asic']}"
    return f"{manifest['name']}_{manifest['version']}_{tag}.epk"


# ---------------------------------------------------------------- build side
def build_payload(staged, epoch, links=()):
    """staged: list of (dst_path_no_leading_slash, abs_src, mode_int).
    links:  list of (dst_path, symlink_target) — e.g. systemd *.wants enablement.
    Returns (payload_bytes, files_meta) — or (payload, files_meta, links_meta) when
    links were given. Deterministic."""
    files_meta, links_meta = [], []
    raw = io.BytesIO()
    # build an uncompressed tar, then gzip it via the CLI (returned payload is .gz)
    with tarfile.open(fileobj=raw, mode="w") as tar:
        for dst, src, mode in sorted(staged, key=lambda x: x[0]):
            ti = tarfile.TarInfo(name=dst.lstrip("/"))
            data = open(src, "rb").read()
            ti.size = len(data)
            ti.mode = mode
            _norm(ti, epoch)
            tar.addfile(ti, io.BytesIO(data))
            files_meta.append({
                "path": "/" + dst.lstrip("/"),
                "mode": "0%o" % mode,
                "size": len(data),
                "sha256": sha256_bytes(data),
            })
        for dst, target in sorted(links, key=lambda x: x[0]):
            ti = tarfile.TarInfo(name=dst.lstrip("/"))
            ti.type = tarfile.SYMTYPE
            ti.linkname = target
            ti.size = 0
            ti.mode = 0o777
            _norm(ti, epoch)
            tar.addfile(ti)
            links_meta.append({"path": "/" + dst.lstrip("/"), "target": target})
    if links:
        return _gzip(raw.getvalue()), files_meta, links_meta
    return _gzip(raw.getvalue()), files_meta


def write_epk(out_path, manifest, payload_bytes, epoch):
    """Assemble the outer .epk tar (manifest.json + data.tar)."""
    manifest = dict(manifest)
    manifest["payload_sha256"] = sha256_bytes(payload_bytes)
    mbytes = (json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode()
    with tarfile.open(out_path, mode="w") as tar:       # uncompressed outer
        for name, data in ((MANIFEST_NAME, mbytes), (PAYLOAD_NAME, payload_bytes)):
            ti = tarfile.TarInfo(name=name)
            ti.size = len(data)
            ti.mode = 0o644
            _norm(ti, epoch)
            tar.addfile(ti, io.BytesIO(data))
    return manifest


# ----------------------------------------------------------------- read side
def read_manifest(epk_path):
    with tarfile.open(epk_path, mode="r") as tar:
        m = tar.extractfile(MANIFEST_NAME)
        if m is None:
            raise ValueError(f"{epk_path}: no {MANIFEST_NAME} member")
        return json.loads(m.read().decode())


def _read_payload(tar):
    p = tar.extractfile(PAYLOAD_NAME)
    if p is None:
        raise ValueError(f"missing {PAYLOAD_NAME} member")
    return p.read()


def verify(epk_path):
    """Validate payload sha + every per-file sha. Returns (ok, [problems])."""
    problems = []
    with tarfile.open(epk_path, mode="r") as tar:
        manifest = json.loads(tar.extractfile(MANIFEST_NAME).read().decode())
        payload = _read_payload(tar)
    if sha256_bytes(payload) != manifest.get("payload_sha256"):
        problems.append("payload_sha256 mismatch")
        return False, problems
    want = {f["path"]: f["sha256"] for f in manifest.get("files", [])}
    want_links = {l["path"]: l["target"] for l in manifest.get("links", [])}
    with tarfile.open(fileobj=io.BytesIO(_gunzip(payload)), mode="r") as ptar:
        seen = set()
        for ti in ptar.getmembers():
            if ti.issym():
                path = "/" + ti.name.lstrip("/")
                if path not in want_links:
                    problems.append(f"{path}: symlink not listed in manifest links")
                elif ti.linkname != want_links[path]:
                    problems.append(f"{path}: symlink target mismatch")
                continue
            if not ti.isfile():
                continue
            path = "/" + ti.name.lstrip("/")
            seen.add(path)
            got = sha256_bytes(ptar.extractfile(ti).read())
            if path not in want:
                problems.append(f"{path}: not listed in manifest")
            elif got != want[path]:
                problems.append(f"{path}: sha256 mismatch")
        for path in want:
            if path not in seen:
                problems.append(f"{path}: listed in manifest but absent from payload")
    return (not problems), problems


def extract_payload(epk_path, root):
    """Extract payload files under `root`. Returns list of absolute paths written."""
    written = []
    with tarfile.open(epk_path, mode="r") as tar:
        payload = _read_payload(tar)
    with tarfile.open(fileobj=io.BytesIO(_gunzip(payload)), mode="r") as ptar:
        for ti in ptar.getmembers():
            if ti.issym():
                rel = ti.name.lstrip("/")
                dst = os.path.join(root, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                if os.path.lexists(dst):
                    os.unlink(dst)
                os.symlink(ti.linkname, dst)
                written.append(dst)
                continue
            if not ti.isfile():
                continue
            rel = ti.name.lstrip("/")
            dst = os.path.join(root, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as out:
                out.write(ptar.extractfile(ti).read())
            os.chmod(dst, ti.mode)
            try:
                os.chown(dst, 0, 0)          # best-effort; needs root
            except (PermissionError, OSError):
                pass
            written.append(dst)
    return written
