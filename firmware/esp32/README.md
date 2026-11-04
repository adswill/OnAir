# OnAir ESP32 radio firmware

Makes an ESP32 board (optionally with an nRF24L01+ module on SPI) an OnAir receiver and transmitter over USB serial. GPL-3.0-or-later.
**Never run on real hardware**: the protocol logic (`onair_core.h`) is tested in OnAir's loopback test against a fake hardware layer; `onair_esp32.ino`, the real hardware layer, is untested and may need fixes.

* `onair_esp32/onair_wire.h` protocol (shared with OnAir), `onair_core.h` dispatcher, `onair_esp32.ino` hardware layer.
* Build with the Arduino IDE (Arduino-ESP32 3.x, board ESP32 Dev Module / S3 / C3 / C6) or `pio run -e esp32dev` in this folder.
* nRF24 wiring (edit at the top of the sketch): SCK 18, MISO 19, MOSI 23, CSN 5, CE 4, 3.3 V with 10 uF across the supply.
* Transmit is off until OnAir sends TX_ENABLE (the Arm button). WiFi is observation only: no transmit, no deauthentication, no key capture.
* Not implemented: 802.15.4 (C6/H2), WiFi transport, 5 GHz.

Protocol and design: docs/ESP32-NRF24-DESIGN.md.
