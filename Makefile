lwis-objs := lwis_device.o
lwis-objs += lwis_clock.o
lwis-objs += lwis_gpio.o
lwis-objs += lwis_i2c.o
lwis-objs += lwis_ioctl.o
lwis-objs += lwis_pinctrl.o
lwis-objs += lwis_regulator.o

# Device tree specific file
ifeq ($(CONFIG_OF), y)
lwis-objs += lwis_dt.o
endif

obj-$(CONFIG_LWIS) += lwis.o

subdir-ccflags-$(CONFIG_LWIS) += -Idrivers/media/platform/google/lwis
