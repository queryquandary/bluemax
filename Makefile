CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

BUILD_DIR := build
TARGET := $(BUILD_DIR)/bluemax
SOURCES := bluemax.c thermal.c
HEADERS := thermal.h

.PHONY: all bluemax clean

all: bluemax

bluemax: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -f $(TARGET)
	rmdir $(BUILD_DIR) 2>/dev/null || true
