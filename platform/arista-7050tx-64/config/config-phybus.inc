
# --- copper ports: route external PHY access to our SCD MDIO hook ---
# phyctrl.c:529 swaps ext_pc.read/write to phy_i2c_miireg_* for any port
# with this set, which then calls whatever phy_i2c_bus_func_hook_set
# installed. The 48 10GBASE-T ports are 1-48; the QSFPs (49+) must NOT be
# listed -- they have no external PHY and run off the internal TSC.

phy_bus_i2c_1.0=1
phy_bus_i2c_2.0=1
phy_bus_i2c_3.0=1
phy_bus_i2c_4.0=1
phy_bus_i2c_5.0=1
phy_bus_i2c_6.0=1
phy_bus_i2c_7.0=1
phy_bus_i2c_8.0=1
phy_bus_i2c_9.0=1
phy_bus_i2c_10.0=1
phy_bus_i2c_11.0=1
phy_bus_i2c_12.0=1
phy_bus_i2c_13.0=1
phy_bus_i2c_14.0=1
phy_bus_i2c_15.0=1
phy_bus_i2c_16.0=1
phy_bus_i2c_17.0=1
phy_bus_i2c_18.0=1
phy_bus_i2c_19.0=1
phy_bus_i2c_20.0=1
phy_bus_i2c_21.0=1
phy_bus_i2c_22.0=1
phy_bus_i2c_23.0=1
phy_bus_i2c_24.0=1
phy_bus_i2c_25.0=1
phy_bus_i2c_26.0=1
phy_bus_i2c_27.0=1
phy_bus_i2c_28.0=1
phy_bus_i2c_29.0=1
phy_bus_i2c_30.0=1
phy_bus_i2c_31.0=1
phy_bus_i2c_32.0=1
phy_bus_i2c_33.0=1
phy_bus_i2c_34.0=1
phy_bus_i2c_35.0=1
phy_bus_i2c_36.0=1
phy_bus_i2c_37.0=1
phy_bus_i2c_38.0=1
phy_bus_i2c_39.0=1
phy_bus_i2c_40.0=1
phy_bus_i2c_41.0=1
phy_bus_i2c_42.0=1
phy_bus_i2c_43.0=1
phy_bus_i2c_44.0=1
phy_bus_i2c_45.0=1
phy_bus_i2c_46.0=1
phy_bus_i2c_47.0=1
phy_bus_i2c_48.0=1
