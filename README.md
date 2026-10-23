# Pocket AI Assistant — Simulasi Wokwi

Versi simulasi **sementara** dari proyek
[Tiny Pocket AI Assistant (huyvector.org)](https://www.huyvector.org/robots-kinetic/pocket-ai-assistant),
dibuat agar bisa langsung dicoba di **VS Code** lewat ekstensi **Wokwi for VS Code**,
tanpa perlu hardware fisik.

## Hardware asli vs. versi simulasi

| Bagian | Hardware asli (huyvector.org) | Versi simulasi ini |
|---|---|---|
| MCU | ESP32-C3 | **Seeed Studio XIAO ESP32-S3** |
| Tampilan | OLED | OLED SSD1306 128x64 I2C (sama) |
| Input | Microphone (I2S) + 1 tombol | **Diganti**: 3 tombol (ASK/UP/DOWN) + ketik teks di Serial Monitor |
| Output suara | Speaker + ampli MAX98357 | **Diganti**: teks di OLED + Serial Monitor |
| Firmware | ESPHome | Arduino (C++) via PlatformIO |
| Otak AI | Tidak disebutkan di web aslinya | NVIDIA NIM API (model Nemotron) lewat HTTPS |

**Kenapa pindah ke XIAO ESP32-S3?** Bentuknya jauh lebih kecil/ringkas untuk
device saku (form factor ~21x17.8mm), dan punya cukup pin bebas untuk 3
tombol fisik (ASK, UP, DOWN) tanpa sesak.

**3 tombol:**
| Tombol | Pin | Klik tunggal | Klik GANDA (2x tekan dalam ≤1 detik) | Fungsi nanti (hardware asli + speaker) |
|---|---|---|---|---|
| ASK | D0 | Push-to-talk (mulai tanya ke AI dari layar rumah manapun), atau "kembali" selama di dalam menu Pengaturan | Khusus di layar **Pengaturan**: masuk LIST. Khusus di LIST (item yg bisa diubah): masuk mode edit nilai | Sama |
| UP | D1 | **Maju** 1 langkah di loop layar rumah (AI → Jam → Pengaturan → AI → ...) / pindah item berikutnya (LIST) / naikkan nilai (mode edit) | — | Volume up |
| DOWN | D3 | **Mundur** 1 langkah di loop layar rumah (AI → Pengaturan → Jam → AI → ...) / pindah item sebelumnya (LIST) / turunkan nilai (mode edit) | — | Volume down |

**Navigasi utama pakai UP/DOWN**, bukan klik-ganda langsung dari mana saja —
klik-ganda dari sembarang layar ternyata terasa sulit/kurang jelas waktu
dicoba. Sekarang ada 3 "layar rumah" yang di-loop pakai UP/DOWN:

```
   ┌──────┐   UP   ┌─────┐   UP   ┌─────────────┐
   │  AI  │ ─────> │ Jam │ ─────> │ Pengaturan  │
   │(idle)│ <───── │     │ <───── │ (launcher)  │
   └──────┘  DOWN  └─────┘  DOWN  └─────────────┘
       ^                                 │
       └─────────── UP (loop balik) ─────┘
```

Tekan ASK kapan saja (di layar rumah manapun) untuk langsung mulai tanya.
Begitu berada TEPAT di layar **Pengaturan**, klik ASK dua kali cepat (≤1
detik antar klik) untuk masuk menu (lihat bagian terpisah di bawah) —
karena klik-ganda cuma perlu dipakai pas sudah lihat layar yang tepat,
jauh lebih gampang ditimpali drpd klik-ganda "buta" dari standby.

> Karena klik tunggal dan klik ganda sama-sama dimulai dengan menekan ASK,
> firmware perlu menunggu sampai ~1 detik setelah tiap klik tunggal untuk
> memastikan tidak ada klik kedua menyusul sebelum benar-benar memprosesnya
> (standar untuk deteksi klik-ganda). Jadi aksi ASK klik-tunggal (mulai
> tanya / kembali) terasa delay sepersekian detik — ini disengaja, bukan bug.

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
[Standby] wajah robot netral di OLED (kedip & lirik kiri-kanan acak,
          tidak diam saja), nunggu tombol / pertanyaan dari HP
      |
      v
[Tombol ditekan] -> status LISTENING (LED kedip)
      |
      v
Ketik pertanyaan di Serial Monitor, tekan Enter
      |                                         \
      v                                          \ (opsional, lewat HP)
wajah "Mikir..." sekilas (lirik kiri/kanan,       Ketik di app HA Companion
lalu kedip) -> status THINKING (LED nyala)        -> HA tulis ke Firebase
-> panggil NVIDIA NIM API (Nemotron)              -> ESP32 polling & deteksi
      |                                          /
      v                                         /
wajah "Ketemu!" (senang) sekilas, lalu:
Jawaban ditampilkan di OLED (ringkas, word-wrap 6 baris)
+ dicetak penuh di Serial Monitor
+ (kalau HA dikonfigurasi) dikirim sebagai notifikasi ke app HA Companion di HP
      |
      v
Tekan tombol lagi untuk bertanya hal baru, ATAU
tunggu 15 detik -> otomatis balik ke wajah robot standby
```

Kalau WiFi gagal connect, OLED menampilkan wajah robot sedih (mata "X",
mulut cemberut, antena terkulai) alih-alih teks error biasa.

### Wajah robot interaktif

Wajah di layar standby tidak statis — ada animasi idle acak biar terasa
"hidup":
- **Kedip** tiap beberapa detik (mata jadi garis tipis sesaat)
- **Lirik kiri/kanan** sesekali (mata bergeser dari posisi tengah)

Ekspresi lain (semua digambar pakai primitif `Adafruit_GFX`, tanpa file
gambar/bitmap terpisah):
| Ekspresi | Kapan muncul | Ciri |
|---|---|---|
| Netral | Standby | Mata kotak membulat, mulut datar |
| **Mikir** | Sekilas (~700ms) saat mulai memproses pertanyaan | Mulut miring, mata lirik kiri→kanan→kedip |
| **Senang** | Sekilas (~600ms) begitu jawaban diterima | Mata melengkung "^ ^", senyum |
| **Sedih** | WiFi gagal connect | Mata "X", mulut cemberut, antena terkulai |

## Menu Pengaturan

Navigasinya tiga tingkat:

**Tingkat HOME** (layar "Pengaturan", salah satu dari 3 layar rumah —
lihat loop AI→Jam→Pengaturan di atas):
- **UP / DOWN** → pindah ke layar rumah lain (loop AI↔Jam↔Pengaturan)
- **ASK (klik ganda, ≤1 detik)** → masuk tingkat LIST
- **ASK (klik tunggal)** → langsung mulai tanya ke AI (sama seperti di
  layar rumah lain)

**Tingkat LIST** (daftar 6 item):
- **UP / DOWN** → pindah ke item berikutnya/sebelumnya
- **ASK (klik ganda)** → masuk mode edit item yang sedang dipilih (hanya
  untuk item yang bisa diubah: Kecerahan/Volume/Timeout layar — klik ganda
  di item info-saja seperti WiFi tidak ngapa-ngapain)
- **ASK (klik tunggal)** → keluar satu tingkat, balik ke layar HOME
  (Pengaturan)

**Tingkat EDIT** (sedang mengubah nilai 1 item, header layar berubah jadi
"Edit X/6"):
- **UP / DOWN** → naikkan/turunkan nilai item
- **ASK (klik tunggal)** → kembali ke tingkat LIST (nilai sudah otomatis
  tersimpan)

Di tingkat LIST maupun EDIT, otomatis keluar satu tingkat & simpan ke NVS
kalau tidak ada tombol ditekan selama 15 detik.

6 item yang bisa dilihat/diatur:
| Item | Bisa diubah? | Keterangan |
|---|---|---|
| **Kecerahan** | Ya (Rendah/Sedang/Tinggi) | Kontrol kontras OLED asli (`ssd1306_command`), bukan cuma label |
| **Volume** | Ya (0/25/50/75/100%) | Tersimpan, tapi belum berpengaruh ke suara apa pun — menunggu speaker fisik (MAX98357) |
| **WiFi** | Tidak (info saja) | SSID, IP, dan kekuatan sinyal (RSSI) saat ini |
| **Timeout layar** | Ya (Tidak pernah/30/60/120 detik) | Setelah idle selaman itu saat standby, panel OLED dimatikan hemat daya (`SSD1306_DISPLAYOFF`); tombol apa pun menyalakannya lagi |
| **Baterai** | Tidak (info saja) | XIAO ESP32-S3 tidak punya pin sensor baterai bawaan yang standar, jadi ditampilkan sebagai keterangan, bukan angka dikira-kira |
| **Firmware** | Tidak (info saja) | Versi firmware, tanggal build, dan sisa heap RAM |

Nilai Kecerahan, Volume, dan Timeout layar disimpan ke NVS (`Preferences`)
sehingga tetap tersimpan setelah ESP32 di-restart atau kehilangan daya.

## Struktur file

```
pocket-ai-assistant/
├── diagram.json          # rangkaian: XIAO ESP32-S3 + OLED + 3 tombol + LED
├── wokwi.toml             # konfigurasi simulator Wokwi (VS Code)
├── platformio.ini         # konfigurasi build (board, library)
├── src/
│   ├── main.cpp           # firmware utama
│   ├── secrets.h.example  # template WiFi + API key (salin jadi secrets.h)
│   └── secrets.h          # (dibuat sendiri, di-gitignore, isi API key asli)
├── web/
│   └── index.html         # halaman web opsional (lihat bawah), deploy ke Firebase Hosting
├── firebase.json          # konfigurasi Firebase Hosting
├── .firebaserc            # project ID Firebase default
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
   (sudah diuji berhasil compile di environment ini — ukuran flash ~27%, RAM ~14%
   dari kapasitas XIAO ESP32-S3 yang jauh lebih besar dari ESP32-C3)
4. **Jalankan simulasi**: buka `diagram.json` di VS Code, lalu tekan tombol
   ▶️ "Start Simulation" dari ekstensi Wokwi (atau `F1` → `Wokwi: Start Simulator`).

## Cara pakai di simulator

1. Tunggu OLED menampilkan wajah robot standby ("Siap! Tekan tombol").
2. Klik tombol **merah (ASK)** di diagram untuk mulai tanya.
3. Buka **Serial Monitor**, ketik pertanyaan, tekan Enter — misalnya:
   `Jam berapa sekarang di Jakarta kalau di London jam 9 pagi?`
4. OLED akan menampilkan animasi "Mikir...", lalu jawaban singkat dari Nemotron.
   Jawaban lengkap selalu tercetak penuh di Serial Monitor.
5. Tekan tombol **biru (UP)** kapan saja untuk pindah ke layar Jam (NTP),
   tekan UP/DOWN lagi untuk lanjut ke layar Pengaturan, dan seterusnya
   berputar (AI → Jam → Pengaturan → AI → ...).
6. Tekan ASK lagi (klik tunggal) dari layar manapun untuk bertanya hal baru.

## Tanya/baca jawaban panjang lewat HP (opsional, via Home Assistant + Firebase)

Layar OLED kecil (128x64) cuma muat ~6 baris teks. Kalau mau tanya/baca
panjang tanpa batas itu, ada jalur opsional lewat **app Home Assistant
Companion** di HP — tidak perlu bikin app Android sendiri, dan **HA tidak
perlu bisa diakses dari internet** (tidak perlu tunnel/Nabu Casa/port-forward
sama sekali).

**Kenapa lewat Firebase, bukan ESP32 connect langsung ke HA?** Supaya kedua
sisi (ESP32 *dan* HA) cuma perlu koneksi **keluar** (outbound) ke Firebase
Realtime Database — URL-nya permanen, tidak pernah berubah walau HA/Docker
di-restart berkali-kali. Alurnya:

```
[HP] ketik di helper HA  →  HA (automation, outbound) → tulis ke Firebase
                                                              ↓
                                  ESP32 (outbound) polling Firebase tiap ~4 detik
                                                              ↓
                                                   panggil NVIDIA Nemotron
                                                              ↓
                                            ESP32 tulis jawaban ke Firebase (outbound)
                                                              ↓
                       HA (automation, outbound, polling tiap 5 detik) baca → notifikasi ke HP
```

### Setup (sekali saja)

1. **Buat project Firebase** (gratis) di https://console.firebase.google.com,
   lalu aktifkan **Realtime Database** (pilih lokasi terdekat, mode "locked"/
   secure default).
2. **Set Security Rules**-nya supaya HANYA 1 path rahasia yang publik
   (read+write), path lain tetap terkunci. Generate path acak panjang dulu:
   ```bash
   echo "pocket_ai_$(openssl rand -hex 16)"
   ```
   Lalu di Firebase Console → Realtime Database → Rules, isi (ganti
   `PATH_ACAK_KAMU` dengan hasil di atas):
   ```json
   {
     "rules": {
       "PATH_ACAK_KAMU": {
         ".read": true,
         ".write": true
       }
     }
   }
   ```
   ⚠️ **Trade-off keamanan**: siapa pun yang tahu path acak ini bisa
   baca/tulis ke situ (tanpa login). Cukup aman untuk data Q&A yang tidak
   sensitif (bukan untuk data pribadi/finansial), karena path-nya praktis
   tidak bisa ditebak. Path lain di database tetap tertutup penuh.
3. Isi `FIREBASE_DB_URL` dan `FIREBASE_SECRET_PATH` di `src/secrets.h`
   (lihat `secrets.h.example`).
4. **Di Home Assistant**, buat 1 helper teks (Settings → Devices & Services
   → tab **Helpers** → **+ Create Helper** → **Text** → Name: `pocket ai
   question`), lalu tambahkan ke `configuration.yaml`:
   ```yaml
   rest_command:
     pocket_ai_push_question:
       url: "https://<DB_URL>/<PATH_ACAK_KAMU>/question.json"
       method: put
       payload: '"{{ states("input_text.pocket_ai_question") | replace("\"", "") }}"'
       content_type: "application/json"
     pocket_ai_clear_answer:
       url: "https://<DB_URL>/<PATH_ACAK_KAMU>/answer.json"
       method: put
       payload: "null"
       content_type: "application/json"

   sensor:
     - platform: rest
       name: pocket_ai_answer
       resource: "https://<DB_URL>/<PATH_ACAK_KAMU>/answer.json"
       value_template: "{{ value_json }}"
       scan_interval: 5
   ```
   Dan ke `automations.yaml`:
   ```yaml
   - alias: "Pocket AI - kirim pertanyaan ke Firebase"
     trigger:
       - platform: state
         entity_id: input_text.pocket_ai_question
     condition:
       - condition: template
         value_template: "{{ trigger.to_state.state not in ['', 'unknown', 'unavailable'] }}"
     action:
       - service: rest_command.pocket_ai_push_question
       - delay: "00:00:01"
       - service: input_text.set_value
         target: { entity_id: input_text.pocket_ai_question }
         data: { value: "" }

   - alias: "Pocket AI - notifikasi jawaban dari Firebase"
     trigger:
       - platform: state
         entity_id: sensor.pocket_ai_answer
     condition:
       - condition: template
         value_template: "{{ trigger.to_state.state not in ['', 'unknown', 'unavailable', 'None'] }}"
     action:
       - service: persistent_notification.create
         data:
           title: "Pocket AI"
           message: "{{ trigger.to_state.state }}"
           notification_id: "pocket_ai_answer"
       - service: rest_command.pocket_ai_clear_answer
   ```
5. Restart HA supaya `rest_command` & sensor baru kepakai.

### Cara pakai

Ketik pertanyaan di helper `pocket ai question` lewat dashboard app HA
Companion di HP (Settings → Devices & Services → Helpers → tap helper-nya) →
ESP32 mendeteksinya lewat Firebase (cek tiap ~4 detik saat tidak sibuk) →
jawaban lengkap Nemotron dikirim balik lewat Firebase → HA mendeteksinya
(cek tiap 5 detik) → muncul sebagai **notifikasi** di app HA Companion,
bisa dibaca penuh tanpa batas panjang OLED. Pertanyaan lewat tombol+Serial
juga tetap mengirim notifikasi yang sama ke HP.

Kosongkan `FIREBASE_DB_URL` (`""`) di `secrets.h` untuk mematikan fitur ini
— firmware tetap jalan normal lewat tombol+Serial+OLED saja.

### Alternatif/tambahan: halaman web sendiri (Firebase Hosting)

Selain lewat app HA Companion, ada juga halaman web ringan (`web/index.html`)
yang di-deploy ke **Firebase Hosting** — dapat URL permanen gratis seperti
`https://<project-id>.web.app`. Halaman ini connect **langsung** ke Firebase
Realtime Database yang sama (bukan lewat HA sama sekali), jadi:

- Tidak perlu app HA Companion / buka Settings → Helpers
- Jawaban muncul **real-time** (pakai Server-Sent Events bawaan REST API
  Firebase, bukan polling) begitu ESP32 selesai menjawab
- Bisa dibuka dari browser mana saja (HP/PC), tanpa install apa pun

Deploy (sekali saja, butuh akun Firebase + `firebase login` di
[Firebase CLI](https://firebase.google.com/docs/cli)):
```bash
firebase deploy --only hosting --project <project-id-kamu>
```
`web/index.html` berisi `DB_URL` dan `SECRET_PATH` yang di-hardcode langsung
di JavaScript sisi klien (sengaja, karena path rahasia itu sendiri yang
jadi "kunci" akses — lihat security rules di atas) — ganti dengan punyamu
sebelum deploy.

## Catatan teknis

- WiFi memakai SSID **`Wokwi-GUEST`** — ini SSID khusus simulator Wokwi yang
  memberi akses internet asli lewat gateway mereka. Jangan diganti selama masih
  di simulator.
- `WiFiClientSecure::setInsecure()` dipakai untuk HTTPS supaya tidak perlu
  mengelola root CA certificate secara manual. Ini **aman untuk
  demo/simulasi** tapi sebaiknya diganti dengan verifikasi sertifikat yang benar
  kalau nanti dipakai di produk sungguhan.
- Model default: `nvidia/nemotron-3.5-lightning-30b-a3b`. Varian lain (mis.
  `nemotron-3-super-120b-a12b`, `nemotron-3-ultra-550b-a55b`) bisa
  dipilih lewat `NVIDIA_MODEL` di `secrets.h` — cek ID persis di katalog
  build.nvidia.com karena bisa berubah (model lama bisa di-retire NVIDIA
  sewaktu-waktu).
- Request menyertakan `"chat_template_kwargs": {"thinking": false}` — Nemotron
  3.x adalah model reasoning yang defaultnya menaruh jawaban di field
  `reasoning_content` (bukan `content`) dan bisa menghabiskan `max_tokens`
  hanya untuk proses "berpikir" sampai `content` kosong. Flag ini mematikan
  reasoning supaya langsung jawab singkat dan `content` terisi.
- **Panjang jawaban beda per jalur**: pertanyaan dari tombol+Serial (OLED
  kecil) dapat instruksi "maksimal 3 kalimat" + `max_tokens: 300`.
  Pertanyaan dari HP (HA/web, tidak ada batas layar) dapat instruksi
  "boleh lengkap" + `max_tokens: 800` — perlu timeout HTTP lebih longgar
  (45 detik, vs 15 detik utk mode singkat) karena jawaban panjang makan
  waktu generate lebih lama (~24 token/detik diamati dari Nemotron
  Lightning). Modelnya juga diminta tidak pakai markdown (supaya tidak
  tampil simbol `**`/`#` mentah di notifikasi), tapi tidak selalu patuh —
  jadi ada pembersih markdown sederhana (`stripMarkdown()`) di firmware
  sebagai jaring pengaman.
- **Deteksi tombol pakai hardware interrupt** (`attachInterrupt`), bukan
  cuma polling `digitalRead()` biasa di `loop()`. Sebelumnya ada bug: saat
  standby, firmware polling Firebase tiap ~4 detik lewat panggilan HTTPS
  yang *blocking* — kalau tombol ditekan-lepas seluruhnya persis di
  jendela waktu itu, `loop()` tidak sempat mengeceknya sama sekali dan
  tekanannya hilang (device terasa "tidak merespons", tapi sebenarnya cuma
  lagi sibuk network call). Interrupt menangkap tepi tekanan tombol
  seketika kapan pun terjadi, lepas dari apa yang sedang dikerjakan
  `loop()`, jadi tekanan tombol tidak pernah hilang lagi.
- **Navigasi menu Pengaturan awalnya pakai tekan-tahan ASK**, tapi diganti
  ke klik-ganda (2x klik ≤1 detik) karena tekan-tahan terasa kurang nyaman
  dipakai. Deteksi klik-ganda-nya pakai **polling + debounce biasa** di
  `loop()` (pola sama persis seperti UP/DOWN), BUKAN baca timestamp di
  dalam interrupt — percobaan awal pakai interrupt `CHANGE` (baca/
  klasifikasi tepi tekan-lepas langsung di ISR) ternyata rentan meleset
  kalau dua transisi kejadian berdekatan, yang justru PALING SERING
  terjadi tepat saat klik-ganda asli (apalagi gesture double-click mouse
  yang sangat cepat, jauh lebih cepat & "berisik" sinyalnya drpd simulasi
  tekan-lepas yang diberi jeda manual). Interrupt falling-only tetap
  dipakai (pola sama kayak UP/DOWN), tapi cuma sbg jaring pengaman "jangan
  sampai kelewat kalau `loop()` lagi blocking" — bukan sumber timestamp.
  Konsekuensinya: aksi klik-tunggal (tanya AI / kembali) baru diproses ~1
  detik setelah dilepas (menunggu memastikan tidak ada klik kedua
  menyusul) -- ini standar utk deteksi klik-ganda, bukan bug.
- Pin mapping XIAO ESP32-S3 (lihat `diagram.json` + komentar di
  `src/main.cpp`): I2C OLED di D4/GPIO5 (SDA) & D5/GPIO6 (SCL), tombol ASK
  di D0/GPIO1, UP di D1/GPIO2, DOWN di D3/GPIO4, LED status di D8/GPIO7.
- `board-xiao-esp32-s3` masih berstatus **"undocumented"/eksperimental**
  di Wokwi (lint selalu menampilkan info ini — normal, bukan error).
  Selama sesi pengembangan ini juga sempat kehabisan kuota bulanan gratis
  `wokwi-cli` (CI testing) karena banyaknya simulasi yang dijalankan untuk
  verifikasi — kalau kamu menjalankan `wokwi-cli` sendiri dan dapat error
  soal kuota, itu normal untuk akun free tier, bukan tanda ada yang rusak.

## Lanjut ke hardware fisik (sesuai desain asli)

Kalau nanti pindah ke board XIAO ESP32-S3 + OLED + mic + MAX98357 + speaker
yang asli, ada dua jalur:

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
