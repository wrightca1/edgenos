# AS4610 site deployment ("our config")

The shipped/default image is **clean** — only the mgmt port (ma1) is addressed, via
DHCP, and no front-panel ports carry IPs. These files capture *our* actual deployment;
apply them on top of the clean image per box (copy into place + restart the services,
or stage them via the config overlay).
