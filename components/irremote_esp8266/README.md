# IRremoteESP8266 (vendored)

- Upstream: https://github.com/crankyoldgit/IRremoteESP8266
- Version: **v2.9.0**, commit `a0c988c9af5130baae9a0c38dde895e1184b859c` (2026-01-03)
- License: **GNU LGPL v2.1** (`LICENSE.txt`). The complete source of this library, including the
  changes below, is part of this repository, so the firmware can be rebuilt and relinked with a
  modified version of the library.

`src/` is the upstream `src/` directory (the `CPPLINT.cfg` lint config removed). Used for automatic
recognition of air-conditioner remotes and to generate their full-state commands
(`components/ac_profiles/ac_irlib.cpp`).

## How it is built

ESP-IDF, no Arduino: the library is compiled with `UNIT_TEST=1`, its own host mode (no `Arduino.h`,
no GPIO or timer access, `String` = `std::string`). The `ESP32`/`ESP8266` macros are not defined, so
none of the Arduino-core receive code is used.

In that mode the protocol classes send through `IRsendTest`, normally defined by the library's unit
tests (`test/IRsend_test.h`, about 110 KB of buffers per instance). `port/IRsend_test.h` replaces it
with a small class that forwards every mark/space to `port/ir_recorder.cpp`; the firmware transmits
the recorded pulse train with the ESP32 RMT peripheral. Decoding is done by `IRrecv::decode()` on a
buffer filled from the RMT capture.

## Changes to the upstream sources

1. `src/IRac.cpp`, `IRac::kelon()`: `setSupercool(false)` restored the previous operating mode
   (Heat after `begin()`), so every Kelon command generated through `IRac` was sent in Heat mode.
   Super cool is now only switched on when requested (marked "ac_matter_ir patch").
2. `src/CPPLINT.cfg` removed (lint configuration, not code).
