#!/usr/bin/env python3
"""
EdgeNOS package format (.epk) — shared library used by both the host-side builder
(pkgbuild.py) and the on-box installer (epkg.py).

An .epk is a plain (uncompressed) tar container with exactly two members, in order:

    manifest.json     metadata (read without touching the payload)
    data.tar          the file payload (paths relative to the rootfs root)

Everything is stdlib-only (tarfile, json, hashlib) and the payload is an
*uncompressed* tar — the minimal Buildroot Python on the switches ships NO
compression modules (no zlib/gzip/bz2/lzma), so epkg.py must not depend on any.
The image itself is squashfs-compressed, so the .epk is only transport. Builds are
reproducible: file mtime/uid/gid are normalized, members are sorted, timestamps
come from an explicit epoch.

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
    payload_sha256      str         sha256 of the data.tar member
    build               {epoch, timestamp}
"""
import io, os, json, tarfile, hashlib

FORMAT_VERSION = 1
MANIFEST_NAME = "manifest.json"
PAYLOAD_NAME = "data.tar"
HOOK_NAMES = ("preinst", "postinst", "prerm", "postrm")


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
def build_payload(staged, epoch):
    """staged: list of (dst_path_no_leading_slash, abs_src, mode_int).
    Returns (payload_bytes, files_meta). Deterministic."""
    files_meta = []
    raw = io.BytesIO()
    # uncompressed payload tar (no compression module needed on-box)
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
    return raw.getvalue(), files_meta


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
    with tarfile.open(fileobj=io.BytesIO(payload), mode="r") as ptar:
        seen = set()
        for ti in ptar.getmembers():
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
    with tarfile.open(fileobj=io.BytesIO(payload), mode="r") as ptar:
        for ti in ptar.getmembers():
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
