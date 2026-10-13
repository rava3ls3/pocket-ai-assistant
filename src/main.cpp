/*
 * Pocket AI Assistant — versi simulasi (Wokwi)
 * ------------------------------------------------
 * Terinspirasi dari: https://www.huyvector.org/robots-kinetic/pocket-ai-assistant
 * (ESP32-C3 + OLED + mic I2S + ampli MAX98357 + speaker, firmware ESPHome)
 *
 * KETERBATASAN SIMULASI:
 * Wokwi belum punya simulasi audio I2S yang nyata (tidak bisa merekam suara
 * asli dari mic maupun memutar suara dari speaker). Karena itu, versi
 * sementara ini memakai Serial Monitor sebagai pengganti suara:
 *
 *   1. Tekan tombol (push-to-talk)      -> status "LISTENING"
 *   2. Ketik pertanyaan di Serial Monitor, lalu Enter
 *   3. ESP32 memanggil NVIDIA NIM API (model Nemotron) lewat WiFi (internet
 *      asli, lewat gateway simulasi Wokwi)   -> status "THINKING"
 *   4. Jawaban ditampilkan di OLED (ringkas) dan di Serial Monitor (lengkap)
 *
 * Nanti kalau dipindah ke hardware fisik, cukup ganti bagian "ambil teks
 * pertanyaan" dari Serial.readStringUntil() menjadi hasil STT dari mic asli
 * (mis. lewat ESPHome voice_assistant + Home Assistant Assist, atau layanan
 * STT cloud), dan tambahkan I2S speaker (MAX98357) untuk TTS. Semua bagian
 * lain (state machine, pemanggilan API, tampilan OLED) tetap sama.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "secrets.h"

// ---------- Pin mapping (lihat diagram.json) ----------
static const int PIN_I2C_SDA = 4;
static const int PIN_I2C_SCL = 5;
static const int PIN_BUTTON  = 3;   // push-to-talk, aktif LOW (ditarik ke GND saat ditekan)
static const int PIN_LED     = 10;  // indikator status

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
enum AssistantState { STATE_IDLE, STATE_LISTENING, STATE_THINKING, STATE_ANSWERED };
AssistantState state = STATE_IDLE;

String inputBuffer;      // buffer teks pertanyaan dari Serial
unsigned long ledTimer = 0;
bool ledOn = false;

// Debounce tombol
bool lastButtonRaw = HIGH;
unsigned long lastDebounceTime = 0;
static const unsigned long DEBOUNCE_MS = 40;

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

// Ekspresi wajah robot yang bisa digambar
enum RobotExpression { FACE_NEUTRAL, FACE_SAD };

// ---------------------------------------------------------
// Wajah robot sederhana (digambar pakai primitif Adafruit_GFX, tanpa perlu
// file bitmap terpisah). Dipakai untuk layar standby/idle (netral) dan
// layar WiFi gagal (sedih).
// ---------------------------------------------------------
void drawRobotFace(const String &caption, RobotExpression expr = FACE_NEUTRAL) {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  const int headW = 56, headH = 44;
  const int headX = (SCREEN_WIDTH - headW) / 2;  // 36
  const int headY = 4;

  if (expr == FACE_NEUTRAL) {
    // Antena tegak - ekspresi siap/netral
    int antX = SCREEN_WIDTH / 2;
    display.drawLine(antX, headY - 6, antX, headY, SSD1306_WHITE);
    display.fillCircle(antX, headY - 8, 2, SSD1306_WHITE);
  } else {
    // Antena terkulai - ekspresi sedih/lesu
    int antX = SCREEN_WIDTH / 2;
    display.drawLine(antX, headY, antX - 5, headY - 6, SSD1306_WHITE);
    display.fillCircle(antX - 5, headY - 6, 2, SSD1306_WHITE);
  }

  // Kepala
  display.drawRoundRect(headX, headY, headW, headH, 8, SSD1306_WHITE);

  int eyeY = headY + 14;
  if (expr == FACE_NEUTRAL) {
    // Mata: dua kotak membulat
    display.fillRoundRect(headX + 11, eyeY, 10, 14, 3, SSD1306_WHITE);
    display.fillRoundRect(headX + headW - 21, eyeY, 10, 14, 3, SSD1306_WHITE);
  } else {
    // Mata sedih: "X" di tiap mata
    int exL = headX + 11, exR = headX + headW - 21, ew = 10, eh = 14;
    display.drawLine(exL, eyeY, exL + ew, eyeY + eh, SSD1306_WHITE);
    display.drawLine(exL + ew, eyeY, exL, eyeY + eh, SSD1306_WHITE);
    display.drawLine(exR, eyeY, exR + ew, eyeY + eh, SSD1306_WHITE);
    display.drawLine(exR + ew, eyeY, exR, eyeY + eh, SSD1306_WHITE);
  }

  int mouthY = headY + headH - 10;
  if (expr == FACE_NEUTRAL) {
    // Mulut: garis datar simpel
    display.drawLine(headX + 16, mouthY, headX + headW - 16, mouthY, SSD1306_WHITE);
  } else {
    // Mulut sedih: lengkungan terbalik (cemberut)
    int mcx = headX + headW / 2;
    display.drawLine(headX + 16, mouthY + 4, mcx, mouthY, SSD1306_WHITE);
    display.drawLine(mcx, mouthY, headX + headW - 16, mouthY + 4, SSD1306_WHITE);
  }

  // Caption singkat di bawah wajah
  display.setTextSize(1);
  display.setCursor(0, SCREEN_HEIGHT - 8);
  display.println(caption);

  display.display();
}

void setLed(bool on) {
  ledOn = on;
  digitalWrite(PIN_LED, on ? HIGH : LOW);
}

// ---------------------------------------------------------
// Panggil NVIDIA NIM API (model Nemotron), kembalikan teks jawaban (atau
// pesan error). Format request/respons mengikuti konvensi OpenAI-compatible
// chat completions yang dipakai build.nvidia.com.
// ---------------------------------------------------------
String askNvidia(const String &question) {
  WiFiClientSecure client;
  client.setInsecure(); // NOTE: skip verifikasi sertifikat TLS -> OK untuk simulasi/demo,
                        // JANGAN dipakai apa adanya untuk produk nyata.

  HTTPClient https;
  String answer;

  if (!https.begin(client, "https://integrate.api.nvidia.com/v1/chat/completions")) {
    return "Gagal membuka koneksi HTTPS";
  }

  https.addHeader("Content-Type", "application/json");
  https.addHeader("Authorization", String("Bearer ") + NVIDIA_API_KEY);

  JsonDocument reqDoc;
  reqDoc["model"] = NVIDIA_MODEL;
  reqDoc["max_tokens"] = 300;
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
  sysMsg["content"] =
      "Kamu adalah asisten AI mini di sebuah perangkat saku (pocket AI "
      "assistant) dengan layar OLED kecil. Jawab singkat dan jelas, maksimal "
      "3 kalimat pendek. Gunakan Bahasa Indonesia kecuali ditanya dalam "
      "bahasa lain.";
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
    drawRobotFace("Siap! Tekan tombol");
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

void enterThinking(const String &question) {
  state = STATE_THINKING;
  setLed(true);
  showScreen("Thinking...", question);
  Serial.println("Mengirim ke NVIDIA NIM API: \"" + question + "\"");

  String answer = askNvidia(question);

  Serial.println("Jawaban lengkap:");
  Serial.println(answer);

  setLed(false);
  state = STATE_ANSWERED;
  showScreen("Jawaban:", answer);
}

// ---------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nPocket AI Assistant (simulasi Wokwi) - boot");

  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("SSD1306 OLED tidak terdeteksi!");
  }
  display.setRotation(0);

  connectWiFi();
}

void loop() {
  // --- Baca tombol dengan debounce ---
  bool raw = digitalRead(PIN_BUTTON);
  if (raw != lastButtonRaw) {
    lastDebounceTime = millis();
  }
  if (millis() - lastDebounceTime > DEBOUNCE_MS) {
    static bool stableState = HIGH;
    if (raw != stableState) {
      stableState = raw;
      // tombol aktif LOW (ditekan = LOW karena INPUT_PULLUP + kaki ke GND)
      if (stableState == LOW) {
        if (state == STATE_IDLE || state == STATE_ANSWERED) {
          enterListening();
        }
      }
    }
  }
  lastButtonRaw = raw;

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
}
