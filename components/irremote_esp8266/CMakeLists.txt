# IRremoteESP8266 v2.9.0 (LGPL-2.1, see LICENSE.txt and README.md) built for ESP-IDF without Arduino.
#
# UNIT_TEST=1 is the library's own host mode: no Arduino.h, no GPIO, no timers, std::string as
# String. Protocol classes then send through IRsendTest, which port/IRsend_test.h replaces with a
# small class that hands every pulse to port/ir_recorder.cpp. Decoding reads a caller-filled buffer.
file(GLOB IRREMOTE_SRCS "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")

idf_component_register(SRCS ${IRREMOTE_SRCS} "port/ir_recorder.cpp"
                       INCLUDE_DIRS "src" "port")

target_compile_definitions(${COMPONENT_LIB} PUBLIC UNIT_TEST=1)
# Third-party sources: keep the project's warnings-as-errors for our own code only.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
