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
 *   - ASK (D0)  : push-to-talk, tanya ke AI
 *   - UP   (D1) : tampilkan jam (nanti: volume up kalau sudah ada speaker asli)
 *   - DOWN (D3) : tampilkan jam (nanti: volume down kalau sudah ada speaker asli)
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
 * UP/DOWN tinggal disambung ke kontrol volume ampli-nya. Semua bagian lain
 * (state machine, pemanggilan API, tampilan OLED) tetap sama.
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

#include "secrets.h"

// ---------- Pin mapping (lihat diagram.json) -- XIAO ESP32-S3 ----------
// Label D0-D10 di papan XIAO sesuai pinout resmi Seeed:
// D0=GPIO1 D1=GPIO2 D2=GPIO3 D3=GPIO4 D4=GPIO5(SDA) D5=GPIO6(SCL)
// D6=GPIO43(TX) D7=GPIO44(RX) D8=GPIO7 D9=GPIO8 D10=GPIO9
static const int PIN_I2C_SDA   = 5;   // D4
static const int PIN_I2C_SCL   = 6;   // D5
static const int PIN_BTN_ASK   = 1;   // D0 -- push-to-talk, aktif LOW
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
// (Sebelumnya dipakai 7 baris -> total 68px, meluber 4px dan bikin baris
// terakhir terlihat "bertumpuk"/terpotong di bagian bawah layar.)
static const uint8_t CHARS_PER_LINE  = 21;
static const uint8_t MAX_BODY_LINES  = 6;
static const int     BODY_START_Y    = 12;

// ---------- State machine ----------
enum AssistantState { STATE_IDLE, STATE_LISTENING, STATE_THINKING, STATE_ANSWERED, STATE_CLOCK };
AssistantState state = STATE_IDLE;

String inputBuffer;      // buffer teks pertanyaan dari Serial
unsigned long ledTimer = 0;
bool ledOn = false;

// ---------------------------------------------------------
// Debounce tombol -- dipakai bersama utk ketiga tombol (ASK/UP/DOWN) lewat
// satu struct+fungsi, supaya tidak menduplikasi logika debounce 3x.
// ---------------------------------------------------------
static const unsigned long DEBOUNCE_MS = 40;

struct Button {
  int pin;
  bool lastRaw;
  bool stable;
  unsigned long lastChange;
};

Button btnAsk  = { PIN_BTN_ASK,  HIGH, HIGH, 0 };
Button btnUp   = { PIN_BTN_UP,   HIGH, HIGH, 0 };
Button btnDown = { PIN_BTN_DOWN, HIGH, HIGH, 0 };

// Mengembalikan true PERSIS SEKALI tiap kali tombol ditekan (falling edge
// LOW setelah di-debounce). Tombol aktif LOW (INPUT_PULLUP + kaki ke GND).
bool checkButtonPressed(Button &b) {
  bool raw = digitalRead(b.pin);
  if (raw != b.lastRaw) {
    b.lastChange = millis();
  }
  bool pressedEdge = false;
  if (millis() - b.lastChange > DEBOUNCE_MS) {
    if (raw != b.stable) {
      b.stable = raw;
      if (b.stable == LOW) pressedEdge = true;
    }
  }
  b.lastRaw = raw;
  return pressedEdge;
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
// Util tampilan: tulis judul status + body teks dengan word-wrap sederhana.
// Two-pass: kumpulkan dulu baris-baris hasil wrap ke array (maks
// MAX_BODY_LINES), baru digambar. Ini mencegah baris meluber ke luar batas
// layar (yang sebelumnya menyebabkan tampilan terlihat "bertumpuk" saat
// jawaban panjang) karena jumlah baris + indikator "..." sudah dijatah pasti
// masuk dalam MAX_BODY_LINES, bukan ditambah di akhir seperti sebelumnya.
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
      // cari spasi terakhir sebelum batas kolom, biar wrap per kata
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
      // kata tunggal lebih panjang dari 1 baris -> potong paksa
      chunk = remaining.substring(0, CHARS_PER_LINE);
      remaining = remaining.substring(CHARS_PER_LINE);
    }
    lines[lineCount++] = chunk;
  }

  // Kalau masih ada sisa teks setelah MAX_BODY_LINES baris penuh, timpa baris
  // TERAKHIR dengan indikator "..." (bukan menambah baris baru di bawahnya),
  // supaya total baris yang digambar tetap <= MAX_BODY_LINES.
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

