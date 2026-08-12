#!/usr/bin/env bash
# One-time host setup for an EdgeNOS docker build host: rootless docker on
# Ubuntu 24.04 running inside an unprivileged LXC container.
#
# This exists because the original build host was configured by hand and nothing
# about it was in version control. When it died the build system died with it.
# This script + the Dockerfiles next to it are the replacement.
#
# Rootless (not rootful) is deliberate: every build script runs its container as
# `-u root:0` and bind-mounts the source tree, so container-uid-0 must map to the
# host's own user or the build outputs land root-owned and the pipeline breaks.
#
# Idempotent -- safe to re-run. Needs passwordless sudo.
#
# Usage: docker/setup-host.sh
set -euo pipefail

USER_NAME=$(id -un)
USER_UID=$(id -u)
RUNTIME_DIR=/run/user/$USER_UID

echo "== 1. prerequisites =="
sudo apt-get update -qq
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
  uidmap slirp4netns dbus-user-session iptables ca-certificates curl

echo "== 2. docker-ce + rootless extras =="
if ! command -v dockerd-rootless-setuptool.sh >/dev/null 2>&1; then
  sudo install -m 0755 -d /etc/apt/keyrings
  sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
  sudo chmod a+r /etc/apt/keyrings/docker.asc
  . /etc/os-release
  echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] https://download.docker.com/linux/ubuntu $VERSION_CODENAME stable" \
    | sudo tee /etc/apt/sources.list.d/docker.list >/dev/null
  sudo apt-get update -qq
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -qq \
    docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-ce-rootless-extras
fi
# The rootful daemon is not used; leave it off so it cannot grab the socket.
sudo systemctl disable --now docker.service docker.socket 2>/dev/null || true

echo "== 3. subuid/subgid must fall INSIDE this LXC's uid range =="
# An unprivileged LXC maps a block of host uids onto the container's 0..65535, so
# the distro-default subordinate range names uids that do not exist in here and
# newuidmap fails with "write to uid_map failed: Operation not permitted".
# Carve a subordinate range out of what the container actually has.
if ! grep -q "^${USER_NAME}:10000:55000$" /etc/subuid 2>/dev/null; then
  echo "${USER_NAME}:10000:55000" | sudo tee /etc/subuid >/dev/null
  echo "${USER_NAME}:10000:55000" | sudo tee /etc/subgid >/dev/null
fi

echo "== 4. disable the rootlesskit AppArmor profile =="
# The shipped profile is flags=(unconfined) and "allows everything" on a normal
# host, but inside the LXC's AppArmor namespace the transition into it is enough
# to make socket() return EACCES -- every image pull dies with
# "dial udp ...: socket: permission denied". Disabling the profile restores the
# plain unconfined label. The symlink persists across reboots.
if [ -f /etc/apparmor.d/rootlesskit ]; then
  sudo mkdir -p /etc/apparmor.d/disable
  sudo ln -sf /etc/apparmor.d/rootlesskit /etc/apparmor.d/disable/rootlesskit
  sudo apparmor_parser -R /etc/apparmor.d/rootlesskit 2>/dev/null || true
fi

echo "== 5. storage driver =="
# No overlayfs module in this LXC kernel and no /dev/fuse, so neither overlay2 nor
# fuse-overlayfs can be used. vfs is the remaining option: correct, but it copies
# whole layers instead of sharing them, so images are large and builds are IO-heavy.
mkdir -p "$HOME/.config/docker"
cat > "$HOME/.config/docker/daemon.json" <<'JSON'
{
  "storage-driver": "vfs"
}
JSON

echo "== 6. DNS wrapper for the rootless daemon =="
# This host's /etc/resolv.conf is managed by a DNS agent that advertises an IPv6
# resolver the rootless daemon cannot reach. Pin IPv4 resolvers in its namespace.
mkdir -p "$HOME/.local/bin"
cat > "$HOME/.local/bin/dockerd-dns-v4.sh" <<'SH'
#!/bin/sh
# Runs INSIDE the rootlesskit namespace: dockerd-rootless.sh execs $DOCKERD there,
# after --copy-up=/etc has made /etc a tmpfs.
#
# copy-up leaves /etc/resolv.conf as a SYMLINK into a read-only .ro* mount, so it
# must be unlinked first -- a plain '>' redirect follows the link and fails.
rm -f /etc/resolv.conf
printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > /etc/resolv.conf
exec dockerd "$@"
SH
chmod +x "$HOME/.local/bin/dockerd-dns-v4.sh"

echo "== 7. rootless daemon =="
export XDG_RUNTIME_DIR=$RUNTIME_DIR
export PATH=/usr/bin:$PATH
dockerd-rootless-setuptool.sh install --skip-iptables 2>/dev/null \
  || dockerd-rootless-setuptool.sh install

mkdir -p "$HOME/.config/systemd/user/docker.service.d"
cat > "$HOME/.config/systemd/user/docker.service.d/edgenos-lxc.conf" <<SH
[Service]
Environment=DOCKERD=$HOME/.local/bin/dockerd-dns-v4.sh
# slirp4netns' seccomp filter and mount sandbox both misbehave under the nested
# userns in this LXC. Note also that slirp cannot forward traffic here at all, so
# the daemon must stay in the host netns -- i.e. keep docker >= 28's default
# detached container netns, do NOT set DOCKERD_ROOTLESS_ROOTLESSKIT_DETACH_NETNS=0.
Environment=DOCKERD_ROOTLESS_ROOTLESSKIT_SLIRP4NETNS_SECCOMP=false
Environment=DOCKERD_ROOTLESS_ROOTLESSKIT_SLIRP4NETNS_SANDBOX=false
SH

# Keep the user manager (and the daemon) alive without an active login session.
sudo loginctl enable-linger "$USER_NAME"

systemctl --user daemon-reload
systemctl --user enable docker.service >/dev/null 2>&1 || true
systemctl --user restart docker.service
sleep 5

echo "== 8. verify =="
export DOCKER_HOST=unix://$RUNTIME_DIR/docker.sock
docker info --format 'server {{.ServerVersion}}  storage {{.Driver}}  rootless {{.SecurityOptions}}' 2>&1 | head -1
docker run --rm --network host hello-world >/dev/null 2>&1 \
  && echo "container run: OK" || { echo "container run: FAILED"; exit 1; }

cat <<EOF

Rootless docker is up. Add to ~/.bashrc if not already there:

  export PATH=/usr/bin:\$PATH
  export DOCKER_HOST=unix://$RUNTIME_DIR/docker.sock

Next: docker/build-images.sh   (build the three builder images)
      docker/verify-images.sh  (check their toolchains)
EOF
