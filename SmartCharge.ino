/*
  ============================================================
  SMART CHARGE PRO v8 CLOUD ONLY - NO AP / NO LOCAL IP QR
  ============================================================

  Alur utama:
  1. User tekan tombol S0 -> GPIO13, seperti project parkir. INPUT_PULLUP, LOW = ditekan.
  2. ESP32 membuat ticket token.
  3. Thermal Bluetooth printer mencetak tiket berisi QR.
  4. QR selalu berisi URL publik/domain cloud, bukan IP laptop dan bukan IP AP ESP32.
     Contoh: https://charge.domainkamu.com/pay?device=SC001&ticket=SC001-ABC123
  5. User scan QR -> buka web cloud -> klik payment custom -> cloud publish MQTT command.
  6. ESP32 menerima command -> relay port menyala -> timer berjalan.

  Mode versi ini:
  - CLOUD ONLY.
  - AP/captive portal ESP32 dimatikan.
  - Tidak ada fallback ke alamat AP ESP32 lama.
  - Jika WiFi/MQTT/cloud belum siap, tiket tidak akan dicetak supaya user tidak diarahkan ke AP.

  Library Arduino IDE yang dibutuhkan:
  - PubSubClient by Nick O'Leary
  - BluetoothSerial bawaan Arduino-ESP32, hanya untuk chip ESP32 yang mendukung Bluetooth Classic SPP.

  Catatan tombol v8:
  - Default tombol print memakai GPIO13, sama seperti project parkir kamu.
  - Wiring paling simpel: GPIO13 -> tombol -> GND.
  - Firmware memakai INPUT_PULLUP, jadi LOW = ditekan.
  - Jangan pakai GPIO34 untuk tombol polos tanpa resistor, karena GPIO34 tidak punya pull-up/pull-down internal dan gampang floating.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h>
#include <BluetoothSerial.h>
#include <esp_system.h>
#if __has_include(<qrcode.h>)
#include <qrcode.h>
#define SMARTCHARGE_HAS_QRCODE_LIB 1
#else
#define SMARTCHARGE_HAS_QRCODE_LIB 0
#endif
#if __has_include(<esp_arduino_version.h>)
#include <esp_arduino_version.h>
#endif

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BLUEDROID_ENABLED)
#error Bluetooth Classic belum aktif. Pakai board ESP32 classic, bukan ESP32-C3/C6. Untuk ESP32-S3, pastikan core mendukung BT Classic SPP.
#endif

#if !defined(CONFIG_BT_SPP_ENABLED)
#error Bluetooth Serial Port Profile/SPP tidak tersedia. Printer Bluetooth Classic butuh SPP.
#endif

// ============================================================
// KONFIGURASI UTAMA
// ============================================================

const char *DEVICE_ID = "SC001";

// ---------- Mode lokal/AP ----------
// Versi ini sengaja CLOUD ONLY.
// AP/captive portal ESP32 tidak dinyalakan sama sekali.

// ---------- WiFi internet untuk cloud/MQTT ----------
// Isi dengan router/modem 4G di lokasi alat. Jangan bergantung ke hotspot HP user.
const bool CLOUD_MQTT_ENABLED = true;
const char *STA_SSID = "VIVA9";
const char *STA_PASSWORD = "1sampai8";

// URL web cloud yang akan masuk ke QR thermal.
// Harus bisa dibuka dari HP user lewat internet.
// Contoh produksi: https://charge.domainkamu.com
const char *PUBLIC_WEB_BASE_URL = "https://smart-charge-tau.vercel.app";

// ---------- MQTT cloud ----------
// Jangan isi IP laptop 192.168.x.x di sini kalau mau alat berdiri sendiri.
// Pakai broker cloud/VPS yang selalu online, lebih bagus pakai domain.
const char *MQTT_HOST = "test.mosquitto.org";
const uint16_t MQTT_PORT = 1883;
const char *MQTT_USER = "";
const char *MQTT_PASSWORD = "";

// ---------- Printer Bluetooth thermal ----------
const char *BT_LOCAL_NAME = "SmartCharge-ESP32";
const char *PRINTER_BT_DEVICE_NAME = "RPP02N";
const char *PRINTER_BT_MAC = "06:2B:C5:F6:B2:82";  // MAC printer dari self-test. Kosongkan kalau mau connect by name.
const char *PRINTER_BT_PIN = "0000";   // coba "1234" kalau printer menolak pairing

// ---------- Tombol print ticket ----------
// Dibuat sama seperti project parkir kamu:
// S0 = GPIO13, INPUT_PULLUP, LOW = ditekan.
// Wiring tombol polos: GPIO13 -> tombol -> GND.
// Jangan pakai GPIO34 untuk tombol polos tanpa resistor, karena GPIO34 sering floating.
const uint8_t PRINT_BUTTON_PIN = 13;    // S0 -> GPIO13
const bool BUTTON_USE_INTERNAL_PULLUP = true;
const uint8_t BUTTON_PRESSED_LEVEL = LOW;
const unsigned long BUTTON_DEBOUNCE_MS = 120;
const unsigned long PRINT_COOLDOWN_MS = 3000;

// ---------- QR thermal raster ----------
// RPP02N sering lebih stabil kalau QR dikirim sebagai bitmap/raster, seperti project parkir.
const uint16_t PRINTER_DOT_WIDTH = 384;     // 58mm thermal umum: 384 dot
const uint8_t QR_RASTER_MAX_VERSION = 10;   // cukup untuk URL cloud trainer
const uint8_t QR_RASTER_SCALE = 5;          // ukuran modul QR di kertas, sama seperti project parkir
const uint8_t QR_RASTER_QUIET_ZONE = 4;     // border putih wajib agar scanner mudah baca
const bool USE_RASTER_QR = true;            // true = bitmap/raster, false = native ESC/POS QR

// ---------- Relay port 1-8 ----------
const bool RELAY_ACTIVE_LOW = true;
const uint8_t RELAY_PINS[8] = {
  26,  // Port 1
  27,  // Port 2
  25,  // Port 3
  33,  // Port 4
  32,  // Port 5
  18,  // Port 6
  19,  // Port 7
  23   // Port 8
};

struct PackageOption {
  uint16_t minutes;
  uint32_t price;
};

PackageOption packages[] = {
  {30, 3000},
  {60, 5000},
  {120, 8000}
};
const uint8_t PACKAGE_COUNT = sizeof(packages) / sizeof(packages[0]);

// Ticket berlaku 15 menit setelah dicetak.
const unsigned long TICKET_VALID_MS = 15UL * 60UL * 1000UL;
const uint8_t MAX_TICKETS = 20;

// ============================================================
// OBJECT GLOBAL
// ============================================================

WebServer server(80);
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
BluetoothSerial SerialBT;

struct PortState {
  bool active;
  unsigned long startMs;
  unsigned long durationMs;
  String transactionId;
  uint32_t price;
  uint16_t minutes;
  String source;
};

struct TicketState {
  bool valid;
  bool used;
  bool cloud;
  unsigned long createdMs;
  String token;
  String url;
};

PortState ports[8];
TicketState tickets[MAX_TICKETS];
uint8_t ticketWriteIndex = 0;

unsigned long lastTimerCheck = 0;
unsigned long lastStatusPublishMs = 0;
unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastWifiReconnectAttempt = 0;
unsigned long lastStaDiagMs = 0;
unsigned long lastPrintMs = 0;
uint32_t transactionCounter = 0;

bool printerBtStartedFlag = false;
bool printerConnected = false;
String lastTicketToken = "";
String lastTicketUrl = "";

uint8_t lastButtonReading = HIGH;
uint8_t stableButtonState = HIGH;
bool buttonPressedLatched = false;
unsigned long lastButtonChangeMs = 0;

bool mqttMessagePending = false;
String pendingMqttTopic = "";
String pendingMqttPayload = "";

// Serial monitor dibuat ringkas: hanya event penting, bukan log loop/retry.
bool wifiWasOnline = false;
bool mqttWasOnline = false;
int lastMqttFailState = 999;
unsigned long lastMqttFailLogMs = 0;

// ============================================================
// PROTOTYPES
// ============================================================

void publishEvent(const String &event);
void publishStatus(bool retained = false);
void createAndPrintTicket();
void activatePort(uint8_t idx, uint16_t minutes, uint32_t price, const String &tx, const String &source);
void deactivatePort(uint8_t idx);

// ============================================================
// HELPER DASAR
// ============================================================

void relayWrite(uint8_t idx, bool on) {
  if (idx >= 8) return;
  const uint8_t pin = RELAY_PINS[idx];
  if (RELAY_ACTIVE_LOW) digitalWrite(pin, on ? LOW : HIGH);
  else digitalWrite(pin, on ? HIGH : LOW);
}

void setupRelays() {
  for (uint8_t i = 0; i < 8; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    relayWrite(i, false);
    ports[i].active = false;
    ports[i].startMs = 0;
    ports[i].durationMs = 0;
    ports[i].transactionId = "";
    ports[i].price = 0;
    ports[i].minutes = 0;
    ports[i].source = "";
  }
}

String htmlEscape(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': out += F("&amp;"); break;
      case '<': out += F("&lt;"); break;
      case '>': out += F("&gt;"); break;
      case '"': out += F("&quot;"); break;
      case '\'': out += F("&#39;"); break;
      default: out += c; break;
    }
  }
  return out;
}

String rupiah(uint32_t amount) {
  String s = String(amount);
  String out = "";
  int count = 0;
  for (int i = s.length() - 1; i >= 0; i--) {
    out = String(s[i]) + out;
    count++;
    if (count == 3 && i > 0) {
      out = String(".") + out;
      count = 0;
    }
  }
  return "Rp " + out;
}

String twoDigits(uint32_t n) {
  if (n < 10) return "0" + String(n);
  return String(n);
}

String formatMs(unsigned long ms) {
  uint32_t totalSec = ms / 1000UL;
  uint32_t h = totalSec / 3600UL;
  uint32_t m = (totalSec % 3600UL) / 60UL;
  uint32_t s = totalSec % 60UL;
  if (h > 0) return String(h) + ":" + twoDigits(m) + ":" + twoDigits(s);
  return twoDigits(m) + ":" + twoDigits(s);
}

String remainingTime(uint8_t idx) {
  if (idx >= 8 || !ports[idx].active) return "-";
  unsigned long elapsed = millis() - ports[idx].startMs;
  if (elapsed >= ports[idx].durationMs) return "00:00";
  return formatMs(ports[idx].durationMs - elapsed);
}

String makeToken() {
  // Dibuat lebih pendek supaya QR thermal tidak terlalu padat dan lebih mudah discan.
  transactionCounter++;
  uint32_t r = esp_random() ^ ((uint32_t)millis() << 7) ^ transactionCounter;
  char buf[32];
  snprintf(buf, sizeof(buf), "%s-%08lX", DEVICE_ID, (unsigned long)r);
  return String(buf);
}

String normalizeBaseUrl(const char *baseUrl) {
  String base = String(baseUrl);
  while (base.endsWith("/")) base.remove(base.length() - 1);
  return base;
}

String extractHostFromUrl(String url) {
  url.trim();
  int start = url.indexOf("://");
  start = (start >= 0) ? start + 3 : 0;

  int end = url.length();
  for (int i = start; i < (int)url.length(); i++) {
    char c = url[i];
    if (c == '/' || c == '?' || c == '#') {
      end = i;
      break;
    }
  }

  String host = url.substring(start, end);
  int at = host.lastIndexOf('@');
  if (at >= 0) host = host.substring(at + 1);

  // IPv6 bracket, misalnya https://[::1]:3000
  if (host.startsWith("[")) {
    int closePos = host.indexOf(']');
    if (closePos > 0) host = host.substring(1, closePos);
  } else {
    int colon = host.indexOf(':');
    if (colon >= 0) host = host.substring(0, colon);
  }

  host.toLowerCase();
  host.trim();
  return host;
}

bool parseIpv4Literal(const String &host, uint8_t out[4]) {
  int partIndex = 0;
  int start = 0;

  for (int i = 0; i <= (int)host.length(); i++) {
    if (i == (int)host.length() || host[i] == '.') {
      if (partIndex >= 4) return false;
      if (i == start) return false;

      int value = 0;
      for (int j = start; j < i; j++) {
        char c = host[j];
        if (c < '0' || c > '9') return false;
        value = value * 10 + (c - '0');
        if (value > 255) return false;
      }

      out[partIndex++] = (uint8_t)value;
      start = i + 1;
    }
  }

  return partIndex == 4;
}

bool isIpv4Literal(const String &host) {
  uint8_t ip[4];
  return parseIpv4Literal(host, ip);
}

bool isPrivateOrLocalIpv4(const String &host) {
  uint8_t ip[4];
  if (!parseIpv4Literal(host, ip)) return false;

  if (ip[0] == 10) return true;
  if (ip[0] == 127) return true;
  if (ip[0] == 0) return true;
  if (ip[0] == 169 && ip[1] == 254) return true;
  if (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31) return true;
  if (ip[0] == 192 && ip[1] == 168) return true;
  return false;
}

bool publicWebUrlConfigured() {
  String base = normalizeBaseUrl(PUBLIC_WEB_BASE_URL);
  String lower = base;
  lower.toLowerCase();

  if (base.length() == 0) return false;
  if (!(lower.startsWith("https://") || lower.startsWith("http://"))) return false;
  if (lower.indexOf("example.com") >= 0) return false;
  if (lower.indexOf("isi-domain") >= 0) return false;
  if (lower.indexOf("domain-kamu") >= 0) return false;

  String host = extractHostFromUrl(base);
  if (host.length() == 0) return false;
  if (host == "localhost" || host.endsWith(".local")) return false;

  // QR cloud wajib memakai domain, bukan angka IP.
  if (isIpv4Literal(host)) return false;
  return true;
}

bool mqttHostConfiguredForCloud() {
  String host = String(MQTT_HOST);
  host.trim();
  host.toLowerCase();

  if (host.length() == 0) return false;
  if (host.indexOf("isi-domain") >= 0) return false;
  if (host.indexOf("domain-kamu") >= 0) return false;
  if (host == "localhost" || host.endsWith(".local")) return false;

  // Ini mencegah alat bergantung ke IP laptop/router lokal seperti 192.168.x.x.
  if (isPrivateOrLocalIpv4(host)) return false;
  return true;
}

bool cloudReadyToPrint() {
  return CLOUD_MQTT_ENABLED &&
         publicWebUrlConfigured() &&
         mqttHostConfiguredForCloud() &&
         (WiFi.status() == WL_CONNECTED) &&
         mqttClient.connected();
}

String buildCloudUrl(const String &token) {
  String url = normalizeBaseUrl(PUBLIC_WEB_BASE_URL);
  url += "/pay?device=";
  url += DEVICE_ID;
  url += "&ticket=";
  url += token;
  return url;
}

uint8_t connectedClients() {
  return 0;
}

// ============================================================
// TICKET STORE
// ============================================================

void clearTickets() {
  for (uint8_t i = 0; i < MAX_TICKETS; i++) {
    tickets[i].valid = false;
    tickets[i].used = false;
    tickets[i].cloud = false;
    tickets[i].createdMs = 0;
    tickets[i].token = "";
    tickets[i].url = "";
  }
}

void cleanupExpiredTickets() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < MAX_TICKETS; i++) {
    if (tickets[i].valid && !tickets[i].used && (now - tickets[i].createdMs > TICKET_VALID_MS)) {
      tickets[i].valid = false;
      tickets[i].token = "";
      tickets[i].url = "";
    }
  }
}

TicketState *findTicket(const String &token) {
  cleanupExpiredTickets();
  for (uint8_t i = 0; i < MAX_TICKETS; i++) {
    if (tickets[i].valid && tickets[i].token == token) return &tickets[i];
  }
  return nullptr;
}

bool ticketCanBeUsed(const String &token) {
  TicketState *t = findTicket(token);
  if (t == nullptr) return false;
  if (t->used) return false;
  if (millis() - t->createdMs > TICKET_VALID_MS) return false;
  return true;
}

void markTicketUsed(const String &token) {
  TicketState *t = findTicket(token);
  if (t != nullptr) t->used = true;
}

TicketState *addTicket(const String &token, const String &url, bool cloud) {
  TicketState *t = &tickets[ticketWriteIndex];
  ticketWriteIndex = (ticketWriteIndex + 1) % MAX_TICKETS;

  t->valid = true;
  t->used = false;
  t->cloud = cloud;
  t->createdMs = millis();
  t->token = token;
  t->url = url;
  return t;
}

// ============================================================
// PRINTER ESC/POS
// ============================================================

bool setBluetoothPinCompat(const char *pin) {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  return SerialBT.setPin(pin, (uint8_t)strlen(pin));
#else
  return SerialBT.setPin(pin);
#endif
}

bool setupBluetoothPrinter() {
  if (printerBtStartedFlag) return true;
  if (!SerialBT.begin(BT_LOCAL_NAME, true)) {
    Serial.println(F("[ERROR] Bluetooth printer gagal start"));
    return false;
  }
  printerBtStartedFlag = true;

  if (strlen(PRINTER_BT_PIN) > 0) {
    setBluetoothPinCompat(PRINTER_BT_PIN);
  }
  return true;
}

bool printerEnsureConnected() {
  if (!setupBluetoothPrinter()) return false;
  if (SerialBT.hasClient()) {
    printerConnected = true;
    return true;
  }

  printerConnected = false;
  SerialBT.disconnect();
  delay(300);

  bool ok = false;
  if (strlen(PRINTER_BT_MAC) > 0) {
    ok = SerialBT.connect(BTAddress(String(PRINTER_BT_MAC)));
  }

  if (!ok && strlen(PRINTER_BT_DEVICE_NAME) > 0) {
    ok = SerialBT.connect(String(PRINTER_BT_DEVICE_NAME));
  }

  printerConnected = ok && SerialBT.hasClient();
  if (printerConnected) Serial.println(F("[PRINTER] Terhubung"));
  else Serial.println(F("[ERROR] Printer tidak terhubung"));
  return printerConnected;
}

void pbytes(const uint8_t *data, size_t len) {
  SerialBT.write(data, len);
  delay(20);
}

void pbyte(uint8_t b) {
  SerialBT.write(b);
}

void printerInit() {
  const uint8_t cmd[] = {0x1B, 0x40};
  pbytes(cmd, sizeof(cmd));
}

void printerAlign(uint8_t align) {
  const uint8_t cmd[] = {0x1B, 0x61, align};
  pbytes(cmd, sizeof(cmd));
}

void printerBold(bool on) {
  const uint8_t cmd[] = {0x1B, 0x45, (uint8_t)(on ? 1 : 0)};
  pbytes(cmd, sizeof(cmd));
}

void printerTextSize(uint8_t n) {
  const uint8_t cmd[] = {0x1D, 0x21, n};
  pbytes(cmd, sizeof(cmd));
}

void printerFeed(uint8_t lines) {
  for (uint8_t i = 0; i < lines; i++) SerialBT.print("\n");
  delay(30);
}

void printerLine() {
  SerialBT.println(F("--------------------------------"));
}

void printerNativeQR(const String &data) {
  // ESC/POS QR Code: GS ( k. Sebagian RPP02N tidak support penuh,
  // jadi fungsi ini hanya fallback kalau raster dimatikan.
  const uint8_t model[] = {0x1D, 0x28, 0x6B, 0x04, 0x00, 0x31, 0x41, 0x32, 0x00};
  const uint8_t size[]  = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x43, 0x06};
  const uint8_t error[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x45, 0x31};
  pbytes(model, sizeof(model));
  pbytes(size, sizeof(size));
  pbytes(error, sizeof(error));

  uint16_t len = data.length() + 3;
  uint8_t pL = len & 0xFF;
  uint8_t pH = (len >> 8) & 0xFF;
  const uint8_t storeHead[] = {0x1D, 0x28, 0x6B, pL, pH, 0x31, 0x50, 0x30};
  pbytes(storeHead, sizeof(storeHead));
  SerialBT.print(data);
  delay(100);

  const uint8_t printCmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x51, 0x30};
  pbytes(printCmd, sizeof(printCmd));
  delay(600);
}

void printerRasterHeader(uint16_t widthDots, uint16_t heightDots) {
  uint16_t widthBytes = (widthDots + 7) / 8;
  uint8_t xL = widthBytes & 0xFF;
  uint8_t xH = (widthBytes >> 8) & 0xFF;
  uint8_t yL = heightDots & 0xFF;
  uint8_t yH = (heightDots >> 8) & 0xFF;
  // GS v 0: print raster bit image.
  const uint8_t header[] = {0x1D, 0x76, 0x30, 0x00, xL, xH, yL, yH};
  pbytes(header, sizeof(header));
}

#if SMARTCHARGE_HAS_QRCODE_LIB && defined(ESP_QRCODE_CONFIG_DEFAULT)
void printerRasterQrEspressifCallback(esp_qrcode_handle_t qrcode) {
  const int qrSize = esp_qrcode_get_size(qrcode);
  const int qrPixelSize = (qrSize + (QR_RASTER_QUIET_ZONE * 2)) * QR_RASTER_SCALE;
  const int rasterWidthDots = PRINTER_DOT_WIDTH;
  const int rasterWidthBytes = rasterWidthDots / 8;
  const int rasterHeightDots = qrPixelSize;
  const int leftMargin = (rasterWidthDots > qrPixelSize) ? ((rasterWidthDots - qrPixelSize) / 2) : 0;

  printerRasterHeader(rasterWidthDots, rasterHeightDots);

  uint8_t row[384 / 8];
  for (int py = 0; py < rasterHeightDots; py++) {
    memset(row, 0, sizeof(row));

    for (int px = 0; px < rasterWidthDots; px++) {
      bool black = false;
      int localX = px - leftMargin;

      if (localX >= 0 && localX < qrPixelSize) {
        int moduleX = (localX / QR_RASTER_SCALE) - QR_RASTER_QUIET_ZONE;
        int moduleY = (py / QR_RASTER_SCALE) - QR_RASTER_QUIET_ZONE;

        if (moduleX >= 0 && moduleX < qrSize && moduleY >= 0 && moduleY < qrSize) {
          black = esp_qrcode_get_module(qrcode, moduleX, moduleY);
        }
      }

      if (black) row[px / 8] |= (uint8_t)(0x80 >> (px & 7));
    }

    SerialBT.write(row, rasterWidthBytes);
    if ((py & 0x03) == 0) delay(1);  // buffer Bluetooth printer kecil, beri napas
  }
  delay(250);
}
#endif

bool printerRasterQR(const String &data) {
#if SMARTCHARGE_HAS_QRCODE_LIB
#if defined(ESP_QRCODE_CONFIG_DEFAULT)
  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = printerRasterQrEspressifCallback;
  cfg.max_qrcode_version = QR_RASTER_MAX_VERSION;
  cfg.qrcode_ecc_level = ESP_QRCODE_ECC_LOW;

  esp_err_t err = esp_qrcode_generate(&cfg, data.c_str());
  if (err != ESP_OK) {
    Serial.println(F("[ERROR] QR raster gagal"));
    return false;
  }
  return true;
#else
  // Fallback untuk library QRCode by ricmoo/serupa.
  QRCode qrcode;
  uint8_t qrcodeData[900];
  int8_t err = qrcode_initText(&qrcode, qrcodeData, QR_RASTER_MAX_VERSION, ECC_LOW, data.c_str());
  if (err != 0) {
    Serial.println(F("[ERROR] QR raster gagal init"));
    return false;
  }

  const int qrSize = qrcode.size;
  const int qrPixelSize = (qrSize + (QR_RASTER_QUIET_ZONE * 2)) * QR_RASTER_SCALE;
  const int rasterWidthDots = PRINTER_DOT_WIDTH;
  const int rasterWidthBytes = rasterWidthDots / 8;
  const int rasterHeightDots = qrPixelSize;
  const int leftMargin = (rasterWidthDots > qrPixelSize) ? ((rasterWidthDots - qrPixelSize) / 2) : 0;

  printerRasterHeader(rasterWidthDots, rasterHeightDots);

  uint8_t row[384 / 8];
  for (int py = 0; py < rasterHeightDots; py++) {
    memset(row, 0, sizeof(row));

    for (int px = 0; px < rasterWidthDots; px++) {
      bool black = false;
      int localX = px - leftMargin;

      if (localX >= 0 && localX < qrPixelSize) {
        int moduleX = (localX / QR_RASTER_SCALE) - QR_RASTER_QUIET_ZONE;
        int moduleY = (py / QR_RASTER_SCALE) - QR_RASTER_QUIET_ZONE;

        if (moduleX >= 0 && moduleX < qrSize && moduleY >= 0 && moduleY < qrSize) {
          black = qrcode_getModule(&qrcode, moduleX, moduleY);
        }
      }

      if (black) row[px / 8] |= (uint8_t)(0x80 >> (px & 7));
    }

    SerialBT.write(row, rasterWidthBytes);
    if ((py & 0x03) == 0) delay(1);
  }
  delay(250);
  return true;
#endif
#else
  (void)data;
  Serial.println(F("[INFO] QR raster library tidak ditemukan, pakai fallback printer"));
  return false;
#endif
}

void printerQR(const String &data) {
  if (USE_RASTER_QR) {
    if (printerRasterQR(data)) return;
  }
  printerNativeQR(data);
}

bool printTicket(const String &token, const String &url, bool cloud) {
  if (!printerEnsureConnected()) {
    Serial.println(F("[ERROR] Ticket tidak tercetak"));
    return false;
  }

  printerInit();
  printerAlign(1);
  printerBold(true);
  printerTextSize(0x11);
  SerialBT.println(F("SMARTCHARGE"));
  printerTextSize(0x00);
  printerBold(false);
  (void)cloud;
  SerialBT.println(F("Scan QR untuk bayar"));
  SerialBT.println(F("Arahkan kamera ke QR"));
  printerFeed(1);

  printerQR(url);
  printerFeed(1);

  printerAlign(0);
  printerLine();
  SerialBT.print(F("Device : "));
  SerialBT.println(DEVICE_ID);
  SerialBT.print(F("Ticket : "));
  SerialBT.println(token);
  SerialBT.print(F("Mode   : "));
  SerialBT.println(F("CLOUD MQTT"));
  printerLine();

  SerialBT.println(F("1. Scan QR"));
  SerialBT.println(F("2. Pilih port & paket"));
  SerialBT.println(F("3. Klik bayar"));
  SerialBT.println(F("4. Colok HP ke port pilihan"));

  printerLine();
  SerialBT.println(url);
  printerFeed(4);
  SerialBT.flush();
  Serial.println(F("[PRINT] QR ticket tercetak"));
  return true;
}

// ============================================================
// PORT TIMER
// ============================================================

void activatePort(uint8_t idx, uint16_t minutes, uint32_t price, const String &tx, const String &source) {
  if (idx >= 8 || minutes == 0) return;
  if (ports[idx].active) return;

  ports[idx].active = true;
  ports[idx].startMs = millis();
  ports[idx].durationMs = (unsigned long)minutes * 60UL * 1000UL;
  ports[idx].minutes = minutes;
  ports[idx].price = price;
  ports[idx].transactionId = tx;
  ports[idx].source = source;
  relayWrite(idx, true);

  Serial.printf("[CHARGE] Port %u ON (%u menit)\n", idx + 1, minutes);
  publishEvent(String("STARTED|") + tx + "|port=" + String(idx + 1) + "|minutes=" + String(minutes));
  publishStatus(false);
}

void deactivatePort(uint8_t idx) {
  if (idx >= 8) return;
  relayWrite(idx, false);
  if (ports[idx].active) {
    Serial.printf("[CHARGE] Port %u OFF\n", idx + 1);
    publishEvent(String("STOPPED|") + ports[idx].transactionId + "|port=" + String(idx + 1));
  }
  ports[idx].active = false;
  ports[idx].startMs = 0;
  ports[idx].durationMs = 0;
  ports[idx].transactionId = "";
  ports[idx].price = 0;
  ports[idx].minutes = 0;
  ports[idx].source = "";
  publishStatus(false);
}

void checkTimers() {
  unsigned long now = millis();
  if (now - lastTimerCheck < 500) return;
  lastTimerCheck = now;
  for (uint8_t i = 0; i < 8; i++) {
    if (ports[i].active && (now - ports[i].startMs >= ports[i].durationMs)) {
      deactivatePort(i);
    }
  }
}

// ============================================================
// MQTT
// ============================================================

String topicPrefix() {
  String t = "smartcharge/";
  t += DEVICE_ID;
  return t;
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  pendingMqttTopic = String(topic);
  pendingMqttPayload = "";
  pendingMqttPayload.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) pendingMqttPayload += (char)payload[i];
  mqttMessagePending = true;
}

int splitPipe(const String &s, String fields[], int maxFields) {
  int count = 0;
  int start = 0;
  while (count < maxFields) {
    int pos = s.indexOf('|', start);
    if (pos < 0) {
      fields[count++] = s.substring(start);
      break;
    }
    fields[count++] = s.substring(start, pos);
    start = pos + 1;
  }
  return count;
}

void publishEvent(const String &event) {
  if (!CLOUD_MQTT_ENABLED || !mqttClient.connected()) return;
  String topic = topicPrefix() + "/event";
  mqttClient.publish(topic.c_str(), event.c_str(), false);
}

void publishTicketMqtt(const TicketState &ticket) {
  if (!CLOUD_MQTT_ENABLED || !mqttClient.connected()) return;
  String topic = topicPrefix() + "/ticket";
  String payload = "TICKET|";
  payload += ticket.token;
  payload += "|";
  payload += ticket.url;
  mqttClient.publish(topic.c_str(), payload.c_str(), false);
  publishEvent(String("TICKET_PRINTED|") + ticket.token + "|" + (ticket.cloud ? "cloud" : "local"));
}

void publishStatus(bool retained) {
  if (!CLOUD_MQTT_ENABLED || !mqttClient.connected()) return;
  String topic = topicPrefix() + "/status";
  String payload = "STATUS|uptime=";
  payload += String(millis());
  payload += "|sta=";
  payload += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("offline"));
  payload += "|ap=disabled";
  payload += "|clients=0";
  payload += "|ports=";
  for (uint8_t i = 0; i < 8; i++) {
    if (i) payload += ",";
    payload += ports[i].active ? "1" : "0";
  }
  mqttClient.publish(topic.c_str(), payload.c_str(), retained);
}

void processMqttCommand(const String &payload) {
  String f[6];
  int n = splitPipe(payload, f, 6);
  if (n <= 0) return;

  f[0].toUpperCase();

  if (f[0] == "START") {
    // START|ticket|port|minutes|price
    if (n < 5) {
      publishEvent(String("REJECT|bad_format|") + payload);
      return;
    }
    String ticket = f[1];
    int port = f[2].toInt();
    int minutes = f[3].toInt();
    uint32_t price = (uint32_t)f[4].toInt();

    if (port < 1 || port > 8 || minutes <= 0) {
      publishEvent(String("REJECT|bad_param|") + ticket);
      return;
    }
    if (!ticketCanBeUsed(ticket)) {
      publishEvent(String("REJECT|invalid_or_used_ticket|") + ticket);
      return;
    }
    if (ports[port - 1].active) {
      publishEvent(String("REJECT|port_busy|") + ticket + "|port=" + String(port));
      return;
    }

    markTicketUsed(ticket);
    activatePort((uint8_t)(port - 1), (uint16_t)minutes, price, ticket, "mqtt");
    return;
  }

  if (f[0] == "STOP") {
    // STOP|port
    if (n < 2) return;
    int port = f[1].toInt();
    if (port >= 1 && port <= 8) deactivatePort((uint8_t)(port - 1));
    return;
  }

  if (f[0] == "STOPALL") {
    for (uint8_t i = 0; i < 8; i++) deactivatePort(i);
    return;
  }

  if (f[0] == "PRINT") {
    createAndPrintTicket();
    return;
  }

  publishEvent(String("REJECT|unknown_cmd|") + payload);
}

void processPendingMqttMessage() {
  if (!mqttMessagePending) return;
  String payload = pendingMqttPayload;
  mqttMessagePending = false;
  pendingMqttTopic = "";
  pendingMqttPayload = "";
  processMqttCommand(payload);
}


void printStaDiag(const __FlashStringHelper *reason) {
  (void)reason;
}

bool tcpProbeBroker() {
  WiFiClient probe;
  bool ok = probe.connect(MQTT_HOST, MQTT_PORT);
  probe.stop();
  return ok;
}

bool mqttConnect() {
  if (!CLOUD_MQTT_ENABLED) return false;
  if (!mqttHostConfiguredForCloud()) return false;
  if (WiFi.status() != WL_CONNECTED) return false;

  String clientId = String("SmartCharge-") + DEVICE_ID + "-" + String((uint32_t)esp_random(), HEX);
  String willTopic = topicPrefix() + "/availability";

  bool ok;
  if (strlen(MQTT_USER) > 0) {
    ok = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD, willTopic.c_str(), 0, true, "offline");
  } else {
    ok = mqttClient.connect(clientId.c_str(), willTopic.c_str(), 0, true, "offline");
  }

  if (!ok) {
    int st = mqttClient.state();
    unsigned long now = millis();
    if (st != lastMqttFailState || now - lastMqttFailLogMs > 60000UL) {
      lastMqttFailState = st;
      lastMqttFailLogMs = now;
      Serial.print(F("[MQTT] Offline, state="));
      Serial.println(st);
    }
    mqttWasOnline = false;
    return false;
  }

  lastMqttFailState = 999;
  mqttWasOnline = true;
  mqttClient.publish(willTopic.c_str(), "online", true);
  String cmdTopic = topicPrefix() + "/cmd";
  mqttClient.subscribe(cmdTopic.c_str());
  Serial.println(F("[MQTT] Online"));
  publishStatus(true);
  return true;
}

void mqttLoop() {
  if (!CLOUD_MQTT_ENABLED) return;
  if (!mqttHostConfiguredForCloud()) return;
  if (WiFi.status() != WL_CONNECTED) return;

  if (!mqttClient.connected()) {
    if (mqttWasOnline) {
      mqttWasOnline = false;
      Serial.println(F("[MQTT] Offline"));
    }
    unsigned long now = millis();
    if (now - lastMqttReconnectAttempt > 5000) {
      lastMqttReconnectAttempt = now;
      mqttConnect();
    }
    return;
  }

  mqttClient.loop();
  if (millis() - lastStatusPublishMs > 10000UL) {
    lastStatusPublishMs = millis();
    publishStatus(false);
  }
}

// ============================================================
// TICKET PRINT LOGIC
// ============================================================

void createAndPrintTicket() {
  unsigned long now = millis();
  if (now - lastPrintMs < PRINT_COOLDOWN_MS) {
    Serial.println(F("[TICKET] Cooldown, tunggu sebentar."));
    return;
  }
  lastPrintMs = now;

  if (!cloudReadyToPrint()) {
    Serial.println(F("[TICKET] Cloud belum siap. QR tidak dicetak agar tidak fallback ke AP/IP lokal."));
    if (!publicWebUrlConfigured()) Serial.println(F("[TICKET] Cek PUBLIC_WEB_BASE_URL, harus domain publik, bukan IP."));
    if (!mqttHostConfiguredForCloud()) Serial.println(F("[TICKET] Cek MQTT_HOST, jangan pakai IP laptop/private network."));
    if (WiFi.status() != WL_CONNECTED) Serial.println(F("[TICKET] WiFi internet belum online."));
    if (!mqttClient.connected()) Serial.println(F("[TICKET] MQTT broker belum online."));
    return;
  }

  String token = makeToken();
  String url = buildCloudUrl(token);

  Serial.print(F("[TICKET] "));
  Serial.print(token);
  Serial.println(F(" | cloud"));

  bool printed = printTicket(token, url, true);
  if (!printed) {
    publishEvent(String("PRINT_FAILED|") + token);
    return;
  }

  TicketState *ticket = addTicket(token, url, true);
  lastTicketToken = token;
  lastTicketUrl = url;
  publishTicketMqtt(*ticket);
}

bool isButtonPressedState(uint8_t state) {
  return state == BUTTON_PRESSED_LEVEL;
}

void handlePrintButton() {
  const uint8_t reading = digitalRead(PRINT_BUTTON_PIN);
  const unsigned long now = millis();

  // Catatan penting:
  // Serial tidak lagi mencetak raw LOW/HIGH agar monitor tidak spam.
  // Perubahan raw hanya dipakai untuk debounce internal.
  if (reading != lastButtonReading) {
    lastButtonReading = reading;
    lastButtonChangeMs = now;
  }

  if ((now - lastButtonChangeMs) < BUTTON_DEBOUNCE_MS) return;
  if (reading == stableButtonState) return;

  stableButtonState = reading;
  const bool pressed = isButtonPressedState(stableButtonState);

  if (pressed && !buttonPressedLatched) {
    buttonPressedLatched = true;
    Serial.println(F("[BUTTON] Tombol ditekan"));
    createAndPrintTicket();
  }

  if (!pressed) {
    buttonPressedLatched = false;
  }
}

// ============================================================
// WEB LOKAL ESP32
// ============================================================

String pageStart(const String &title) {
  String h;
  h.reserve(3200);
  h += F("<!doctype html><html lang='id'><head><meta charset='utf-8'>");
  h += F("<meta name='viewport' content='width=device-width,initial-scale=1,maximum-scale=1'>");
  h += F("<meta http-equiv='Cache-Control' content='no-store'>");
  h += F("<title>"); h += htmlEscape(title); h += F("</title>");
  h += F("<style>");
  h += F(":root{font-family:system-ui,-apple-system,Segoe UI,sans-serif;color:#172033;background:#eef3ff}");
  h += F("*{box-sizing:border-box}body{margin:0}.wrap{max-width:920px;margin:auto;padding:18px}");
  h += F(".hero{background:linear-gradient(135deg,#101828,#2447ff);color:white;border-radius:24px;padding:22px;box-shadow:0 16px 40px #10182830}");
  h += F(".hero h1{margin:0 0 8px;font-size:28px}.hero p{margin:5px 0;color:#e8edff}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(145px,1fr));gap:12px;margin-top:16px}");
  h += F(".card{background:white;border-radius:20px;padding:16px;box-shadow:0 10px 26px #24365a17;border:1px solid #dfe7ff}.port{min-height:150px}");
  h += F(".badge{display:inline-flex;padding:5px 10px;border-radius:999px;font-size:12px;font-weight:800}.on{background:#dcfce7;color:#166534}.off{background:#f1f5f9;color:#475569}.bad{background:#fee2e2;color:#991b1b}");
  h += F(".btn{display:inline-flex;align-items:center;justify-content:center;text-decoration:none;border:0;border-radius:14px;padding:12px 14px;font-weight:800;background:#2447ff;color:white;margin-top:10px;min-height:44px}");
  h += F(".btn.secondary{background:#eef2ff;color:#2447ff}.btn.danger{background:#ef4444;color:white}.btn.full{width:100%}.muted{color:#64748b}.big{font-size:24px;font-weight:900}.small{font-size:12px}.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}.price{font-size:28px;font-weight:950;margin:6px 0}.box{background:#f8fafc;border:1px dashed #cbd5e1;border-radius:18px;padding:14px}.center{text-align:center}.footer{margin-top:20px;text-align:center;color:#64748b;font-size:12px}");
  h += F("</style></head><body><div class='wrap'>");
  return h;
}

String pageEnd() {
  String h;
  h += F("<div class='footer'>SmartCharge Ticket Printer · Cloud-only · AP off</div></div></body></html>");
  return h;
}

void sendHtml(const String &html) {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
  server.sendHeader("Pragma", "no-cache");
  server.send(200, "text/html; charset=utf-8", html);
}

void redirectHome() {
  server.sendHeader("Location", String("/"), true);
  server.send(302, "text/plain", "");
}

void redirectTo(const String &path) {
  server.sendHeader("Location", path, true);
  server.send(302, "text/plain", "");
}

int getPortArg() {
  if (!server.hasArg("port")) return -1;
  int p = server.arg("port").toInt();
  if (p < 1 || p > 8) return -1;
  return p - 1;
}

int getPkgArg() {
  if (!server.hasArg("pkg")) return -1;
  int p = server.arg("pkg").toInt();
  if (p < 0 || p >= PACKAGE_COUNT) return -1;
  return p;
}

String getTicketArg() {
  if (!server.hasArg("ticket")) return "";
  return server.arg("ticket");
}

void handleRoot() {
  String h = pageStart("SmartCharge Admin Lokal");
  h += F("<section class='hero'><h1>SmartCharge</h1><p>Mode cloud-only. AP/captive portal mati. Tiket QR hanya dicetak saat WiFi internet dan MQTT cloud siap.</p>");
  h += F("<p class='small'>WiFi: ");
  h += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("offline"));
  h += F(" · MQTT: ");
  h += mqttClient.connected() ? F("online") : F("offline");
  h += F(" · Printer: ");
  h += printerConnected ? F("online") : F("unknown/offline");
  h += F("</p></section>");

  h += F("<div class='card'><h2>Cetak ticket</h2><p>Tekan tombol fisik S0/GPIO13, atau tombol ini untuk tes. Kalau cloud belum siap, QR tidak akan dicetak.</p>");
  h += F("<a class='btn full' href='/print'>Cetak QR sekarang</a>");
  if (lastTicketUrl.length()) {
    h += F("<p class='small muted'>Last ticket: "); h += htmlEscape(lastTicketToken); h += F("</p>");
    h += F("<p><a class='btn secondary full' href='"); h += htmlEscape(lastTicketUrl); h += F("'>Buka last ticket</a></p>");
  }
  h += F("</div>");

  h += F("<h2>Status port</h2><div class='grid'>");
  for (uint8_t i = 0; i < 8; i++) {
    h += F("<div class='card port'><div class='row'><div class='big'>Port "); h += String(i + 1); h += F("</div>");
    h += ports[i].active ? F("<span class='badge on'>AKTIF</span>") : F("<span class='badge off'>OFF</span>");
    h += F("</div>");
    if (ports[i].active) {
      h += F("<p>Sisa: <b>"); h += remainingTime(i); h += F("</b></p>");
      h += F("<p class='small muted'>TX: "); h += htmlEscape(ports[i].transactionId); h += F("</p>");
      h += F("<a class='btn danger full' href='/stop?port="); h += String(i + 1); h += F("'>Matikan</a>");
    } else {
      h += F("<p class='muted'>Siap</p>");
    }
    h += F("</div>");
  }
  h += F("</div><p class='row'><a class='btn danger' href='/stopall'>Matikan semua</a><a class='btn secondary' href='/status'>JSON status</a></p>");
  h += pageEnd();
  sendHtml(h);
}

void handleCloudOnlyPage() {
  String h = pageStart("Cloud only");
  h += F("<section class='hero'><h1>Mode cloud-only</h1><p>Halaman pembayaran lokal dimatikan. QR harus membuka web cloud dari PUBLIC_WEB_BASE_URL.</p>");
  h += F("<p class='small'>Cloud: "); h += htmlEscape(String(PUBLIC_WEB_BASE_URL)); h += F("</p></section>");
  h += F("<div class='card'><p>Jika halaman ini muncul, berarti kamu membuka endpoint lokal ESP32. Untuk user, gunakan QR yang tercetak dari printer dan pastikan backend cloud mengirim MQTT ke topic command alat.</p>");
  h += F("<a class='btn full' href='/'>Kembali ke admin</a></div>");
  h += pageEnd();
  sendHtml(h);
}

void handlePay() {
  handleCloudOnlyPage();
}

void handlePackage() {
  handleCloudOnlyPage();
}

void handleConfirm() {
  handleCloudOnlyPage();
}

void handlePaid() {
  handleCloudOnlyPage();
}

void handlePrintNow() {
  createAndPrintTicket();
  redirectTo("/");
}

void handleButtonDebug() {
  String h = pageStart("Button debug");
  h += F("<section class='hero'><h1>Button debug</h1><p>Default v8 mengikuti project parkir: S0 ke GPIO13, INPUT_PULLUP, LOW = ditekan.</p></section>");
  h += F("<div class='card'><p>GPIO tombol: <b>"); h += String(PRINT_BUTTON_PIN); h += F("</b></p>");
  h += F("<p>Raw sekarang: <b>"); h += (digitalRead(PRINT_BUTTON_PIN) == HIGH ? F("HIGH / lepas") : F("LOW / ditekan")); h += F("</b></p>");
  h += F("<p>Mode: <b>");
  h += (BUTTON_USE_INTERNAL_PULLUP ? F("INPUT_PULLUP") : F("INPUT"));
  h += F(", aktif: "); h += (BUTTON_PRESSED_LEVEL == LOW ? F("LOW") : F("HIGH"));
  h += F("</b></p>");
  h += F("<p>Kalau Raw tidak berubah saat tombol ditekan, wiring tombol belum masuk ke GPIO yang dibaca firmware.</p>");
  h += F("<a class='btn full' href='/button'>Refresh</a><a class='btn secondary full' href='/'>Kembali</a></div>");
  h += pageEnd();
  sendHtml(h);
}

void handleStop() {
  int idx = getPortArg();
  if (idx >= 0) deactivatePort((uint8_t)idx);
  redirectTo("/");
}

void handleStopAll() {
  for (uint8_t i = 0; i < 8; i++) deactivatePort(i);
  redirectTo("/");
}

void handlePing() {
  server.send(200, "text/plain; charset=utf-8", "pong");
}

void handleStatusJson() {
  String j = F("{\"device\":\"");
  j += DEVICE_ID;
  j += F("\",\"ap\":\"disabled\"");
  j += F(",\"ap_ip\":\"disabled\"");
  j += F(",\"sta_ip\":\"");
  j += (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("offline"));
  j += F("\",\"mqtt\":");
  j += mqttClient.connected() ? "true" : "false";
  j += F(",\"clients\":0");
  j += F(",\"last_ticket\":\"");
  j += lastTicketToken;
  j += F("\",\"ports\":[");
  for (uint8_t i = 0; i < 8; i++) {
    if (i) j += F(",");
    j += F("{\"port\":"); j += String(i + 1);
    j += F(",\"active\":"); j += (ports[i].active ? "true" : "false");
    j += F(",\"remaining\":\""); j += remainingTime(i); j += F("\"}");
  }
  j += F("]}");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", j);
}

void handleCaptiveProbe() {
  redirectHome();
}

void handleNotFound() {
  redirectHome();
}

void setupRoutes() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/ping", HTTP_GET, handlePing);
  server.on("/status", HTTP_GET, handleStatusJson);
  server.on("/api/status", HTTP_GET, handleStatusJson);
  server.on("/pay", HTTP_GET, handlePay);
  server.on("/package", HTTP_GET, handlePackage);
  server.on("/confirm", HTTP_GET, handleConfirm);
  server.on("/paid", HTTP_GET, handlePaid);
  server.on("/print", HTTP_GET, handlePrintNow);
  server.on("/print-test", HTTP_GET, handlePrintNow);
  server.on("/button", HTTP_GET, handleButtonDebug);
  server.on("/stop", HTTP_GET, handleStop);
  server.on("/stopall", HTTP_GET, handleStopAll);

  // Endpoint probe tetap diarahkan ke root, tetapi AP/captive portal tidak dinyalakan.
  server.on("/generate_204", HTTP_GET, handleCaptiveProbe);
  server.on("/gen_204", HTTP_GET, handleCaptiveProbe);
  server.on("/mobile/status.php", HTTP_GET, handleCaptiveProbe);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptiveProbe);
  server.on("/library/test/success.html", HTTP_GET, handleCaptiveProbe);
  server.on("/connecttest.txt", HTTP_GET, handleCaptiveProbe);
  server.on("/ncsi.txt", HTTP_GET, handleCaptiveProbe);
  server.on("/fwlink", HTTP_GET, handleCaptiveProbe);
  server.on("/redirect", HTTP_GET, handleCaptiveProbe);
  server.onNotFound(handleNotFound);
}

// ============================================================
// NETWORK
// ============================================================

void setupNetwork() {
  WiFi.mode(WIFI_OFF);
  delay(300);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);

  Serial.println(F("[AP] Nonaktif, mode cloud-only"));

  if (CLOUD_MQTT_ENABLED && strlen(STA_SSID) > 0 && strcmp(STA_SSID, "ISI_WIFI_INTERNET") != 0) {
    Serial.print(F("[WIFI] Join "));
    Serial.println(STA_SSID);
    WiFi.begin(STA_SSID, STA_PASSWORD);
  } else {
    Serial.println(F("[WIFI] SSID internet belum diset / cloud nonaktif"));
  }
}

void handleWiFiReconnect() {
  if (!CLOUD_MQTT_ENABLED) return;
  if (strlen(STA_SSID) == 0 || strcmp(STA_SSID, "ISI_WIFI_INTERNET") == 0) return;

  const bool online = (WiFi.status() == WL_CONNECTED);
  if (online && !wifiWasOnline) {
    wifiWasOnline = true;
    Serial.print(F("[WIFI] Online: "));
    Serial.println(WiFi.localIP());
  }
  if (!online && wifiWasOnline) {
    wifiWasOnline = false;
    mqttWasOnline = false;
    Serial.println(F("[WIFI] Offline, reconnecting"));
  }
  if (online) return;

  unsigned long now = millis();
  if (now - lastWifiReconnectAttempt > 15000UL) {
    lastWifiReconnectAttempt = now;
    WiFi.disconnect(false, false);
    WiFi.begin(STA_SSID, STA_PASSWORD);
  }
}

void setupMqtt() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);
}

void setupButton() {
  if (BUTTON_USE_INTERNAL_PULLUP) pinMode(PRINT_BUTTON_PIN, INPUT_PULLUP);
  else pinMode(PRINT_BUTTON_PIN, INPUT);

  delay(80);
  const uint8_t initialState = digitalRead(PRINT_BUTTON_PIN);
  lastButtonReading = initialState;
  stableButtonState = initialState;
  lastButtonChangeMs = millis();
  buttonPressedLatched = isButtonPressedState(initialState);

  Serial.print(F("[BUTTON] GPIO"));
  Serial.print(PRINT_BUTTON_PIN);
  Serial.println(F(" ready"));
}

// ============================================================
// ARDUINO SETUP/LOOP
// ============================================================

void setup() {
  Serial.begin(115200);
  delay(800);
  Serial.println(F("\n=== SmartCharge Pro v8 Cloud Only ==="));
  Serial.print(F("Device : ")); Serial.println(DEVICE_ID);
  Serial.print(F("Cloud  : ")); Serial.println(PUBLIC_WEB_BASE_URL);
  Serial.print(F("MQTT   : ")); Serial.print(MQTT_HOST); Serial.print(F(":")); Serial.println(MQTT_PORT);

  if (!publicWebUrlConfigured()) {
    Serial.println(F("[INFO] Cloud URL belum valid. QR tidak akan dicetak sampai PUBLIC_WEB_BASE_URL memakai domain publik."));
  }
  if (!mqttHostConfiguredForCloud()) {
    Serial.println(F("[INFO] MQTT_HOST belum valid/cloud. Jangan pakai IP laptop/private network."));
  }

  clearTickets();
  setupRelays();
  setupButton();
  setupNetwork();
  setupMqtt();
  setupRoutes();
  server.begin();

  Serial.println(F("[WEB] Admin lokal hanya via IP WiFi router setelah STA online, bukan AP ESP32."));
  Serial.println(F("[READY] Tekan tombol S0/GPIO13 untuk cetak QR cloud"));
}

void loop() {
  server.handleClient();
  handleWiFiReconnect();
  mqttLoop();
  processPendingMqttMessage();
  handlePrintButton();
  checkTimers();
  cleanupExpiredTickets();
  delay(2);
}