// Mata "senang": chevron melengkung ke atas ( ^ ), dipakai utk FACE_HAPPY
// dan juga saat mata "tersenyum menyipit".
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

  // Antena: terkulai saat sedih, tegak selain itu
  if (expr == FACE_SAD) {
    display.drawLine(antX, headY, antX - 5, headY - 6, SSD1306_WHITE);
    display.fillCircle(antX - 5, headY - 6, 2, SSD1306_WHITE);
  } else {
    display.drawLine(antX, headY - 6, antX, headY, SSD1306_WHITE);
    display.fillCircle(antX, headY - 8, 2, SSD1306_WHITE);
  }

  // Kepala
  display.drawRoundRect(headX, headY, headW, headH, 8, SSD1306_WHITE);

  // Mata digeser kiri/kanan sedikit untuk kesan "melirik"
  int eyeShift = (look == LOOK_LEFT) ? -3 : (look == LOOK_RIGHT) ? 3 : 0;
  int eyeY = headY + 14;
  int lEyeX = headX + 11 + eyeShift;
  int rEyeX = headX + headW - 21 + eyeShift;
  const int EW = 10, EH = 14;

  if (expr == FACE_SAD) {
    // Mata sedih: "X" di tiap mata (abaikan arah lirik/kedip)
    display.drawLine(lEyeX, eyeY, lEyeX + EW, eyeY + EH, SSD1306_WHITE);
    display.drawLine(lEyeX + EW, eyeY, lEyeX, eyeY + EH, SSD1306_WHITE);
    display.drawLine(rEyeX, eyeY, rEyeX + EW, eyeY + EH, SSD1306_WHITE);
    display.drawLine(rEyeX + EW, eyeY, rEyeX, eyeY + EH, SSD1306_WHITE);
  } else if (blinking) {
    // Mata terpejam: garis tipis datar
    display.drawLine(lEyeX, eyeY + 7, lEyeX + EW, eyeY + 7, SSD1306_WHITE);
    display.drawLine(rEyeX, eyeY + 7, rEyeX + EW, eyeY + 7, SSD1306_WHITE);
  } else if (expr == FACE_HAPPY) {
    // Mata senang: chevron melengkung ( ^ ^ )
    drawChevronEye(lEyeX, eyeY + 7);
    drawChevronEye(rEyeX, eyeY + 7);
  } else {
    // Mata normal: kotak membulat terisi penuh
    display.fillRoundRect(lEyeX, eyeY, EW, EH, 3, SSD1306_WHITE);
    display.fillRoundRect(rEyeX, eyeY, EW, EH, 3, SSD1306_WHITE);
  }

  int mouthY = headY + headH - 10;
  int mcx = headX + headW / 2;
  if (expr == FACE_SAD) {
    // Cemberut: lengkungan terbalik
    display.drawLine(headX + 16, mouthY + 4, mcx, mouthY, SSD1306_WHITE);
    display.drawLine(mcx, mouthY, headX + headW - 16, mouthY + 4, SSD1306_WHITE);
  } else if (expr == FACE_HAPPY) {
    // Senyum: lengkungan normal
    display.drawLine(headX + 16, mouthY, mcx, mouthY + 5, SSD1306_WHITE);
    display.drawLine(mcx, mouthY + 5, headX + headW - 16, mouthY, SSD1306_WHITE);
  } else if (expr == FACE_THINKING) {
    // Bingung: mulut miring sebelah (skeptis)
    display.drawLine(headX + 18, mouthY, headX + headW - 14, mouthY - 3, SSD1306_WHITE);
  } else {
    // Netral: garis datar simpel
    display.drawLine(headX + 16, mouthY, headX + headW - 16, mouthY, SSD1306_WHITE);
  }

  // Caption singkat di bawah wajah
  display.setTextSize(1);
  display.setCursor(0, SCREEN_HEIGHT - 8);
  display.println(caption);

  display.display();
}

// ---------------------------------------------------------
// Animasi idle: kedip & lirik kiri-kanan acak selama di layar standby,
// supaya wajahnya "hidup" alih-alih diam saja. Dipanggil tiap loop() saat
// state == STATE_IDLE; hanya redraw layar kalau ada perubahan.
// ---------------------------------------------------------
unsigned long nextBlinkAt = 0;
unsigned long nextLookAt = 0;
unsigned long blinkEndAt = 0;
unsigned long lookEndAt = 0;
bool idleBlinking = false;
LookDir idleLook = LOOK_CENTER;

