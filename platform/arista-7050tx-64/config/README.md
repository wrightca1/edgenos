# config/ — SDK properties and PHY bus map

## ⚠ `config.bcm` is not here, and that is deliberate

The SDK will not attach without a property set: with none, `soc_attach` fails
with `Port config error !!` and `soc_cm_device_init` returns `-2`, because it
cannot build a port map. So this file is **required to boot**, and it is the one
piece of this platform that is not ours to publish.

The working `config.bcm` is 549 properties — `portmap_N`, `port_phy_addr_N`,
`phy_xaui_{tx,rx}_polarity_flip_N`, `phy_mdi_pair_map_N` and friends — describing
which SerDes lane reaches which front-panel port, with what polarity and PHY
address. That is the board vendor's data.

## Generating one on your own switch

`tools/mkconfigbcm.py` produces it from the property set the switch's own NOS
reports:

```
# on the switch, under the vendor NOS:
platform trident diag config          > asic-config.txt
# then:
tools/mkconfigbcm.py asic-config.txt  > config.bcm
```

It does two transforms and documents both: strip the CLI's indent, and remap the
unit suffix (the vendor NOS drives this chip as unit 1; our agent gets unit 0
from `soc_cm_device_create`, and a property tagged `.1` is invisible to unit 0).

Add `config-phybus.inc` when driving the 48 copper PHYs — it sets
`phy_bus_i2c_<port>=1` so the SDK looks for an external PHY on the SCD MDIO bus
rather than binding the internal TSC SerDes. That file **is** ours and is here.

## The independent route

A port map derived by measurement rather than capture — bringing up each SerDes
lane and determining which front-panel port it lights — would remove this
dependency entirely. It is the honest long-term answer and it has not been done.
See `PROVENANCE.md`.
