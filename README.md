# C3 Media Server

A standalone **WiFi access point + DLNA media server** for the **ESP32-C3**, built
with **ESP-IDF**. It streams **H.264 video straight off an SD card** to a **Roku Media
Player** with little to no buffering — the C3 hosts its own WiFi network, the Roku joins
it, discovers the server over DLNA/SSDP, and direct-plays files.

No transcoding happens on the device: files must already be in a Roku-native format
(**H.264 in MP4/MKV/MOV**, with AAC/MP3/AC3 audio). MP4 with the moov atom at the front
("faststart") plays and seeks best.

## How it works

| Piece | What it does |
|-------|--------------|
| **SoftAP** (`wifi_ap.c`) | Hosts a WiFi network; its DHCP server puts the Roku on the C3's subnet (C3 = `192.168.4.1`). No router needed. |
| **SD card** (`sdcard.c`) | Mounts the FAT card over SPI at `/sdcard` (the C3 has no SDMMC host). |
| **HTTP streamer** (`media_stream.c`) | `GET /media?path=...` streams a file with **HTTP Range** support (`206 Partial Content`) so Roku can seek and reconnect. |
| **SSDP** (`dlna_ssdp.c`) | Answers `M-SEARCH` discovery and multicasts `ssdp:alive` so control points list the server. |
| **UPnP** (`dlna_upnp.c`, `dlna_didl.c`) | Serves the device description + SCPDs and handles the ContentDirectory `Browse` SOAP action, returning DIDL-Lite that points `<res>` URLs back at the streamer. |

## Hardware

An ESP32-C3 board and a microSD breakout wired over SPI. Default GPIOs (change in
`menuconfig`):

| Signal | Default GPIO |
|--------|--------------|
| MOSI (DI)  | 6 |
| MISO (DO)  | 5 |
| CLK (SCK)  | 4 |
| CS         | 7 |

Defaults avoid the C3 strapping pins (2, 8, 9) and USB pins (18, 19). Add pull-ups per your
SD breakout, and keep the wiring short if you raise the SPI clock.

## Build & flash

Requires **ESP-IDF ≥ 5.1**. In VS Code use the Espressif IDF extension, or on the CLI:

```bash
idf.py set-target esp32c3
idf.py menuconfig        # C3 Media Server Configuration -> set AP SSID/password, SD pins
idf.py -p <PORT> flash monitor
```

Key `menuconfig` items under **C3 Media Server Configuration**:
- **WiFi Access Point**: `AP_SSID`, `AP_PASS` (≥8 chars, or blank for open), channel.
- **SD Card (SPI)**: the four GPIOs and the SPI clock (`SD_SPI_FREQ_KHZ`, default 20 MHz —
  try 40 MHz on short/clean wiring for more headroom).
- **DLNA / UPnP**: friendly name shown to Roku, a stable device UUID.

## Load media (manual for now)

Put your H.264 files on the SD card from a computer (FAT32), optionally in folders — the
server browses the directory tree. Then insert the card and power the C3.

> WebDAV upload over the network is planned but not yet implemented; load the card by hand
> until then.

## Use it from Roku

1. On the Roku: **Settings → Network → Wireless** and join the C3's AP (your `AP_SSID`).
   The Roku will have no internet on this network — it's a dedicated media link.
2. Open the **Roku Media Player** channel → **Media Servers**. The friendly name should
   appear.
3. Browse into your folders and play a file. Seeking uses HTTP Range under the hood.

Quick sanity check from a laptop on the same AP: open
`http://192.168.4.1/media?path=/YourMovie.mp4` in VLC ("Open Network Stream") or a browser.

## Bitrate ceiling (important)

The ESP32-C3 is single-core with a realistic **~15–20 Mbit/s WiFi TCP ceiling**, and the SD
card runs over SPI. The effective streaming ceiling is roughly the lower of the two. In
practice:

- **≤ ~8 Mbit/s H.264** (720p / moderate 1080p): plays smoothly.
- **High-bitrate 1080p (15–25+ Mbit/s)**: will stutter — re-encode/remux to a lower
  bitrate.

Since the device can't transcode, match your source files to this budget ahead of time
(e.g. `ffmpeg -i in.mkv -c:v libx264 -b:v 6M -movflags +faststart -c:a aac out.mp4`).

## Roadmap

- **WebDAV** on the same HTTP server (`PROPFIND`/`PUT`/`DELETE`/...) so you can manage the
  SD card over the network instead of pulling the card.

## Layout

```
main/
  app_main.c        boot: nvs -> wifi_ap -> sdcard -> mdns -> http -> ssdp
  wifi_ap.c         SoftAP + DHCP
  sdcard.c          SDSPI FATFS mount
  http_server.c     shared esp_http_server + route registration
  media_stream.c    GET /media with HTTP Range
  content_dir.c     path/MIME/objectID helpers
  dlna_ssdp.c       SSDP discovery responder + announcer
  dlna_upnp.c       device/SCPD XML + SOAP Browse
  dlna_didl.c       DIDL-Lite generation + DLNA.ORG flags
  templates.h       static UPnP XML
```

Protocol formats (SSDP templates, device/SCPD XML, DIDL-Lite) follow
[`pschatzmann/arduino-dlna-server`](https://github.com/pschatzmann/arduino-dlna-server) as a
reference; the implementation here is native ESP-IDF C.
