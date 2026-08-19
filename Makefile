CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic

BUILD_DIR := build
TARGET := $(BUILD_DIR)/bluemax
SOURCES := bluemax.c thermal.c
HEADERS := thermal.h
TEST_TARGET := $(BUILD_DIR)/test_thermal
TEST_SOURCES := tests/test_thermal.c tests/test_helpers.c thermal.c
TEST_HEADERS := tests/test_helpers.h

.PHONY: all bluemax test clean

all: bluemax

bluemax: $(TARGET)

$(TARGET): $(SOURCES) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(SOURCES) $(LDFLAGS) $(LDLIBS)

test: $(TEST_TARGET)
	$(TEST_TARGET)

$(TEST_TARGET): $(TEST_SOURCES) $(TEST_HEADERS) $(HEADERS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(CPPFLAGS) -I. -o $@ $(TEST_SOURCES) $(LDFLAGS) $(LDLIBS)

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -f $(TARGET) $(TEST_TARGET)
	rmdir $(BUILD_DIR) 2>/dev/null || true
