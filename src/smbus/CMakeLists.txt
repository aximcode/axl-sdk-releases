# src/smbus -- its sources of libaxl.a, in the archive's order.
target_sources(axl PRIVATE
    axl-smbus.c
    axl-smbus-hc.c
    axl-smbus-i2c.c
    axl-smbus-piix4.c
    axl-smbus-format.c)