void scheduleIdleAnim() {
  unsigned long now = millis();
  nextBlinkAt = now + random(1500, 4000);
  nextLookAt = now + random(2500, 6000);
  idleBlinking = false;
  idleLook = LOOK_CENTER;
}

void updateIdleFace() {
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
// Tampilan jam (NTP) -- muncul saat tombol UP atau DOWN ditekan. Waktu
// disinkronkan lewat NTP (butuh WiFi, otomatis coba sync di setup()).
// Auto-kembali ke standby setelah CLOCK_TIMEOUT_MS tanpa aktivitas, dan
// jarumnya ikut "jalan" (redraw tiap detik) selama layar ini tampil --
// konsisten dengan tema "jangan statis" pada wajah robot.
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
// Panggil NVIDIA NIM API (model Nemotron), kembalikan teks jawaban (atau
// pesan error). Format request/respons mengikuti konvensi OpenAI-compatible
// chat completions yang dipakai build.nvidia.com.
// ---------------------------------------------------------
// Pembersih markdown sederhana. Modelnya diminta jangan pakai markdown di
// system prompt, tapi tidak selalu patuh -- jadi dibersihkan programatik
// di sini juga, supaya notifikasi/OLED tidak kotor simbol ** # dsb.
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

  HTTPClient https;
  String answer;

  if (!https.begin(client, "https://integrate.api.nvidia.com/v1/chat/completions")) {
    return "Gagal membuka koneksi HTTPS";
  }

  // Timeout default HTTPClient (~5 detik) sering kurang -- apalagi untuk
  // jawaban panjang (mode tidak-concise, max_tokens lebih besar) yang makan
  // waktu generate lebih lama di sisi NVIDIA. Kasih jatah lebih longgar.
  // ~24 token/detik diamati dari Nemotron Lightning utk jawaban panjang --
  // kasih jatah waktu yang nyaman di atas estimasi max_tokens / 24.
  https.setTimeout(concise ? 15000 : 45000);
  https.setConnectTimeout(10000);

  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", String("Bearer ") + NVIDIA_API_KEY);

  JsonDocument reqDoc;
  reqDoc["model"] = NVIDIA_MODEL;
  reqDoc["max_tokens"] = concise ? 300 : 800;
  reqDoc["temperature"] = 0.6;
  reqDoc["stream"] = false;
  // Nemotron 3.x adalah model reasoning: tanpa ini, jawabannya ditaruh di
  // field "reasoning_content" (bukan "content") dan bisa menghabiskan semua
  // max_tokens cuma untuk proses "berpikir", sehingga "content" kosong.
  // Matikan reasoning-nya supaya langsung jawab singkat & field content terisi.
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
        // Fallback jaga-jaga: kalau model tetap masuk mode reasoning (field
        // "content" kosong), tampilkan potongan "reasoning_content" daripada
        // kosong total.
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

    // Sinkronisasi jam lewat NTP (butuh WiFi, dicoba sekali di sini).
    // Kalau gagal/timeout, drawClockScreen() nanti akan bilang "belum sync"
    // saat tombol UP/DOWN ditekan -- tidak menghalangi fitur lain.
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

// Ambil pertanyaan dari Firebase (hasil ketikan di app HA Companion di HP,
// diteruskan HA ke Firebase lewat automation). String kosong = tidak
// ada/gagal/null.
String haGetQuestion() {
  if (!haConfigured()) return "";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String result;

  // Query param acak (cache-buster) -- jaga-jaga terhadap kemungkinan
  // caching di jalur GET (proxy/gateway), supaya tidak dapat respons basi.
  String url = firebasePath("question") + "?cb=" + String(millis());
  if (https.begin(client, url)) {
    int code = https.GET();
    if (code == 200) {
      String payload = https.getString();
      payload.trim();
      // Firebase RTDB kembalikan string JSON ber-quote (mis. "halo"), atau
      // literal null kalau belum ada nilai.
      if (payload != "null" && payload.length() >= 2 &&
          payload.startsWith("\"") && payload.endsWith("\"")) {
        result = payload.substring(1, payload.length() - 1);
      }
    }
    https.end();
  }
  return result;
}

// Kosongkan lagi field "question" di Firebase setelah diproses, supaya
// tidak terus-terusan ke-trigger ulang oleh nilai yang sama.
void haClearQuestion() {
  if (!haConfigured()) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;

  if (https.begin(client, firebasePath("question"))) {
    https.addHeader("Content-Type", "application/json");
    https.PUT("null");
    https.end();
  }
}

// Tulis jawaban lengkap ke Firebase -> automation di HA (polling tiap ~5
// detik) mendeteksinya dan mengirim sebagai persistent_notification ke app
// HA Companion di HP, bisa dibaca penuh tanpa batas panjang.
void haNotify(const String &question, const String &answer) {
  if (!haConfigured()) return;
  (void)question; // judul tidak disertakan di sisi Firebase; HA yang format pesan

  WiFiClientSecure client;
  client.setInsecure();
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

  // Animasi singkat "mikir" (lirik kiri-kanan, lalu kedip) sebelum masuk ke
  // layar teks yang menampilkan pertanyaan -- biar transisinya tidak kaku.
  // Dipakai delay() pendek karena ini cuma sekilas (~700ms total), bukan
  // animasi berkelanjutan selama nunggu respons API yang (blocking).
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

  // Selalu juga dikirim ke HA (kalau sudah dikonfigurasi) -- baik pertanyaan
  // datang dari tombol+Serial maupun dari HP, jawabannya tetap bisa dibaca
  // penuh lewat notifikasi app HA Companion.
  haNotify(question, answer);

  setLed(false);

  // Kilas senang sebentar begitu jawaban datang, sebelum tampil teksnya.
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

  pinMode(PIN_BTN_ASK, INPUT_PULLUP);
  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  randomSeed(esp_random());  // biar animasi idle (kedip/lirik) tidak selalu pola sama

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 OLED tidak terdeteksi!");
  }
  display.setRotation(0);

  connectWiFi();
}

