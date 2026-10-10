/*
  Windbird desk display
  Board : Waveshare ESP32-S3-Touch-AMOLED-1.8 V2 hardware (CO5300 QSPI AMOLED, 368x448)
          The older V1 board uses an SH8601 driver instead - see the 'panel' line below.
  Data  : OpenWindMap / Pioupiou open API  ->  http://api.pioupiou.fr/v1/live/<id>
          (c) contributors of the Pioupiou/OpenWindMap network, see developers.pioupiou.fr

  Libraries (Arduino Library Manager):
    - "GFX Library for Arduino" by moononournation
    - "ArduinoJson" by Benoit Blanchon (v7)
    - "Adafruit GFX Library" (only used for its nice fonts)

  Arduino IDE > Tools:
    Board: ESP32S3 Dev Module | PSRAM: OPI PSRAM | Flash Size: 16MB | USB CDC On Boot: Enabled

  BOOT button toggles km/h <-> knots.
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_GFX.h>   // only so the IDE finds the Fonts/ folder below
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include "secrets.h"   // WIFI_SSID / WIFI_PASS, generated from .env by flash.sh

// ================= YOUR SETTINGS =================
const int      STATION_ID   = 2176;
const char*    TIMEZONE     = "CET-1CEST,M3.5.0,M10.5.0/3";  // France
const int      NIGHT_START  = 22;   // dim the screen from 22:00...
const int      NIGHT_END    = 7;    // ...until 07:00
const uint8_t  BRIGHT_DAY   = 200;  // 0-255
const uint8_t  BRIGHT_NIGHT = 25;
const uint32_t POLL_MS      = 60000; // API asks for max one request per minute
const int      STALE_MIN    = 15;    // grey out data older than this

// ============ BOARD PINS (Waveshare demo pin_config.h) ============
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK  11
#define LCD_CS    12
#define LCD_W     368
#define LCD_H     448
#define BOOT_BTN  0

#define RGB(r, g, b) ((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3))
const uint16_t C_BLACK = 0x0000;
const uint16_t C_GREY  = RGB(150, 150, 150);
const uint16_t C_DIM   = RGB(70, 70, 70);
const uint16_t C_RED   = RGB(240, 60, 60);

Arduino_DataBus *bus   = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
// V2 board (touch chip at I2C 0x15): CO5300, 16 px column offset, as in Waveshare's arduino-v2 demo.
// V1 board: use  Arduino_SH8601 *panel = new Arduino_SH8601(bus, GFX_NOT_DEFINED, 0, LCD_W, LCD_H);
Arduino_CO5300  *panel = new Arduino_CO5300(bus, GFX_NOT_DEFINED, 0, LCD_W, LCD_H, 16, 0, 0, 0);
Arduino_Canvas  *gfx   = new Arduino_Canvas(LCD_W, LCD_H, panel);  // framebuffer in PSRAM = no flicker

struct Wind {
  bool   valid = false;
  String name;
  float  avg = 0, mn = 0, mx = 0, heading = 0;
  bool   hasHeading = false;
  bool   stationOn = true;
  time_t measured = 0;
} wind;

bool     useKnots = true;   // default unit; BOOT button toggles
String   lastError;
uint32_t lastPoll = 0;

// ---------------- helpers ----------------
// "2026-10-04T12:34:56.000Z" -> Unix time (UTC)
time_t parseIsoUtc(const char* s) {
  int Y, M, D, h, m, sec;
  if (!s || sscanf(s, "%d-%d-%dT%d:%d:%d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
  Y -= M <= 2;
  int era = (Y >= 0 ? Y : Y - 399) / 400;
  unsigned yoe = (unsigned)(Y - era * 400);
  unsigned doy = (153 * (M > 2 ? M - 3 : M + 9) + 2) / 5 + D - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = era * 146097L + (long)doe - 719468L;
  return (time_t)days * 86400 + h * 3600 + m * 60 + sec;
}

const char* cardinal(float deg) {
  static const char* d[16] = {"N","NNE","NE","ENE","E","ESE","SE","SSE",
                              "S","SSW","SW","WSW","W","WNW","NW","NNW"};
  return d[(int)((deg + 11.25f) / 22.5f) % 16];
}

uint16_t speedColor(float kmh) {
  if (kmh < 12) return RGB(0, 210, 130);    // light
  if (kmh < 20) return RGB(230, 220, 40);   // moderate
  if (kmh < 30) return RGB(255, 140, 0);    // strong
  return RGB(240, 50, 50);                  // very strong
}

void drawCentered(const char* txt, int cx, int baseline, const GFXfont* f, uint16_t col, uint8_t size = 1) {
  gfx->setFont(f);
  gfx->setTextSize(size);
  gfx->setTextColor(col);
  int16_t x1, y1; uint16_t w, h;
  gfx->getTextBounds(txt, 0, baseline, &x1, &y1, &w, &h);
  gfx->setCursor(cx - (int)w / 2 - x1, baseline);
  gfx->print(txt);
}

// station names can be wider than the screen: step down a font, then trim with "..."
void drawFitted(const char* txt, int cx, int baseline, uint16_t col) {
  const int maxW = LCD_W - 16;
  int16_t x1, y1; uint16_t w, h;
  gfx->setTextSize(1);
  for (const GFXfont* f : {&FreeSans12pt7b, &FreeSans9pt7b}) {
    gfx->setFont(f);
    gfx->getTextBounds(txt, 0, baseline, &x1, &y1, &w, &h);
    if (w <= maxW) { drawCentered(txt, cx, baseline, f, col); return; }
  }
  String s = txt, t;
  do {
    s.remove(s.length() - 1);
    t = s + "...";
    gfx->getTextBounds(t.c_str(), 0, baseline, &x1, &y1, &w, &h);
  } while (w > maxW && s.length() > 1);
  drawCentered(t.c_str(), cx, baseline, &FreeSans9pt7b, col);
}

// ---------------- data ----------------
bool fetchWind() {
  if (WiFi.status() != WL_CONNECTED) { lastError = "No WiFi"; return false; }

  HTTPClient http;
  http.begin("http://api.pioupiou.fr/v1/live/" + String(STATION_ID));
  http.setTimeout(8000);
  int code = http.GET();
  if (code != 200) { lastError = "HTTP error " + String(code); http.end(); return false; }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) { lastError = "Bad data"; return false; }

  JsonObject d  = doc["data"];
  JsonObject ms = d["measurements"];
  if (ms["wind_speed_avg"].isNull()) { lastError = "No measurements"; return false; }

  wind.name       = d["meta"]["name"] | "Windbird";
  wind.avg        = ms["wind_speed_avg"] | 0.0f;
  wind.mn         = ms["wind_speed_min"] | 0.0f;
  wind.mx         = ms["wind_speed_max"] | 0.0f;
  wind.hasHeading = !ms["wind_heading"].isNull();
  wind.heading    = ms["wind_heading"] | 0.0f;
  wind.measured   = parseIsoUtc(ms["date"].as<const char*>());
  const char* st  = d["status"]["state"];
  wind.stationOn  = !(st && strcmp(st, "off") == 0);
  wind.valid      = true;
  lastError       = "";
  return true;
}

// ---------------- drawing ----------------
void render() {
  // shift everything a few pixels each refresh to limit AMOLED burn-in
  int ox = random(-4, 5), oy = random(-4, 5);
  int cx = LCD_W / 2 + ox, cy = 200 + oy, R = 130;
  char buf[48];

  gfx->fillScreen(C_BLACK);

  if (!wind.valid) {
    drawCentered(lastError.length() ? lastError.c_str() : "Loading...", cx, cy, &FreeSans12pt7b, C_GREY);
    gfx->flush();
    return;
  }

  time_t now = time(nullptr);
  long ageMin = (now > 1700000000 && wind.measured) ? (long)(now - wind.measured) / 60 : -1;
  bool stale = !wind.stationOn || ageMin > STALE_MIN;
  uint16_t accent = stale ? C_GREY : speedColor(wind.avg);
  float k = useKnots ? 1.852f : 1.0f;

  // station name
  drawFitted(wind.name.c_str(), cx, 32 + oy, C_GREY);

  // compass ring with ticks
  gfx->drawCircle(cx, cy, R, C_DIM);
  gfx->drawCircle(cx, cy, R - 1, C_DIM);
  for (int a = 0; a < 360; a += 30) {
    float t = a * DEG_TO_RAD;
    int r1 = (a % 90 == 0) ? R - 14 : R - 8;
    gfx->drawLine(cx + R * sin(t), cy - R * cos(t), cx + r1 * sin(t), cy - r1 * cos(t), C_DIM);
  }
  const char* nesw[4] = {"N", "E", "S", "W"};
  for (int i = 0; i < 4; i++) {
    float t = i * 90 * DEG_TO_RAD;
    drawCentered(nesw[i], cx + (R - 32) * sin(t), cy - (R - 32) * cos(t) + 8, &FreeSans12pt7b, C_DIM);
  }

  // wind marker: sits on the ring where the wind comes FROM, pointing inward
  if (wind.hasHeading) {
    float t = wind.heading * DEG_TO_RAD;
    float spread = 0.15f;
    int tipX = cx + (R - 20) * sin(t),          tipY = cy - (R - 20) * cos(t);
    int b1X  = cx + (R + 14) * sin(t - spread), b1Y  = cy - (R + 14) * cos(t - spread);
    int b2X  = cx + (R + 14) * sin(t + spread), b2Y  = cy - (R + 14) * cos(t + spread);
    gfx->fillTriangle(tipX, tipY, b1X, b1Y, b2X, b2Y, accent);
  }

  // big average speed
  // whole part large, decimal at half size so two-digit speeds still fit inside the ring
  long tenths = lround(wind.avg / k * 10);
  char whole[8], frac[4];
  snprintf(whole, sizeof whole, "%ld", tenths / 10);
  snprintf(frac, sizeof frac, ".%ld", tenths % 10);
  int16_t bx, bby; uint16_t wW, wF, bh;
  gfx->setFont(&FreeSansBold24pt7b);
  gfx->setTextColor(accent);
  gfx->setTextSize(2); gfx->getTextBounds(whole, 0, 0, &bx, &bby, &wW, &bh);
  gfx->setTextSize(1); gfx->getTextBounds(frac, 0, 0, &bx, &bby, &wF, &bh);
  int left = cx - (wW + 6 + wF) / 2;
  gfx->setTextSize(2); gfx->setCursor(left, cy + 30);          gfx->print(whole);
  gfx->setTextSize(1); gfx->setCursor(left + wW + 6, cy + 30); gfx->print(frac);
  drawCentered(useKnots ? "knots" : "km/h", cx, cy + 64, &FreeSans12pt7b, C_GREY);

  // direction, min/max, age
  int by = cy + R + 42;
  if (wind.hasHeading) {
    snprintf(buf, sizeof buf, "from %s (%d)", cardinal(wind.heading), (int)lround(wind.heading));
    drawCentered(buf, cx, by, &FreeSansBold18pt7b, C_GREY);
  }
  snprintf(buf, sizeof buf, "min %.1f   max %.1f", wind.mn / k, wind.mx / k);
  drawCentered(buf, cx, by + 32, &FreeSans12pt7b, C_GREY);

  if (!wind.stationOn)      snprintf(buf, sizeof buf, "station switched off");
  else if (ageMin < 0)      snprintf(buf, sizeof buf, " ");
  else if (ageMin == 0)     snprintf(buf, sizeof buf, "just now");
  else                      snprintf(buf, sizeof buf, "%ld min ago", ageMin);
  drawCentered(buf, cx, by + 62, &FreeSans12pt7b, stale ? C_RED : C_DIM);

  gfx->flush();
}

void updateBrightness() {
  struct tm lt;
  if (!getLocalTime(&lt, 10)) return;
  bool night = lt.tm_hour >= NIGHT_START || lt.tm_hour < NIGHT_END;
  panel->setBrightness(night ? BRIGHT_NIGHT : BRIGHT_DAY);
}

// ---------------- main ----------------
void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BTN, INPUT_PULLUP);

  if (!gfx->begin()) Serial.println("Display init failed - check PSRAM setting and pins");
  panel->setBrightness(BRIGHT_DAY);
  gfx->setTextWrap(false);  // we centre text ourselves; wrapping would break the width measurements

  gfx->fillScreen(C_BLACK);
  drawCentered("Connecting to WiFi...", LCD_W / 2, LCD_H / 2, &FreeSans12pt7b, C_GREY);
  gfx->flush();

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);

  configTzTime(TIMEZONE, "pool.ntp.org", "time.google.com");
  delay(1500);  // give NTP a moment so "min ago" is right on first screen

  fetchWind();
  updateBrightness();
  render();
  lastPoll = millis();
}

void loop() {
  static bool lastBtn = HIGH;
  bool btn = digitalRead(BOOT_BTN);
  if (lastBtn == HIGH && btn == LOW) {
    useKnots = !useKnots;
    render();
    delay(50);
  }
  lastBtn = btn;

  if (millis() - lastPoll >= POLL_MS) {
    lastPoll = millis();
    if (WiFi.status() != WL_CONNECTED) WiFi.reconnect();
    fetchWind();          // on failure the last reading stays, and its age keeps counting up
    updateBrightness();
    render();
  }
  delay(20);
}
