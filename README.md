# Pocket AI Assistant — Simulasi Wokwi

Versi simulasi **sementara** dari proyek
[Tiny Pocket AI Assistant (huyvector.org)](https://www.huyvector.org/robots-kinetic/pocket-ai-assistant),
dibuat agar bisa langsung dicoba di **VS Code** lewat ekstensi **Wokwi for VS Code**,
tanpa perlu hardware fisik.

## Hardware asli vs. versi simulasi

| Bagian | Hardware asli (huyvector.org) | Versi simulasi ini |
|---|---|---|
| MCU | ESP32-C3 | ESP32-C3 DevKitM-1 (sama) |
| Tampilan | OLED | OLED SSD1306 128x64 I2C (sama) |
| Input suara | Microphone module (I2S) | **Diganti**: ketik teks di Serial Monitor |
| Output suara | Speaker + ampli MAX98357 | **Diganti**: teks di OLED + Serial Monitor |
| Firmware | ESPHome | Arduino (C++) via PlatformIO |
| Otak AI | Tidak disebutkan di web aslinya | NVIDIA NIM API (model Nemotron) lewat HTTPS |

**Kenapa diganti?** Wokwi belum punya simulasi audio I2S yang nyata — tidak bisa
merekam suara asli dari mic atau memutar suara dari speaker virtual. Supaya kamu
tetap bisa mencoba *alur kerja* asisten AI-nya (dengar pertanyaan → pikir →
jawab) secara nyata (bukan cuma mock), suara digantikan dengan teks lewat Serial
Monitor. WiFi dan pemanggilan API-nya **asli**, bukan simulasi/mock — ESP32
benar-benar memanggil internet lewat gateway Wokwi.

> Project ini awalnya dicoba dengan Anthropic Claude API, lalu dipindah ke
> NVIDIA NIM (Nemotron) karena build.nvidia.com menyediakan free API credits —
> cocok untuk eksperimen tanpa perlu isi saldo dulu. Mau balik ke Claude tinggal
> ganti isi `askNvidia()` di `src/main.cpp`: endpoint jadi
> `api.anthropic.com/v1/messages`, header auth jadi `x-api-key` +
> `anthropic-version`, dan field respons jadi `content[0].text` (bukan
> `choices[0].message.content`).

## Cara kerja

```
[Tombol ditekan] -> status LISTENING (LED kedip)
      |
      v
Ketik pertanyaan di Serial Monitor, tekan Enter
      |
      v
status THINKING (LED nyala terus) -> panggil NVIDIA NIM API (Nemotron) via WiFi+HTTPS
      |
      v
Jawaban ditampilkan di OLED (ringkas) + Serial Monitor (lengkap)
      |
      v
Tekan tombol lagi untuk bertanya hal baru
```

## Struktur file

```
pocket-ai-assistant/
├── diagram.json          # rangkaian: ESP32-C3 + OLED + tombol + LED
├── wokwi.toml             # konfigurasi simulator Wokwi (VS Code)
├── platformio.ini         # konfigurasi build (board, library)
├── src/
│   ├── main.cpp           # firmware utama
│   ├── secrets.h.example  # template WiFi + API key (salin jadi secrets.h)
│   └── secrets.h          # (dibuat sendiri, di-gitignore, isi API key asli)
└── .gitignore
```

## Setup

1. **Install ekstensi VS Code**:
   - `PlatformIO IDE`
   - `Wokwi for VS Code` (butuh login/akun Wokwi gratis untuk menjalankan simulasi)
2. **Siapkan API key NVIDIA**:
   ```bash
   cp src/secrets.h.example src/secrets.h
   ```
   Lalu edit `src/secrets.h`, isi `NVIDIA_API_KEY` dengan key dari
   https://build.nvidia.com (daftar akun → generate API key → biasanya
   langsung dapat free credits). File ini sudah di-`.gitignore` supaya key
   tidak ikut ter-commit.
3. **Build firmware**:
   ```bash
   pio run
   ```
   (sudah diuji berhasil compile di environment ini — ukuran flash ~72%, RAM ~12%)
4. **Jalankan simulasi**: buka `diagram.json` di VS Code, lalu tekan tombol
   ▶️ "Start Simulation" dari ekstensi Wokwi (atau `F1` → `Wokwi: Start Simulator`).

## Cara pakai di simulator

1. Tunggu OLED menampilkan `WiFi OK. Tekan tombol untuk bertanya.`
2. Klik tombol biru di diagram (atau pakai keyboard shortcut yang ditampilkan Wokwi).
3. Buka **Serial Monitor**, ketik pertanyaan, tekan Enter — misalnya:
   `Jam berapa sekarang di Jakarta kalau di London jam 9 pagi?`
4. OLED akan menampilkan `Thinking...`, lalu jawaban singkat dari Nemotron.
   Jawaban lengkap selalu tercetak penuh di Serial Monitor.
5. Tekan tombol lagi untuk bertanya hal lain.

## Catatan teknis

- WiFi memakai SSID **`Wokwi-GUEST`** — ini SSID khusus simulator Wokwi yang
  memberi akses internet asli lewat gateway mereka. Jangan diganti selama masih
  di simulator.
- `WiFiClientSecure::setInsecure()` dipakai untuk HTTPS supaya tidak perlu
  mengelola root CA certificate secara manual. Ini **aman untuk
  demo/simulasi** tapi sebaiknya diganti dengan verifikasi sertifikat yang benar
  kalau nanti dipakai di produk sungguhan.
- Model default: `nvidia/llama-3.3-nemotron-super-49b-v1`. Varian lain (mis.
  `nemotron-3-super-120b-a12b`, `llama-3.1-nemotron-ultra-253b-v1`) bisa
  dipilih lewat `NVIDIA_MODEL` di `secrets.h` — cek ID persis di katalog
  build.nvidia.com karena bisa berubah.
- Request menyertakan `"chat_template_kwargs": {"thinking": false}` — Nemotron
  3.x adalah model reasoning yang defaultnya menaruh jawaban di field
  `reasoning_content` (bukan `content`) dan bisa menghabiskan `max_tokens`
  hanya untuk proses "berpikir" sampai `content` kosong. Flag ini mematikan
  reasoning supaya langsung jawab singkat dan `content` terisi.
- Pin mapping (lihat `diagram.json`): I2C OLED di GPIO4 (SDA) / GPIO5 (SCL),
  tombol di GPIO3 (aktif LOW), LED status di GPIO10.

## Lanjut ke hardware fisik (sesuai desain asli)

Kalau nanti pindah ke board ESP32-C3 + OLED + mic + MAX98357 + speaker yang
asli, ada dua jalur:

- **Tetap pakai firmware custom ini**: ganti bagian `Serial.readStringUntil()`
  di `src/main.cpp` dengan hasil Speech-to-Text dari mic I2S asli (misalnya pakai
  library I2S + layanan STT cloud, atau ESP-SR untuk wake-word on-device), dan
  tambahkan output I2S ke speaker lewat MAX98357 untuk Text-to-Speech dari
  jawaban AI.
- **Pindah ke ESPHome + Home Assistant Assist** (konsisten dengan stack
  ESPHome yang sudah kamu pakai di project weather station): pakai komponen
  `voice_assistant` ESPHome yang terhubung ke pipeline Assist di Home Assistant
  — HA yang mengurus wake-word, STT, pemanggilan LLM (bisa diarahkan ke
  NVIDIA/Anthropic/OpenAI lewat integrasi conversation agent), dan TTS.
