# EdgeNOS docker build system

The build scripts in `build/` and `core/control-plane/` have always run their
compilers inside containers. Those container images only ever existed as *built
images* on the old build host — no Dockerfile was ever committed. When that host
was lost, the scripts survived in git with nothing left to run in.

This directory is the fix: the builders are now reproducible from the repo.

## The images

| Image | Built from | Used by |
|---|---|---|
| `edgenos/builder9:1.8-rootless` | `Dockerfile.builder9` | `build/build-bcmd.sh`, `build/build-fit-4610.sh`, `build/build-fit-5610.sh`, `build/build-onie-installer-4610.sh` |
| `sdk5610build:1` | `Dockerfile.sdk5610` | `build/build-sdk-5610.sh`, `build/build-bcmd-5610.sh` |
| `edgenos-builder` | `Dockerfile.edgenos-builder` | `core/control-plane/build-quagga.sh` |

The names and tags are exactly the ones the build scripts already default to, so
**no build script needed changing** — they find these images and run unmodified.

`Dockerfile.builder9` and `Dockerfile.edgenos-builder` are *reconstructions*:
rebuilt from what the scripts actually invoke, not recovered from the originals.
`Dockerfile.sdk5610` matches the recipe `build-sdk-5610.sh` still carries inline.

Base is Debian 11 (bullseye) throughout — the newest Debian that still ships a
python2 interpreter, which ONL's `mkshar` and `pyfit` both need. The "9" in
`builder9` is historical (Debian 9 / ONL stretch era). If the 4610 SDK build ever
trips over gcc-10 strictness:

```sh
docker build --build-arg BASE=debian:buster -f docker/Dockerfile.builder9 docker/
```

## Usage

```sh
docker/setup-host.sh      # one-time host setup (rootless docker)
docker/build-images.sh    # build all three images  (or: build-images.sh builder9)
docker/verify-images.sh   # exercise every toolchain the build scripts call
```

`verify-images.sh` actually cross-compiles and checks the resulting ELF machine
type, so a toolchain that quietly produced x86 objects would be caught.

## Why rootless

Every build script runs its container as `-u root:0` and bind-mounts the source
tree. Under rootless docker container-uid-0 maps to the invoking user, so build
outputs land owned by that user. Under a rootful daemon the same scripts would
write root-owned files into the tree and the pipeline would break. This is also
why the build scripts default `DOCKER_HOST` to the per-user socket under
`/run/user/`, and why the image tag says `rootless`.

## Host quirks (rootless docker in an unprivileged container)

The build host runs Ubuntu 24.04 inside an unprivileged LXC container.
`setup-host.sh` handles all of the following; they are recorded here because each
one cost real debugging time, and any similarly nested host will hit them too.

- **subuid/subgid.** An unprivileged LXC maps a block of host uids onto the
  container's `0..65535`, so the distro-default subordinate range names uids that
  do not exist inside the container and `newuidmap` fails with
  `write to uid_map failed: Operation not permitted`. The subordinate range has to
  be carved out of the uid space the container actually has.
- **AppArmor.** The shipped `rootlesskit` profile is `flags=(unconfined)` and is
  harmless on a normal host, but inside a nested AppArmor namespace merely
  transitioning into it makes `socket()` return `EACCES` — every pull fails with
  `dial udp ...: socket: permission denied`. The profile is disabled.
- **Storage driver `vfs`.** No overlayfs module in this kernel and no `/dev/fuse`,
  so neither `overlay2` nor `fuse-overlayfs` is available. `vfs` is correct but
  copies whole layers rather than sharing them: images are large (builder9 is
  ~1.1 GB) and builds are IO-heavy. Keep an eye on disk.
- **DNS.** `/etc/resolv.conf` on this host is managed by a DNS agent that
  advertises an IPv6 resolver the rootless daemon cannot reach, so a small wrapper
  pins IPv4 resolvers inside the daemon's namespace only. The host file is
  untouched.
- **Containers must use `--network host`.** slirp4netns cannot forward traffic in
  this environment, and a container with a *private* netns dies in runc with
  `open sysctl net.ipv4.ip_unprivileged_port_start file: ... permission denied`
  because the container mounts `/proc/sys` read-only (`proc:mixed`). Fixing that
  properly requires a change to the **outer container's configuration**, not
  anything in here.

  In practice this costs nothing: the build scripts that need network already pass
  `--network host` / `--network=host`, and the rest are offline cross-compiles.
  `build-images.sh` and `verify-images.sh` pass `--network host` too (override with
  `NETWORK=bridge` on a normal host).

## Still missing: the external source trees

The images are only half of a build. The build scripts also reference large
vendor/sibling checkouts that lived on the old build host and are **not** in this
repo — they are not restored by anything here, and every full pipeline will fail
on a missing-path check until they are back:

| Expected path | Wanted by |
|---|---|
| `../OpenMDK` | `build-edged.sh`, `build-sdk-and-edged.sh` |
| `../newnos` | `build-sdk-and-edged.sh`, `build-fit-5610.sh` (kernel + initramfs) |
| `../edgecore-4610-54t` | `build-bcmd.sh` (SDK), `build-fit-4610.sh`, `build-onie-installer-4610.sh` |
| `../OpenNetworkLinux` | `build-fit-4610.sh`, `build-onie-installer-4610.sh` (`mkshar`, `pyfit`, loader initrd) |
| OpenBCM `sdk-6.5.16` | `build-sdk-5610.sh`, `build-bcmd-5610.sh` |

(`..` is the directory containing `edgenos/`.) Each script takes an env override —
e.g. `SDK=`, `KIMAGE=`, `KERNEL_GZ=` — so they can be pointed elsewhere once the
trees are recovered or re-cloned.
