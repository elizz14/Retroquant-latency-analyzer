# Compiler settings for ARM64 Cross-Compilation
CC = aarch64-linux-gnu-gcc
CFLAGS = -O2 -Wall -DLV_CONF_INCLUDE_SIMPLE -I. -Icommon
LDFLAGS = -lm -lpthread
LVGL_LIB = liblvgl.a # LVGL static library generated during build

# Targets
all: exchange trader

# Host A (Exchange)
exchange: chart.c common/hal.c
	$(CC) $(CFLAGS) $^ $(LVGL_LIB) -o $@ $(LDFLAGS)

# Host B (Trader)
trader: sysmon.c common/hal.c
	$(CC) $(CFLAGS) $^ $(LVGL_LIB) -o $@ $(LDFLAGS)

clean:
	rm -f exchange trader *.o
