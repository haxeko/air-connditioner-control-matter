# Host unit tests of the hardware-independent modules (gcc, AddressSanitizer + UBSan).
#   make -C test/host -j8    build and run
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
	-I$(ROOT)/components/matter_thermostat/include \
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
	$(ROOT)/components/web_config/web_util.c \
	$(ROOT)/components/matter_thermostat/matter_map.c

TESTS := test_main.c test_button.c test_ir.c test_ac.c test_climate.c test_rt.c test_config.c test_web.c \
	test_matter_map.c

# IR protocol library (components/irremote_esp8266, built like on the device: UNIT_TEST=1, no Arduino)
# and its glue components/ac_profiles/ac_irlib.cpp. The library objects are compiled once (use -j).
CXX ?= g++
IRLIB_DIR := $(ROOT)/components/irremote_esp8266
IRLIB_SRCS := $(wildcard $(IRLIB_DIR)/src/*.cpp) $(IRLIB_DIR)/port/ir_recorder.cpp
IRLIB_OBJS := $(patsubst %.cpp,$(BUILD)/irlib/%.o,$(notdir $(IRLIB_SRCS)))
IRLIB_CXXFLAGS := -std=gnu++17 -O1 -DUNIT_TEST=1 -w -I$(IRLIB_DIR)/src -I$(IRLIB_DIR)/port
SAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
GLUE_CXXFLAGS := -std=gnu++17 -Wall -Wextra -Werror -O1 -g -DUNIT_TEST=1 $(SAN) $(INCLUDES) \
	-isystem $(IRLIB_DIR)/src -isystem $(IRLIB_DIR)/port
IRLIB_C_MODULES := $(ROOT)/components/ir_transceiver/ir_code.c $(ROOT)/components/ir_transceiver/ir_capture.c \
	$(ROOT)/components/ac_profiles/ac_types.c
IRLIB_C_OBJS := $(patsubst %.c,$(BUILD)/c/%.o,$(notdir $(IRLIB_C_MODULES)))

vpath %.cpp $(IRLIB_DIR)/src $(IRLIB_DIR)/port
vpath %.c $(ROOT)/components/ir_transceiver $(ROOT)/components/ac_profiles

all: run

$(BUILD)/host_tests: $(TESTS) $(MODULES) test.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $(TESTS) $(MODULES) $(LDLIBS)

$(BUILD)/irlib/%.o: %.cpp | $(BUILD)
	@mkdir -p $(BUILD)/irlib
	$(CXX) $(IRLIB_CXXFLAGS) -c $< -o $@

$(BUILD)/c/%.o: %.c | $(BUILD)
	@mkdir -p $(BUILD)/c
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/irlib_tests: test_irlib.cpp $(ROOT)/components/ac_profiles/ac_irlib.cpp test.h $(IRLIB_OBJS) $(IRLIB_C_OBJS)
	$(CXX) $(GLUE_CXXFLAGS) -o $@ test_irlib.cpp $(ROOT)/components/ac_profiles/ac_irlib.cpp $(IRLIB_OBJS) \
		$(IRLIB_C_OBJS) $(LDLIBS)

$(BUILD):
	mkdir -p $(BUILD)

run: $(BUILD)/host_tests $(BUILD)/irlib_tests
	./$(BUILD)/host_tests
	./$(BUILD)/irlib_tests

clean:
	rm -rf $(BUILD)

.PHONY: all run clean