void loop() {
  // --- Baca ketiga tombol (debounce via checkButtonPressed()) ---
  bool askPressed  = checkButtonPressed(btnAsk);
  bool upPressed   = checkButtonPressed(btnUp);
  bool downPressed = checkButtonPressed(btnDown);

  // Tombol ASK: mulai tanya (dari IDLE/ANSWERED/CLOCK -- menekan ASK saat
  // lagi lihat jam langsung membatalkan jam & mulai tanya).
  if (askPressed &&
      (state == STATE_IDLE || state == STATE_ANSWERED || state == STATE_CLOCK)) {
    enterListening();
  }

  // Tombol UP/DOWN: tampilkan jam. Dari IDLE/ANSWERED -> masuk STATE_CLOCK.
  // Kalau sudah di STATE_CLOCK, tekan lagi cukup reset timer auto-kembali
  // (drawClockScreen() tetap di-refresh tiap detik oleh blok di bawah).
  if (upPressed || downPressed) {
    if (state == STATE_IDLE || state == STATE_ANSWERED) {
      enterClock();
    } else if (state == STATE_CLOCK) {
      clockShownAt = millis();
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
  // Hanya dicek saat device sedang tidak sibuk (IDLE/ANSWERED), supaya tidak
  // mengganggu alur tombol+Serial yang sedang berjalan.
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
      enterThinking(q, false);  // dari HP -> boleh jawaban lengkap, tidak dipaksa 3 kalimat
      haClearQuestion();
    }
  }

  // --- Auto-kembali ke standby (wajah robot) kalau tidak ada aktivitas ---
  if (state == STATE_ANSWERED && millis() - answeredAtTimer > ANSWERED_TIMEOUT_MS) {
    state = STATE_IDLE;
    drawRobotFace("Siap! Tekan tombol");
    scheduleIdleAnim();
  }

  // --- Animasi idle (kedip & lirik kiri-kanan) selama standby ---
  if (state == STATE_IDLE) {
    updateIdleFace();
  }

  // --- Layar jam: refresh tiap detik (biar jarum detik "jalan"), lalu
  // auto-kembali ke standby setelah CLOCK_TIMEOUT_MS tanpa tombol UP/DOWN
  // ditekan lagi ---
  if (state == STATE_CLOCK) {
    if (millis() - clockLastTick > 1000) {
      clockLastTick = millis();
      drawClockScreen();
    }
    if (millis() - clockShownAt > CLOCK_TIMEOUT_MS) {
      state = STATE_IDLE;
      drawRobotFace("Siap! Tekan tombol");
      scheduleIdleAnim();
    }
  }
}
