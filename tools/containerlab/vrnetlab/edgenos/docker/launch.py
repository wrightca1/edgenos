#!/usr/bin/env python3
"""vrnetlab launcher for the EdgeNOS x86_64 virtual switch (containerlab kind: generic_vm).

The VM boots from the EdgeNOS qcow2; NIC 0 is ma1 (mgmt, DHCP from the vrnetlab/QEMU
user network, SSH forwarded from the container), NICs 1..N are ge0..geN-1 and map to
the container's eth1..ethN (tc connection mode). We wait for the serial login prompt,
log in as root, set the hostname and the requested root password, and declare the VM
running. Serial console: telnet <container> 5000.
"""
import datetime
import logging
import os
import re
import signal
import sys

import vrnetlab

DEFAULT_USER = "root"
DEFAULT_PASSWORD = "edgenos"


def handle_SIGCHLD(_signal, _frame):
    os.waitpid(-1, os.WNOHANG)


def handle_SIGTERM(_signal, _frame):
    sys.exit(0)


signal.signal(signal.SIGINT, handle_SIGTERM)
signal.signal(signal.SIGTERM, handle_SIGTERM)
signal.signal(signal.SIGCHLD, handle_SIGCHLD)

TRACE_LEVEL_NUM = 9
logging.addLevelName(TRACE_LEVEL_NUM, "TRACE")


def trace(self, message, *args, **kws):
    if self.isEnabledFor(TRACE_LEVEL_NUM):
        self._log(TRACE_LEVEL_NUM, message, args, **kws)


logging.Logger.trace = trace


class EdgeNOS_vm(vrnetlab.VM):
    def __init__(self, hostname, username, password, conn_mode, nics):
        disk_image = "/"
        for e in os.listdir("/"):
            if re.search(r"\.qcow2$", e):
                disk_image = "/" + e
                break
        super(EdgeNOS_vm, self).__init__(
            username, password, disk_image=disk_image, ram=1024, smp="1"
        )
        self.nic_type = "virtio-net-pci"
        self.conn_mode = conn_mode
        self.num_nics = nics
        self.hostname = hostname

    def bootstrap_spin(self):
        if self.spins > 300:
            self.logger.debug("too many spins with no result -> restart")
            self.stop()
            self.start()
            return

        ridx, match, res = self.tn.expect([b"login:"], 1)
        if match and ridx == 0:
            self.logger.info("VM started")
            self.wait_write("\r", None)
            self.wait_write(DEFAULT_USER, wait="login:")
            self.wait_write(DEFAULT_PASSWORD, wait="Password:")
            self.wait_write("", wait="#")
            self.logger.info("login completed")
            self.bootstrap_config()
            self.tn.close()
            startup_time = datetime.datetime.now() - self.start_time
            self.logger.info(f"startup complete in: {startup_time}")
            self.running = True
            return

        if res != b"":
            self.logger.trace("OUTPUT: %s" % res.decode(errors="replace"))
            self.spins = 0
        self.spins += 1

    def bootstrap_config(self):
        """Hostname + the credentials containerlab asked for. root/edgenos stays valid on the
        serial console; the requested user (clab by default) gets SSH access."""
        self.logger.info("applying bootstrap configuration")
        self.wait_write("hostnamectl set-hostname %s" % self.hostname, "#")
        self.wait_write("sed -i 's/^hostname .*/hostname %s/' /etc/quagga/*.conf" % self.hostname, "#")
        if self.username == "root":
            if self.password and self.password != DEFAULT_PASSWORD:
                self.wait_write("printf '%s\\n%s\\n' | passwd root >/dev/null 2>&1" % (self.password, self.password), "#")
        else:
            self.wait_write("id %s >/dev/null 2>&1 || adduser -D -h /home/%s -s /bin/sh %s" % (self.username, self.username, self.username), "#")
            self.wait_write("printf '%s\\n%s\\n' | passwd %s >/dev/null 2>&1" % (self.password, self.password, self.username), "#")
        mode = os.getenv("EDGENOS_DATAPATH", "").strip()
        if mode in ("vswitch", "none"):
            self.logger.info("datapath mode: %s" % mode)
            self.wait_write("/opt/edgenos/edgenos-datapath-mode %s --now" % mode, "#")
        self.push_startup_config()
        self.wait_write("edgenos version", "#")
        self.logger.info("completed bootstrap configuration")

    def push_startup_config(self):
        """Per-node startup config: a directory bound into the container at /startup (or
        /config/startup) with netconf.sh / frr.conf / daemons / sysctl.conf. Copied into the
        VM at /etc/edgenos/startup (persists on the overlay) over the host-forwarded SSH, then
        applied with edgenos-startup.sh --restart-frr. Reboots re-apply it via the unit."""
        src = None
        for d in ("/startup", "/config/startup"):
            if os.path.isdir(d) and os.listdir(d):
                src = d
                break
        if not src:
            self.logger.info("no startup config dir (/startup) — skipping")
            return
        self.logger.info("pushing startup config from %s" % src)
        self.wait_write("mkdir -p /etc/edgenos/startup", "#")
        import subprocess, time as _t
        ok = False
        for attempt in range(30):
            r = subprocess.run(["sshpass", "-p", DEFAULT_PASSWORD, "scp", "-q", "-o", "StrictHostKeyChecking=no",
                                "-o", "UserKnownHostsFile=/dev/null", "-o", "ConnectTimeout=5", "-r", src + "/.",
                                "%s@127.0.0.1:/etc/edgenos/startup/" % DEFAULT_USER],
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if r.returncode == 0:
                ok = True
                break
            self.logger.info("scp attempt %d failed: %s" % (attempt + 1, r.stdout.decode(errors="replace")[-200:]))
            _t.sleep(5)
        if not ok:
            self.logger.error("could not push the startup config")
            return
        self.wait_write("chmod +x /etc/edgenos/startup/*.sh 2>/dev/null; /opt/edgenos/edgenos-startup.sh --restart-frr", "#")
        self.logger.info("startup config applied")


class EdgeNOS(vrnetlab.VR):
    def __init__(self, hostname, username, password, conn_mode, nics):
        super().__init__(username, password)
        self.vms = [EdgeNOS_vm(hostname, username, password, conn_mode, nics)]


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(description="")
    parser.add_argument("--trace", action="store_true", help="enable trace level logging")
    parser.add_argument("--hostname", default="edgenos", help="hostname")
    parser.add_argument("--username", default=DEFAULT_USER, help="Username")
    parser.add_argument("--password", default=DEFAULT_PASSWORD, help="Password")
    # containerlab only creates the ethN that have links, and vrnetlab maps VM NIC i <-> container
    # eth<i> by index, so the VM must carry NICs up to the HIGHEST eth index in the topology, not
    # just as many as there are links: default to 32 (like other vrnetlab NOS launchers; unpeered
    # ports simply stay down). Override with EDGENOS_NICS / --nics.
    parser.add_argument("--nics", type=int, default=int(os.getenv("EDGENOS_NICS", "32")),
                        help="number of front-panel NICs to attach (default EDGENOS_NICS or 32)")
    parser.add_argument("--connection-mode", default="tc", help="Connection mode to use in the datapath")
    args = parser.parse_args()

    LOG_FORMAT = "%(asctime)s: %(module)-10s %(levelname)-8s %(message)s"
    logging.basicConfig(format=LOG_FORMAT)
    logger = logging.getLogger()
    logger.setLevel(logging.DEBUG)
    if args.trace:
        logger.setLevel(1)

    vr = EdgeNOS(args.hostname, args.username, args.password, args.connection_mode, args.nics)
    vr.start()
