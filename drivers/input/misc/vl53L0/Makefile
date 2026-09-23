#
# Makefile for the vl53L0 drivers.
#

# Each configuration option enables a list of files.
# NOTE: these were bare relative -I paths, which only resolved correctly
# for an in-tree (srctree == objtree) build; with an out-of-tree O= build
# the compiler's cwd is the objtree, so the plain-header-only "inc" dir
# (which has no .o targets and is therefore never mirrored into objtree)
# was never found. Prefixed with $(srctree)/, the standard kbuild idiom
# for cross-directory includes (see e.g. drivers/media/v4l2-core/Makefile).
ifeq ($(CONFIG_SENSORS_STMVL53L0_USE_CCI), y)
ccflags-y	+= -I$(srctree)/drivers/input/misc/vl53L0/inc -DCAMERA_CCI
else
ccflags-y	+= -I$(srctree)/drivers/input/misc/vl53L0/inc
endif

ccflags-y += -I$(srctree)/drivers/media/platform/msm/camera_v2/sensor/io
ccflags-y += -I$(srctree)/drivers/media/platform/msm/camera_v2
ccflags-y += -I$(srctree)/drivers/media/platform/msm/camera_v2/common
ccflags-y += -I$(srctree)/drivers/media/platform/msm/camera_v2/sensor/cci
obj-$(CONFIG_INPUT_STMVL53L0)			+= stmvl53l0.o
stmvl53l0-objs				:= stmvl53l0_module.o stmvl53l0_module-i2c.o stmvl53l0_module-cci.o src/vl53l0_api_calibration.o src/vl53l0_api_core.o src/vl53l0_api_histogram.o src/vl53l0_api_ranging.o src/vl53l0_api_strings.o src/vl53l0_api.o src/vl53l0_platform.o src/vl53l0_i2c_platform.o src/vl53l0_port_i2c.o src/vl53l010_api.o src/vl53l010_tuning.o
