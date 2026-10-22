/*
 * Pocket AI Assistant — versi simulasi (Wokwi)
 * ------------------------------------------------
 * Hardware: Seeed Studio XIAO ESP32-S3 + OLED SSD1306 + 3 tombol + LED.
 * Terinspirasi dari: https://www.huyvector.org/robots-kinetic/pocket-ai-assistant
 * (desain asli pakai ESP32-C3 + mic I2S + ampli MAX98357 + speaker, firmware
 * ESPHome -- proyek ini dipindah ke XIAO ESP32-S3 karena bentuknya lebih
 * kecil/praktis utk device saku, dan punya lebih banyak pin bebas utk 3
 * tombol di bawah).
 *
 * Tombol:
 *   - ASK (D0)  : klik tunggal = tanya ke AI (atau "kembali" selama di
 *                 dalam menu Pengaturan). Klik GANDA (2x tekan dalam
 *                 <=1 detik) dari layar standby = buka menu Pengaturan;
 *                 di dalam menu, pada item yang bisa diubah (Kecerahan/
 *                 Volume/Timeout layar) = masuk mode edit nilai.
 *   - UP   (D1) : tampilkan jam. Di dalam menu Pengaturan: pindah ke item
 *                 berikutnya (LIST) atau naikkan nilai (mode edit).
 *   - DOWN (D3) : tampilkan jam juga (nanti: volume down kalau sudah ada
 *                 speaker asli). Di dalam menu Pengaturan: pindah ke item
 *                 sebelumnya (LIST) atau turunkan nilai (mode edit).
 *
 * Deteksi tombol pakai HARDWARE INTERRUPT (attachInterrupt), bukan cuma
 * polling digitalRead() biasa -- supaya penekanan tombol tetap tertangkap
 * walau loop() sedang "sibuk" di tengah panggilan network yang blocking
 * (mis. polling Firebase tiap beberapa detik saat standby). Lihat
 * checkEdge()/updateAskButton() untuk detail.
 *
 * KETERBATASAN SIMULASI:
 * Wokwi belum punya simulasi audio I2S yang nyata (tidak bisa merekam suara
 * asli dari mic maupun memutar suara dari speaker). Karena itu, versi
 * sementara ini memakai Serial Monitor sebagai pengganti suara:
 *
 *   1. Tekan tombol ASK (push-to-talk)  -> status "LISTENING"
 *   2. Ketik pertanyaan di Serial Monitor, lalu Enter
 *   3. ESP32 memanggil NVIDIA NIM API (model Nemotron) lewat WiFi (internet
 *      asli, lewat gateway simulasi Wokwi)   -> status "THINKING"
 *   4. Jawaban ditampilkan di OLED (ringkas) dan di Serial Monitor (lengkap)
 *
 * Nanti kalau dipindah ke hardware fisik, cukup ganti bagian "ambil teks
 * pertanyaan" dari Serial.readStringUntil() menjadi hasil STT dari mic asli
 * (mis. lewat ESPHome voice_assistant + Home Assistant Assist, atau layanan
 * STT cloud), dan tambahkan I2S speaker (MAX98357) untuk TTS -- tombol
 * UP/DOWN tinggal disambung ke kontrol volume ampli-nya (di luar menu
 * Pengaturan). Semua bagian lain (state machine, pemanggilan API, tampilan
 * OLED) tetap sama.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <time.h>
#include <Preferences.h>

#include "secrets.h"

#define FIRMWARE_VERSION "1.1.0"

// ---------- Pin mapping (lihat diagram.json) -- XIAO ESP32-S3 ----------
// Label D0-D10 di papan XIAO sesuai pinout resmi Seeed:
// D0=GPIO1 D1=GPIO2 D2=GPIO3 D3=GPIO4 D4=GPIO5(SDA) D5=GPIO6(SCL)
// D6=GPIO43(TX) D7=GPIO44(RX) D8=GPIO7 D9=GPIO8 D10=GPIO9
static const int PIN_I2C_SDA   = 5;   // D4
static const int PIN_I2C_SCL   = 6;   // D5
static const int PIN_BTN_ASK   = 1;   // D0 -- aktif LOW
static const int PIN_BTN_UP    = 2;   // D1 -- aktif LOW
static const int PIN_BTN_DOWN  = 4;   // D3 -- aktif LOW
static const int PIN_LED       = 7;   // D8 -- indikator status

// ---------- OLED ----------
static const uint8_t SCREEN_WIDTH  = 128;
static const uint8_t SCREEN_HEIGHT = 64;
static const int8_t  OLED_RESET    = -1; // tidak dipakai di modul I2C ini
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Tata letak teks: font size 1 ~= 6x8 px -> 21 kolom.
// Body dimulai di y=12 (setelah judul + garis pemisah) dan layar tinggi 64px,
// jadi ruang tersisa 52px -> maksimal 6 baris (6*8=48, pas tanpa meluber).
static const uint8_t CHARS_PER_LINE  = 21;
static const uint8_t MAX_BODY_LINES  = 6;
static const int     BODY_START_Y    = 12;

// ---------- State machine ----------
enum AssistantState {
  STATE_IDLE, STATE_LISTENING, STATE_THINKING, STATE_ANSWERED,
  STATE_CLOCK, STATE_SETTINGS, STATE_SETTINGS_EDIT
};
AssistantState state = STATE_IDLE;

String inputBuffer;      // buffer teks pertanyaan dari Serial
unsigned long ledTimer = 0;
bool ledOn = false;

// ---------------------------------------------------------
// Deteksi tombol via HARDWARE INTERRUPT + debounce.
//
// Kenapa interrupt, bukan cuma polling digitalRead() di loop()? Karena
// loop() ini kadang BLOCKING cukup lama (mis. haGetQuestion() polling
// Firebase tiap beberapa detik saat standby, bisa makan waktu 1-3+ detik
// untuk request HTTPS-nya). Tombol yang ditekan-lepas SELURUHNYA dalam
// jendela waktu itu akan terlewat sama sekali kalau cuma mengandalkan
// polling biasa -- device terasa "tidak merespons" padahal sebenarnya
// cuma lagi sibuk network call. Interrupt menangkap tepi tekanan (falling
// edge) seketika kapan pun terjadi, lepas dari apa yang sedang dikerjakan
// loop() -- begitu loop() longgar lagi, flag-nya masih ada untuk diproses.
// ---------------------------------------------------------
static const unsigned long DEBOUNCE_MS = 60;

volatile bool upIsrFlag = false;
volatile bool downIsrFlag = false;
volatile bool askIsrFlag = false;

void IRAM_ATTR onUpFalling() { upIsrFlag = true; }
void IRAM_ATTR onDownFalling() { downIsrFlag = true; }
void IRAM_ATTR onAskFalling() { askIsrFlag = true; }

struct SimpleButton {
  int pin;
  volatile bool *isrFlag;
  bool down;
  unsigned long lastChangeTime;
};

SimpleButton btnUp   = { PIN_BTN_UP,   &upIsrFlag,   false, 0 };
SimpleButton btnDown = { PIN_BTN_DOWN, &downIsrFlag, false, 0 };

// True PERSIS SEKALI tiap kali tombol baru ditekan (butuh rilis dulu
// sebelum bisa memicu lagi). isrFlag dipakai sebagai jaring pengaman kalau
// seluruh tekan-lepas kejadian di tengah loop() sedang blocking -- begitu
// longgar lagi, raw sudah HIGH tapi isrFlag membuktikan sempat ada tekanan.
bool checkEdge(SimpleButton &b) {
  bool raw = (digitalRead(b.pin) == LOW);
  bool isrCaught = *b.isrFlag;
  if (isrCaught) *b.isrFlag = false;

  if (!b.down && (raw || isrCaught)) {
    if (millis() - b.lastChangeTime < DEBOUNCE_MS) return false;
    b.down = true;
    b.lastChangeTime = millis();
    return true;
  }
  if (b.down && !raw) {
    b.down = false;
    b.lastChangeTime = millis();
  }
  return false;
}

// Tombol ASK dibedakan klik-tunggal vs klik-ganda (bukan lagi tekan-tahan --
// tekan-tahan ternyata terasa kurang nyaman dipakai). Klik-ganda = dua
// tekan-lepas dalam jarak <=DOUBLE_CLICK_MS, dipakai sbg aksi "enter" (buka
// menu Pengaturan / masuk mode edit item). Klik-tunggal tetap jadi aksi
// default (tanya AI, atau "kembali" selama di dalam menu Pengaturan) --
// tapi karena klik-tunggal baru bisa dipastikan BUKAN awal dari klik-ganda
// setelah jendela DOUBLE_CLICK_MS lewat tanpa klik kedua, aksinya baru
// benar-benar terpicu setelah jeda itu (maks ~1 detik keterlambatan).
//
// PENTING -- status tekan/rilis dilacak lewat POLLING + debounce biasa
// (persis pola checkEdge() di atas), BUKAN lewat timestamp yang dibaca di
// dalam ISR. Percobaan sebelumnya (pakai interrupt CHANGE + baca/klasifikasi
// tepi di dalam ISR) ternyata rentan meleset kalau dua transisi terjadi
// berdekatan (umum terjadi persis saat klik-ganda, apalagi gesture
// double-click mouse yang asli di Wokwi GUI -- jauh lebih cepat & lebih
// "berisik" sinyalnya daripada rangkaian set-control yang diberi jeda
// manual). Dengan polling+debounce di sini, digitalRead() selalu dibaca
// dari konteks loop() biasa (tidak racy), dan isrFlag (falling-only, sama
// seperti UP/DOWN) cuma dipakai sbg jaring pengaman kalau SELURUH
// tekan-lepas kejadian persis di tengah panggilan blocking.
static const unsigned long DOUBLE_CLICK_MS = 1000;
bool askButtonDown = false;        // status "ditekan" (debounced) saat ini
unsigned long askLastChangeTime = 0;  // kapan status debounced terakhir berubah
bool askClickPending = false;      // ada 1 klik yg masih nunggu potensi klik kedua
unsigned long askPendingSince = 0; // kapan klik yg masih nunggu itu selesai (rilis)

void updateAskButton(bool &singleClick, bool &doubleClick) {
  singleClick = false;
  doubleClick = false;

  bool raw = (digitalRead(PIN_BTN_ASK) == LOW);
  bool isrCaught = askIsrFlag;
  if (isrCaught) askIsrFlag = false;
  unsigned long now = millis();

  if (!askButtonDown) {
    if ((raw || isrCaught) && now - askLastChangeTime >= DEBOUNCE_MS) {
      askButtonDown = true;
      askLastChangeTime = now;
      return;  // progres: mulai ditekan -- jangan lanjut cek timeout di bawah
                // (kalau tidak, klik SEBELUMnya yang masih "pending" bisa
                // salah difinalisasi jadi klik-tunggal tepat saat klik BARU
                // ini baru mulai, padahal seharusnya diberi kesempatan
                // berpasangan jadi klik-ganda)
    }
  } else {
    if (!raw && (now - askLastChangeTime >= DEBOUNCE_MS)) {
      // Rilis terkonfirmasi (debounced).
      askButtonDown = false;
      askLastChangeTime = now;
      if (askClickPending && now - askPendingSince <= DOUBLE_CLICK_MS) {
        // Klik kedua datang dalam jendela -> klik-ganda.
        askClickPending = false;
        doubleClick = true;
      } else {
        // Klik pertama (atau klik sendirian) -- tunggu dulu, jangan
        // langsung dianggap klik-tunggal, siapa tahu ada klik kedua nyusul.
        askClickPending = true;
        askPendingSince = now;
      }
    }
    return;  // masih ditekan, atau baru rilis -- tidak ada progres lain
  }

  // Tidak ada progres tekan/rilis -- kalau ada klik yg masih "nunggu" dan
  // jendela klik-ganda sudah lewat tanpa klik kedua, baru itu dipastikan
  // klik-tunggal.
  if (askClickPending && now - askPendingSince > DOUBLE_CLICK_MS) {
    askClickPending = false;
    singleClick = true;
  }
}

// Polling pertanyaan dari HP via Home Assistant (lihat haGetQuestion())
unsigned long haPollTimer = 0;
static const unsigned long HA_POLL_INTERVAL_MS = 4000;
String lastHaQuestion = "";

// Auto-kembali ke standby (wajah robot) kalau tidak ada aktivitas setelah
// jawaban ditampilkan.
unsigned long answeredAtTimer = 0;
static const unsigned long ANSWERED_TIMEOUT_MS = 15000;

// ---------------------------------------------------------
// Pengaturan tersimpan (NVS via Preferences) -- bertahan walau direstart.
// ---------------------------------------------------------
Preferences prefs;

struct Settings {
  uint8_t brightness;     // index ke BRIGHTNESS_CONTRAST[]
  uint8_t volume;         // 0..4 -> 0/25/50/75/100% (placeholder, blm ada speaker asli)
  uint8_t screenTimeout;  // index ke SCREEN_TIMEOUT_MS[]
};
Settings settings = { 1, 2, 0 };  // default: kecerahan sedang, volume 50%, layar tidak pernah mati

const uint8_t BRIGHTNESS_CONTRAST[3] = { 10, 127, 255 };
const char *BRIGHTNESS_LABELS[3] = { "Rendah", "Sedang", "Tinggi" };

const unsigned long SCREEN_TIMEOUT_MS[4] = { 0, 30000, 60000, 120000 };
const char *SCREEN_TIMEOUT_LABELS[4] = { "Tidak pernah", "30 detik", "60 detik", "120 detik" };

void applyBrightness() {
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(BRIGHTNESS_CONTRAST[settings.brightness]);
}

void loadSettings() {
  prefs.begin("pocketai", true);  // read-only
  settings.brightness = prefs.getUChar("bright", settings.brightness);
  settings.volume = prefs.getUChar("volume", settings.volume);
  settings.screenTimeout = prefs.getUChar("scrtimeout", settings.screenTimeout);
  prefs.end();

  if (settings.brightness > 2) settings.brightness = 1;
  if (settings.volume > 4) settings.volume = 2;
  if (settings.screenTimeout > 3) settings.screenTimeout = 0;
}

void saveSettings() {
  prefs.begin("pocketai", false);  // read-write
  prefs.putUChar("bright", settings.brightness);
  prefs.putUChar("volume", settings.volume);
  prefs.putUChar("scrtimeout", settings.screenTimeout);
  prefs.end();
}

// ---------------------------------------------------------
// Util tampilan: tulis judul status + body teks dengan word-wrap sederhana.
// Two-pass: kumpulkan dulu baris-baris hasil wrap ke array (maks
// MAX_BODY_LINES), baru digambar. Ini mencegah baris meluber ke luar batas
// layar karena jumlah baris + indikator "..." sudah dijatah pasti masuk
// dalam MAX_BODY_LINES, bukan ditambah di akhir.
// ---------------------------------------------------------
void showScreen(const String &title, const String &body) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println(title);
  display.drawLine(0, 9, SCREEN_WIDTH - 1, 9, SSD1306_WHITE);

  // --- Pass 1: wrap jadi baris-baris, maksimal MAX_BODY_LINES ---
  String lines[MAX_BODY_LINES];
  uint8_t lineCount = 0;
  String remaining = body;
  remaining.trim();

  while (remaining.length() > 0 && lineCount < MAX_BODY_LINES) {
    int spaceIdx = -1;
    if ((int)remaining.length() > CHARS_PER_LINE) {
      spaceIdx = remaining.substring(0, CHARS_PER_LINE + 1).lastIndexOf(' ');
    }
    String chunk;
    if ((int)remaining.length() <= CHARS_PER_LINE) {
      chunk = remaining;
      remaining = "";
    } else if (spaceIdx > 0) {
      chunk = remaining.substring(0, spaceIdx);
      remaining = remaining.substring(spaceIdx + 1);
    } else {
      chunk = remaining.substring(0, CHARS_PER_LINE);
      remaining = remaining.substring(CHARS_PER_LINE);
    }
    lines[lineCount++] = chunk;
  }

  if (remaining.length() > 0) {
    lines[MAX_BODY_LINES - 1] = "..lengkap di Serial";
  }

  // --- Pass 2: gambar ---
  int cursorY = BODY_START_Y;
  for (uint8_t i = 0; i < lineCount; i++) {
    display.setCursor(0, cursorY);
    display.println(lines[i]);
    cursorY += 8;
  }

  display.display();
}

// Ekspresi & arah lirikan wajah robot yang bisa digambar
enum RobotExpression { FACE_NEUTRAL, FACE_SAD, FACE_HAPPY, FACE_THINKING };
enum LookDir { LOOK_CENTER, LOOK_LEFT, LOOK_RIGHT };

void drawChevronEye(int x, int y) {
  display.drawLine(x, y + 5, x + 5, y, SSD1306_WHITE);
  display.drawLine(x + 5, y, x + 10, y + 5, SSD1306_WHITE);
}

// ---------------------------------------------------------
// Wajah robot (digambar pakai primitif Adafruit_GFX, tanpa perlu file
// bitmap terpisah) -- dipakai di layar standby/idle, WiFi gagal, dan
// sekilas transisi "mikir"/"senang". Mendukung beberapa ekspresi, arah
// lirikan mata (kiri/kanan/tengah), dan kedipan, supaya tidak terasa
// statis saat idle lama.
// ---------------------------------------------------------
void drawRobotFace(const String &caption, RobotExpression expr = FACE_NEUTRAL,
                    LookDir look = LOOK_CENTER, bool blinking = false) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  const int headW = 56, headH = 44;
  const int headX = (SCREEN_WIDTH - headW) / 2;  // 36
  const int headY = 4;
  const int antX = SCREEN_WIDTH / 2;

  if (expr == FACE_SAD) {
    display.drawLine(antX, headY, antX - 5, headY - 6, SSD1306_WHITE);
    display.fillCircle(antX - 5, headY - 6, 2, SSD1306_WHITE);
  } else {
    display.drawLine(antX, headY - 6, antX, headY, SSD1306_WHITE);
    display.fillCircle(antX, headY - 8, 2, SSD1306_WHITE);
  }

  display.drawRoundRect(headX, headY, headW, headH, 8, SSD1306_WHITE);

  int eyeShift = (look == LOOK_LEFT) ? -3 : (look == LOOK_RIGHT) ? 3 : 0;
  int eyeY = headY + 14;
  int lEyeX = headX + 11 + eyeShift;
  int rEyeX = headX + headW - 21 + eyeShift;
  const int EW = 10, EH = 14;

  if (expr == FACE_SAD) {
    display.drawLine(lEyeX, eyeY, lEyeX + EW, eyeY + EH, SSD1306_WHITE);
    display.drawLine(lEyeX + EW, eyeY, lEyeX, eyeY + EH, SSD1306_WHITE);
    display.drawLine(rEyeX, eyeY, rEyeX + EW, eyeY + EH, SSD1306_WHITE);
    display.drawLine(rEyeX + EW, eyeY, rEyeX, eyeY + EH, SSD1306_WHITE);
  } else if (blinking) {
    display.drawLine(lEyeX, eyeY + 7, lEyeX + EW, eyeY + 7, SSD1306_WHITE);
    display.drawLine(rEyeX, eyeY + 7, rEyeX + EW, eyeY + 7, SSD1306_WHITE);
  } else if (expr == FACE_HAPPY) {
    drawChevronEye(lEyeX, eyeY + 7);
    drawChevronEye(rEyeX, eyeY + 7);
  } else {
    display.fillRoundRect(lEyeX, eyeY, EW, EH, 3, SSD1306_WHITE);
    display.fillRoundRect(rEyeX, eyeY, EW, EH, 3, SSD1306_WHITE);
  }

  int mouthY = headY + headH - 10;
  int mcx = headX + headW / 2;
  if (expr == FACE_SAD) {
    display.drawLine(headX + 16, mouthY + 4, mcx, mouthY, SSD1306_WHITE);
    display.drawLine(mcx, mouthY, headX + headW - 16, mouthY + 4, SSD1306_WHITE);
  } else if (expr == FACE_HAPPY) {
    display.drawLine(headX + 16, mouthY, mcx, mouthY + 5, SSD1306_WHITE);
    display.drawLine(mcx, mouthY + 5, headX + headW - 16, mouthY, SSD1306_WHITE);
  } else if (expr == FACE_THINKING) {
    display.drawLine(headX + 18, mouthY, headX + headW - 14, mouthY - 3, SSD1306_WHITE);
  } else {
    display.drawLine(headX + 16, mouthY, headX + headW - 16, mouthY, SSD1306_WHITE);
  }

  display.setTextSize(1);
  display.setCursor(0, SCREEN_HEIGHT - 8);
  display.println(caption);

  display.display();
}

// ---------------------------------------------------------
// Animasi idle: kedip & lirik kiri-kanan acak selama di layar standby,
// supaya wajahnya "hidup" alih-alih diam saja.
// ---------------------------------------------------------
unsigned long nextBlinkAt = 0;
unsigned long nextLookAt = 0;
unsigned long blinkEndAt = 0;
unsigned long lookEndAt = 0;
bool idleBlinking = false;
LookDir idleLook = LOOK_CENTER;

// Kapan terakhir ada aktivitas tombol apa pun, dipakai utk fitur "timeout
// layar" (mematikan panel OLED hemat daya setelah idle lama).
unsigned long lastActivityAt = 0;
bool screenOff = false;

void wakeScreen() {
  if (screenOff) {
    display.ssd1306_command(SSD1306_DISPLAYON);
    screenOff = false;
  }
  lastActivityAt = millis();
}

void scheduleIdleAnim() {
  unsigned long now = millis();
  nextBlinkAt = now + random(1500, 4000);
  nextLookAt = now + random(2500, 6000);
  idleBlinking = false;
  idleLook = LOOK_CENTER;
}

void updateIdleFace() {
  // --- Timeout layar: matikan panel OLED hemat daya setelah idle lama ---
  unsigned long timeoutMs = SCREEN_TIMEOUT_MS[settings.screenTimeout];
  if (timeoutMs > 0 && !screenOff && millis() - lastActivityAt > timeoutMs) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    screenOff = true;
    return;  // tidak perlu lanjut animasi kalau panel sudah mati
  }
  if (screenOff) return;  // panel mati, tunggu tombol (wakeScreen() dipanggil dari loop())

  unsigned long now = millis();
  bool changed = false;

  if (idleBlinking) {
    if (now >= blinkEndAt) {
      idleBlinking = false;
      nextBlinkAt = now + random(2000, 6000);
      changed = true;
    }
  } else if (now >= nextBlinkAt) {
    idleBlinking = true;
    blinkEndAt = now + 150;
    changed = true;
  }

  if (idleLook != LOOK_CENTER) {
    if (now >= lookEndAt) {
      idleLook = LOOK_CENTER;
      nextLookAt = now + random(3000, 7000);
      changed = true;
    }
  } else if (now >= nextLookAt) {
    idleLook = (random(0, 2) == 0) ? LOOK_LEFT : LOOK_RIGHT;
    lookEndAt = now + random(600, 1200);
    changed = true;
  }

  if (changed) {
    drawRobotFace("Siap! Tekan tombol", FACE_NEUTRAL, idleLook, idleBlinking);
  }
}

void setLed(bool on) {
  ledOn = on;
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

// ---------------------------------------------------------
// Tampilan jam (NTP) -- muncul saat tombol UP atau DOWN ditekan (di luar
// menu Pengaturan). Waktu disinkronkan lewat NTP (butuh WiFi, otomatis
// coba sync di setup()). Auto-kembali ke standby setelah CLOCK_TIMEOUT_MS
// tanpa aktivitas, dan jarumnya ikut "jalan" (redraw tiap detik).
// ---------------------------------------------------------
static const unsigned long CLOCK_TIMEOUT_MS = 5000;
static const long GMT_OFFSET_SEC = 7 * 3600;  // WIB (UTC+7)
static const int  DAYLIGHT_OFFSET_SEC = 0;    // Indonesia tidak pakai DST
static const char *NTP_SERVER = "pool.ntp.org";

unsigned long clockShownAt = 0;
unsigned long clockLastTick = 0;

void drawClockScreen() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.println("Jam");
  display.drawLine(0, 9, SCREEN_WIDTH - 1, 9, SSD1306_WHITE);

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 100)) {
    display.setTextSize(1);
    display.setCursor(0, 28);
    display.println("Waktu belum sync.");
    display.setCursor(0, 40);
    display.println("Cek koneksi WiFi.");
  } else {
    char timeBuf[12];
    strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &timeinfo);
    display.setTextSize(2);
    display.setCursor(4, 24);
    display.println(timeBuf);

    char dateBuf[24];
    strftime(dateBuf, sizeof(dateBuf), "%a, %d %b %Y", &timeinfo);
    display.setTextSize(1);
    display.setCursor(0, 48);
    display.println(dateBuf);
  }

  display.display();
}

void enterClock() {
  state = STATE_CLOCK;
  clockShownAt = millis();
  clockLastTick = millis();
  drawClockScreen();
}

// ---------------------------------------------------------
// Menu Pengaturan -- navigasi dua level:
//   - LIST (STATE_SETTINGS): UP/DOWN pindah antar 6 item. Klik-ganda ASK
//     (<=1 detik) pada item yang bisa diubah (Kecerahan/Volume/Timeout
//     layar) masuk ke mode EDIT; pada item read-only (WiFi/Baterai/
//     Firmware) klik-ganda tidak ngapa-ngapain. Klik-tunggal ASK keluar
//     menu, balik ke standby.
//   - EDIT (STATE_SETTINGS_EDIT): UP/DOWN mengubah nilai item yang sedang
//     aktif. Klik-tunggal ASK kembali ke LIST (nilai sudah otomatis
//     kepakai & tersimpan).
// Dibuka dari standby/jawaban dengan klik-ganda ASK. Idle 15 detik di
// LIST maupun EDIT otomatis keluar & menyimpan ke NVS.
// ---------------------------------------------------------
enum SettingItem {
  SET_BRIGHTNESS, SET_VOLUME, SET_WIFI, SET_TIMEOUT, SET_BATTERY, SET_FIRMWARE,
  SET_COUNT
};
int settingIndex = 0;
unsigned long settingsLastActivity = 0;
static const unsigned long SETTINGS_IDLE_TIMEOUT_MS = 15000;

bool settingItemIsAdjustable(int idx) {
  return idx == SET_BRIGHTNESS || idx == SET_VOLUME || idx == SET_TIMEOUT;
}

void drawSettingsScreen(bool editing) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  display.setCursor(0, 0);
  display.println((editing ? "Edit " : "Setting ") + String(settingIndex + 1) +
                   "/" + String((int)SET_COUNT));
  display.drawLine(0, 9, SCREEN_WIDTH - 1, 9, SSD1306_WHITE);

  display.setCursor(0, 12);
  switch (settingIndex) {
    case SET_BRIGHTNESS:
      display.println("Kecerahan OLED");
      display.setTextSize(2);
      display.setCursor(0, 30);
      display.println(BRIGHTNESS_LABELS[settings.brightness]);
      break;

    case SET_VOLUME:
      display.println("Volume (speaker");
      display.println("fisik blm ada)");
      display.setTextSize(2);
      display.setCursor(0, 38);
      display.println(String(settings.volume * 25) + "%");
      break;

    case SET_WIFI:
      display.println("WiFi:");
      display.println(WiFi.SSID());
      display.println("IP:" + WiFi.localIP().toString());
      display.println(String(WiFi.RSSI()) + " dBm");
      break;

    case SET_TIMEOUT:
      display.println("Timeout layar");
      display.setTextSize(2);
      display.setCursor(0, 30);
      display.println(SCREEN_TIMEOUT_LABELS[settings.screenTimeout]);
      break;

    case SET_BATTERY:
      display.println("Baterai:");
      display.println("Board ini tidak");
      display.println("punya sensor");
      display.println("baterai bawaan.");
      break;

    case SET_FIRMWARE:
      display.println("Firmware:");
      display.println("Pocket AI v" FIRMWARE_VERSION);
      display.println(String(__DATE__));
      display.println("Heap:" + String(ESP.getFreeHeap() / 1024) + "KB");
      break;
  }

  display.display();
}

void enterSettings() {
  state = STATE_SETTINGS;
  settingIndex = 0;
  settingsLastActivity = millis();
  drawSettingsScreen(false);
}

void exitSettings() {
  saveSettings();
  state = STATE_IDLE;
  lastActivityAt = millis();
  drawRobotFace("Siap! Tekan tombol");
  scheduleIdleAnim();
}

void enterSettingsEdit() {
  state = STATE_SETTINGS_EDIT;
  settingsLastActivity = millis();
  drawSettingsScreen(true);
}

void exitSettingsEdit() {
  saveSettings();
  state = STATE_SETTINGS;
  settingsLastActivity = millis();
  drawSettingsScreen(false);
}

// ---------------------------------------------------------
// Panggil NVIDIA NIM API (model Nemotron), kembalikan teks jawaban (atau
// pesan error). Format request/respons mengikuti konvensi OpenAI-compatible
// chat completions yang dipakai build.nvidia.com.
// ---------------------------------------------------------
void stripMarkdown(String &text) {
  text.replace("**", "");
  text.replace("$$", "");
  text.replace("$", "");
  text.replace("### ", "");
  text.replace("## ", "");
  text.replace("# ", "");
  text.replace("* ", "- ");
}

// concise=true (tombol+OLED): dibatasi 3 kalimat karena layar cuma muat
// ~6 baris. concise=false (lewat HP via HA/web): tidak ada batas layar,
// jadi boleh jawab lebih lengkap/detail.
String askNvidia(const String &question, bool concise = true) {
  WiFiClientSecure client;
  client.setInsecure(); // NOTE: skip verifikasi sertifikat TLS -> OK untuk simulasi/demo,
                        // JANGAN dipakai apa adanya untuk produk nyata.
  // PENTING: https.setTimeout()/setConnectTimeout() di bawah HANYA membatasi
  // fase TCP connect & baca data -- fase TLS handshake-nya sendiri dibatasi
  // TERPISAH lewat setHandshakeTimeout() di WiFiClientSecure. Tanpa ini,
  // handshake yang macet/lambat bisa membuat panggilan ini blocking jauh
  // lebih lama dari timeout yang di-set (observasi langsung: bisa >10
  // detik walau timeout di-set 3 detik).
  client.setHandshakeTimeout(concise ? 15 : 45);
  client.setTimeout(concise ? 15 : 45);

  HTTPClient https;
  String answer;

  if (!https.begin(client, "https://integrate.api.nvidia.com/v1/chat/completions")) {
    return "Gagal membuka koneksi HTTPS";
  }

  https.setTimeout(concise ? 15000 : 45000);
  https.setConnectTimeout(10000);

  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", String("Bearer ") + NVIDIA_API_KEY);

  JsonDocument reqDoc;
  reqDoc["model"] = NVIDIA_MODEL;
  reqDoc["max_tokens"] = concise ? 300 : 800;
  reqDoc["temperature"] = 0.6;
  reqDoc["stream"] = false;
  reqDoc["chat_template_kwargs"]["thinking"] = false;

  JsonArray messages = reqDoc["messages"].to<JsonArray>();
  JsonObject sysMsg = messages.add<JsonObject>();
  sysMsg["role"] = "system";
  if (concise) {
    sysMsg["content"] =
        "Kamu adalah asisten AI mini di sebuah perangkat saku (pocket AI "
        "assistant) dengan layar OLED kecil. Jawab singkat dan jelas, "
        "maksimal 3 kalimat pendek. Gunakan Bahasa Indonesia kecuali "
        "ditanya dalam bahasa lain.";
  } else {
    sysMsg["content"] =
        "Kamu adalah asisten AI pribadi. Pertanyaan ini datang lewat HP "
        "(bukan layar kecil), jadi jawab dengan lengkap dan jelas -- boleh "
        "beberapa kalimat/paragraf kalau memang perlu, tidak perlu dipaksa "
        "singkat, tapi tetap padat (jangan bertele-tele). PENTING: tulis "
        "sebagai teks polos biasa saja -- JANGAN pakai markdown (tanpa "
        "tanda #, **, *, atau heading), karena ini akan tampil sebagai "
        "notifikasi/pesan teks biasa yang tidak merender markdown. Gunakan "
        "Bahasa Indonesia kecuali ditanya dalam bahasa lain.";
  }
  JsonObject userMsg = messages.add<JsonObject>();
  userMsg["role"] = "user";
  userMsg["content"] = question;

  String body;
  serializeJson(reqDoc, body);

  int httpCode = https.POST(body);

  if (httpCode > 0) {
    String payload = https.getString();
    JsonDocument respDoc;
    DeserializationError err = deserializeJson(respDoc, payload);

    if (err) {
      answer = "Gagal parse JSON respons";
    } else if (respDoc["choices"].is<JsonArray>() &&
               respDoc["choices"].size() > 0) {
      JsonObject msgObj = respDoc["choices"][0]["message"];
      const char *text = msgObj["content"];
      if (text) {
        answer = String(text);
      } else {
        const char *reasoning = msgObj["reasoning_content"];
        answer = reasoning ? ("[reasoning] " + String(reasoning))
                            : "(respons kosong, coba lagi)";
      }
    } else if (!respDoc["error"]["message"].isNull()) {
      const char *errMsg = respDoc["error"]["message"];
      answer = String("API error: ") + errMsg;
    } else if (!respDoc["detail"].isNull()) {
      const char *detail = respDoc["detail"];
      answer = String("API error: ") + detail;
    } else {
      answer = "Respons tidak dikenali: " + payload;
    }
  } else {
    answer = "HTTP request gagal, kode: " + String(httpCode);
  }

  https.end();
  stripMarkdown(answer);
  return answer;
}

// ---------------------------------------------------------
void connectWiFi() {
  showScreen("Pocket AI", "Menyambungkan WiFi...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi tersambung, IP: " + WiFi.localIP().toString());

    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo, 5000)) {
      char buf[32];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
      Serial.println(String("Jam tersinkron (NTP): ") + buf);
    } else {
      Serial.println("Sinkronisasi jam (NTP) gagal/timeout.");
    }

    drawRobotFace("Siap! Tekan tombol");
    scheduleIdleAnim();
    lastActivityAt = millis();
  } else {
    Serial.println("WiFi GAGAL tersambung.");
    drawRobotFace("WiFi gagal :(", FACE_SAD);
  }
}

void enterListening() {
  state = STATE_LISTENING;
  inputBuffer = "";
  Serial.println();
  Serial.println("=== Ketik pertanyaan kamu, lalu tekan Enter ===");
  showScreen("Listening...", "Ketik pertanyaan di\nSerial Monitor,\nlalu Enter.");
}

// ---------------------------------------------------------
// Integrasi opsional dengan Home Assistant (lewat Firebase Realtime
// Database sebagai jembatan), supaya pertanyaan panjang bisa diketik dan
// jawabannya dibaca penuh lewat app HA Companion di HP (tidak dibatasi
// layar OLED 128x64).
//
// ESP32 TIDAK connect langsung ke HA -- HA tidak perlu bisa diakses dari
// internet sama sekali. Sebagai gantinya:
//   - ESP32 polling Firebase RTDB (URL permanen, tidak pernah berubah)
//   - HA punya automation (lihat README) yang menulis pertanyaan dari HP ke
//     Firebase, dan membaca jawaban dari Firebase untuk dikirim sebagai
//     notifikasi ke app HA Companion
// Keduanya (ESP32 & HA) cuma butuh koneksi KELUAR (outbound) ke Firebase,
// jadi tidak perlu tunnel/port-forward ke HA lagi.
//
// Path Firebase-nya sendiri berperan sebagai "kunci" (string acak panjang
// di FIREBASE_SECRET_PATH) -- security rules Firestore cuma mengizinkan
// baca/tulis publik di path spesifik itu, path lain tetap terkunci. Semua
// fungsi ini no-op kalau FIREBASE_DB_URL belum diisi di secrets.h.
// ---------------------------------------------------------
bool haConfigured() {
  return strlen(FIREBASE_DB_URL) > 0;
}

String firebasePath(const String &key) {
  return String(FIREBASE_DB_URL) + "/" + FIREBASE_SECRET_PATH + "/" + key + ".json";
}

// Ambil pertanyaan dari Firebase. String kosong = tidak ada/gagal/null.
// Timeout di-set pendek (3 detik) -- ini dipanggil tiap HA_POLL_INTERVAL_MS
// saat standby, jadi dijaga singkat supaya jendela "loop() blocking" yang
// bisa bikin tombol lain terasa delay (sebelum interrupt menangkapnya)
// tetap sekecil mungkin. PENTING: fase TLS handshake dibatasi TERPISAH
// lewat client.setHandshakeTimeout() (bukan https.setTimeout()/
// setConnectTimeout() yang cuma membatasi fase TCP connect & baca data) --
// tanpa ini, handshake yang macet bisa membuat panggilan ini blocking jauh
// lebih lama dari 3 detik yang dimaksud (terbukti langsung: deteksi klik
// tombol ASK bisa telat/meleset kalau ini kejadian persis di tengah jarak
// antar klik yang sedang diukur).
String haGetQuestion() {
  if (!haConfigured()) return "";

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(3);
  client.setTimeout(3);
  HTTPClient https;
  String result;

  String url = firebasePath("question") + "?cb=" + String(millis());
  if (https.begin(client, url)) {
    https.setTimeout(3000);
    https.setConnectTimeout(3000);
    int code = https.GET();
    if (code == 200) {
      String payload = https.getString();
      payload.trim();
      if (payload != "null" && payload.length() >= 2 &&
          payload.startsWith("\"") && payload.endsWith("\"")) {
        result = payload.substring(1, payload.length() - 1);
      }
    }
    https.end();
  }
  return result;
}

void haClearQuestion() {
  if (!haConfigured()) return;

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(5);
  client.setTimeout(5);
  HTTPClient https;

  if (https.begin(client, firebasePath("question"))) {
    https.addHeader("Content-Type", "application/json");
    https.PUT("null");
    https.end();
  }
}

void haNotify(const String &question, const String &answer) {
  if (!haConfigured()) return;
  (void)question;

  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(5);
  client.setTimeout(5);
  HTTPClient https;

  if (https.begin(client, firebasePath("answer"))) {
    https.addHeader("Content-Type", "application/json");
    JsonDocument doc;
    doc.set(answer);
    String body;
    serializeJson(doc, body);
    https.PUT(body);
    https.end();
  }
}

// concise=true: pertanyaan dari tombol+Serial (OLED kecil, jawaban dipaksa
// ringkas). concise=false: pertanyaan dari HP lewat HA/Firebase (tidak ada
// batas layar, jawaban boleh lebih lengkap).
void enterThinking(const String &question, bool concise = true) {
  state = STATE_THINKING;
  setLed(true);

  drawRobotFace("Mikir...", FACE_THINKING, LOOK_LEFT);
  delay(250);
  drawRobotFace("Mikir...", FACE_THINKING, LOOK_RIGHT);
  delay(250);
  drawRobotFace("Mikir...", FACE_THINKING, LOOK_CENTER, true);
  delay(180);

  showScreen("Thinking...", question);
  Serial.println("Mengirim ke NVIDIA NIM API: \"" + question + "\"");

  String answer = askNvidia(question, concise);

  Serial.println("Jawaban lengkap:");
  Serial.println(answer);

  haNotify(question, answer);

  setLed(false);

  drawRobotFace("Ketemu!", FACE_HAPPY);
  delay(600);

  state = STATE_ANSWERED;
  answeredAtTimer = millis();
  showScreen("Jawaban:", answer);
}

// ---------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nPocket AI Assistant (simulasi Wokwi) - boot");
  Serial.println("Firmware v" FIRMWARE_VERSION ", build " __DATE__ " " __TIME__);

  pinMode(PIN_BTN_ASK, INPUT_PULLUP);
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // Interrupt FALLING utk ketiga tombol -- lihat komentar panjang di bagian
  // atas file soal kenapa ini penting (bukan cuma polling biasa). Semua
  // keputusan status tekan/rilis & timestamp tetap lewat polling+debounce
  // biasa di checkEdge()/updateAskButton(); interrupt di sini cuma jaring
  // pengaman "jangan sampai kelewat kalau loop() lagi blocking".
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_ASK), onAskFalling, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_UP), onUpFalling, FALLING);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_DOWN), onDownFalling, FALLING);

  randomSeed(esp_random());

  loadSettings();

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 OLED tidak terdeteksi!");
  }
  display.setRotation(0);
  applyBrightness();

  connectWiFi();
}

void loop() {
  // --- Baca tombol ASK (klik-tunggal vs klik-ganda) dan UP/DOWN (edge biasa) ---
  bool askSingle, askDouble;
  updateAskButton(askSingle, askDouble);
  bool upPressed = checkEdge(btnUp);
  bool downPressed = checkEdge(btnDown);

  if (askSingle || askDouble || upPressed || downPressed) {
    wakeScreen();  // tombol apa pun membangunkan layar kalau sempat mati (timeout)
  }
  // --- Klik-ganda ASK dari standby/jawaban = buka menu Pengaturan ---
  if (askDouble && (state == STATE_IDLE || state == STATE_ANSWERED)) {
    enterSettings();
  }
  // Klik-ganda ASK di LIST, pada item yang bisa diubah = masuk mode edit.
  else if (askDouble && state == STATE_SETTINGS) {
    if (settingItemIsAdjustable(settingIndex)) {
      enterSettingsEdit();
    }
    // item read-only (WiFi/Baterai/Firmware): klik-ganda tidak ngapa-ngapain
  }
  // Klik-tunggal ASK: mulai tanya (dari IDLE/ANSWERED/CLOCK -- menekan ASK
  // saat lagi lihat jam langsung membatalkan jam & mulai tanya).
  else if (askSingle &&
           (state == STATE_IDLE || state == STATE_ANSWERED || state == STATE_CLOCK)) {
    enterListening();
  }
  // Klik-tunggal ASK di LIST Pengaturan = keluar, balik ke standby.
  else if (askSingle && state == STATE_SETTINGS) {
    exitSettings();
  }
  // Klik-tunggal ASK di mode EDIT = kembali ke LIST (nilai sudah tersimpan).
  else if (askSingle && state == STATE_SETTINGS_EDIT) {
    exitSettingsEdit();
  }

  // --- Tombol UP/DOWN ---
  if (upPressed || downPressed) {
    if (state == STATE_SETTINGS) {
      // Navigasi antar item di LIST.
      settingIndex = upPressed ? (settingIndex + 1) % SET_COUNT
                                : (settingIndex - 1 + SET_COUNT) % SET_COUNT;
      settingsLastActivity = millis();
      drawSettingsScreen(false);
    } else if (state == STATE_SETTINGS_EDIT) {
      // Ubah nilai item yang sedang di-edit.
      switch (settingIndex) {
        case SET_BRIGHTNESS:
          settings.brightness = upPressed ? (settings.brightness + 1) % 3
                                           : (settings.brightness + 2) % 3;
          applyBrightness();
          break;
        case SET_VOLUME:
          settings.volume = upPressed ? (settings.volume + 1) % 5
                                       : (settings.volume + 4) % 5;
          break;
        case SET_TIMEOUT:
          settings.screenTimeout = upPressed ? (settings.screenTimeout + 1) % 4
                                              : (settings.screenTimeout + 3) % 4;
          break;
        default:
          break;  // seharusnya tidak pernah masuk EDIT utk item read-only
      }
      settingsLastActivity = millis();
      drawSettingsScreen(true);
    } else if (state == STATE_IDLE || state == STATE_ANSWERED) {
      enterClock();
    } else if (state == STATE_CLOCK) {
      clockShownAt = millis();  // reset timer auto-kembali
    }
  }

  // --- LED berkedip pelan saat LISTENING, menyala tetap saat THINKING ---
  if (state == STATE_LISTENING) {
    if (millis() - ledTimer > 300) {
      ledTimer = millis();
      setLed(!ledOn);
    }
  }

  // --- Baca input teks dari Serial saat LISTENING ---
  if (state == STATE_LISTENING && Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (inputBuffer.length() > 0) {
        String question = inputBuffer;
        inputBuffer = "";
        enterThinking(question);
      }
    } else {
      inputBuffer += c;
    }
  }

  // --- Polling pertanyaan dari HP (lewat helper input_text di HA) ---
  // Hanya dicek saat device sedang tidak sibuk (IDLE/ANSWERED). Panggilan
  // di dalam haGetQuestion() ini BLOCKING (network) -- inilah skenario yang
  // dulu bikin tombol kadang terasa tidak merespons sebelum dipindah ke
  // interrupt; sekarang aman karena interrupt menangkap tekanan tombol
  // terlepas dari kapan blocking call ini terjadi.
  if (haConfigured() &&
      (state == STATE_IDLE || state == STATE_ANSWERED) &&
      millis() - haPollTimer > HA_POLL_INTERVAL_MS) {
    haPollTimer = millis();

    String q = haGetQuestion();
    q.trim();
    bool isEmpty = (q.length() == 0 || q == "unknown" || q == "unavailable");

    if (!isEmpty && q != lastHaQuestion) {
      lastHaQuestion = q;
      Serial.println();
      Serial.println("=== Pertanyaan baru dari HP (Home Assistant) ===");
      enterThinking(q, false);
      haClearQuestion();
    }
  }

  // --- Auto-kembali ke standby (wajah robot) kalau tidak ada aktivitas ---
  if (state == STATE_ANSWERED && millis() - answeredAtTimer > ANSWERED_TIMEOUT_MS) {
    state = STATE_IDLE;
    lastActivityAt = millis();
    drawRobotFace("Siap! Tekan tombol");
    scheduleIdleAnim();
  }

  // --- Animasi idle (kedip & lirik kiri-kanan) + timeout layar selama standby ---
  if (state == STATE_IDLE) {
    updateIdleFace();
  }

  // --- Layar jam: refresh tiap detik, auto-kembali ke standby setelah
  // CLOCK_TIMEOUT_MS tanpa tombol UP/DOWN ditekan lagi ---
  if (state == STATE_CLOCK) {
    if (millis() - clockLastTick > 1000) {
      clockLastTick = millis();
      drawClockScreen();
    }
    if (millis() - clockShownAt > CLOCK_TIMEOUT_MS) {
      state = STATE_IDLE;
      lastActivityAt = millis();
      drawRobotFace("Siap! Tekan tombol");
      scheduleIdleAnim();
    }
  }

  // --- Menu Pengaturan: auto-keluar & simpan kalau idle terlalu lama ---
  if (millis() - settingsLastActivity > SETTINGS_IDLE_TIMEOUT_MS) {
    if (state == STATE_SETTINGS) {
      exitSettings();
    } else if (state == STATE_SETTINGS_EDIT) {
      exitSettingsEdit();
    }
  }
}
