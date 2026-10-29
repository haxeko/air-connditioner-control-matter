# Host unit tests of the hardware-independent modules (gcc, AddressSanitizer + UBSan).
#   make -C test/host        build and run
#   make -C test/host clean
ROOT := ../..
CC ?= gcc
BUILD := build

INCLUDES := -Ishim \
	-I$(ROOT)/components/button_handler/include \
	-I$(ROOT)/components/ir_transceiver/include \
	-I$(ROOT)/components/ac_profiles/include \
	-I$(ROOT)/components/climate_control/include \
	-I$(ROOT)/components/remote_temperature/include \
	-I$(ROOT)/components/config_store/include \
	-I$(ROOT)/components/web_config

CFLAGS := -std=gnu11 -Wall -Wextra -Werror -Wno-format-truncation -O1 -g \
	-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(INCLUDES)
LDLIBS := -lm

MODULES := \
	$(ROOT)/components/button_handler/press_detector.c \
	$(ROOT)/components/ir_transceiver/ir_code.c \
	$(ROOT)/components/ir_transceiver/ir_capture.c \
	$(ROOT)/components/ac_profiles/ac_types.c \
	$(ROOT)/components/climate_control/climate_algo.c \
	$(ROOT)/components/remote_temperature/rt_logic.c \
	$(ROOT)/components/config_store/app_config.c \
	$(ROOT)/components/web_config/web_util.c

TESTS := test_main.c test_button.c test_ir.c test_ac.c test_climate.c test_rt.c test_config.c test_web.c

all: run

$(BUILD)/host_tests: $(TESTS) $(MODULES) test.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODULES) $(LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

run: $(BUILD)/host_tests
	./$(BUILD)/host_tests

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
