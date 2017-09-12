# Microsemi MT88L70 driver

This driver use MT88L70 chip's pins: **STD** and **Q1-Q4**

**STD pin** - used IRQ when success decoded DTMF code from audio inputs

**Q1-Q4 pins** - used for convert received bits into keycodes

For implementation see [device tree bindings doc](dtmf-mt88l70.txt)
