# Windbird desk display

A small desk dashboard showing live wind from an [OpenWindMap](https://www.openwindmap.org/windbird-2176) station
(Windbird 2176, Lac du Bourget) on a Waveshare ESP32-S3-Touch-AMOLED-1.8.

It shows the average speed (knots, one decimal), min/max, a compass marker for where the wind comes from,
and how old the reading is. Data is refreshed once a minute from `http://api.pioupiou.fr/v1/live/<id>`.

## Hardware

Waveshare ESP32-S3-Touch-AMOLED-1.8, **V2** revision (CO5300 display driver, touch chip at I2C `0x15`).
The older V1 board uses an SH8601 driver: swap the `panel` line near the top of the sketch (the alternative is in a comment there).

## Setup

1. Install the Arduino IDE (its bundled `arduino-cli` is used) or `arduino-cli` itself, then:

   ```bash
   arduino-cli core install esp32:esp32
   arduino-cli lib install "GFX Library for Arduino" "ArduinoJson" "Adafruit GFX Library"
   ```

2. Put your Wi-Fi details in `.env` (2.4 GHz network only):

   ```bash
   cp .env.example .env
   ```

3. Plug the board in over USB-C and flash:

   ```bash
   ./flash.sh
   ```

`flash.sh` turns `.env` into `windbird_display_1/secrets.h` (both gitignored), compiles and uploads.
`./flash.sh --compile` only compiles. After `secrets.h` exists you can also upload from the Arduino IDE with:
Board "ESP32S3 Dev Module", PSRAM "OPI PSRAM", Flash Size 16MB, USB CDC On Boot "Enabled",
Partition Scheme "16M Flash (3MB APP/9.9MB FATFS)".

## Settings

At the top of `windbird_display_1/windbird_display_1.ino`: station id, timezone, night dimming hours, brightness.
The BOOT button toggles knots / km/h until the next restart.

## Data

Wind data (c) contributors of the OpenWindMap wind network, see <https://developers.pioupiou.fr>.
