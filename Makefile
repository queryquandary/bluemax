CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

BUILD_DIR := build
TARGET := $(BUILD_DIR)/bluemax
SOURCES := bluemax.c gpu_mmio.c thermal.c
HEADERS := gpu_mmio.h thermal.h
THERMAL_TEST_TARGET := $(BUILD_DIR)/test_thermal
THERMAL_TEST_SOURCES := tests/test_thermal.c tests/test_helpers.c thermal.c
GPU_MMIO_TEST_TARGET := $(BUILD_DIR)/test_gpu_mmio
GPU_MMIO_TEST_SOURCES := tests/test_gpu_mmio.c tests/test_helpers.c gpu_mmio.c
TEST_TARGETS := $(THERMAL_TEST_TARGET) $(GPU_MMIO_TEST_TARGET)
TEST_HEADERS := tests/test_helpers.h gpu_mmio.h thermal.h

.PHONY: all bluemax test clean

all: bluemax

bluemax: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

test: $(TEST_TARGETS)
	$(THERMAL_TEST_TARGET)
	$(GPU_MMIO_TEST_TARGET)

$(THERMAL_TEST_TARGET): $(THERMAL_TEST_SOURCES) $(TEST_HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I. -o $@ $(THERMAL_TEST_SOURCES) $(LDFLAGS) $(LDLIBS)

$(GPU_MMIO_TEST_TARGET): $(GPU_MMIO_TEST_SOURCES) $(TEST_HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I. -o $@ $(GPU_MMIO_TEST_SOURCES) $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -f $(TARGET) $(TEST_TARGETS)
	rmdir $(BUILD_DIR) 2>/dev/null || true
