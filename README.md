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
- **WiFi**: `STA_SSID`/`STA_PASS` (your home network — optional), and `AP_SSID`/`AP_PASS`
  (the fallback access point), channel.
- **SD Card (SPI)**: the four GPIOs and the SPI clock (`SD_SPI_FREQ_KHZ`, default 20 MHz —
  try 40 MHz on short/clean wiring for more headroom).
- **DLNA / UPnP**: friendly name, a stable device UUID.
- **Media streaming**: HTTP transfer method — **Chunked (keep-alive)** (default; usually
  higher sustained throughput) or **Content-Length (raw socket)** (cleaner seeking). Override
  per request for A/B testing with `?tx=chunked` or `?tx=clen`, e.g.
  `http://<ip>/media?path=/movie.mp4&tx=chunked`.

## Two WiFi modes (station or access point)

At boot the box picks one:

- **Station** — if `STA_SSID` is set, it tries to join that home network (~15 s). On success
  it lives on your LAN and the router gives it an IP; find it at **`c3-media.local`** or its
  DHCP address. Everything (streaming, DLNA discovery, WebDAV) works across the LAN, and
  because the network has real internet, **clients stay connected** (no captive-drop). This is
  the mode to use at home.
- **Access point** — if `STA_SSID` is blank, or the home network isn't reachable, it hosts its
  own WiFi (`AP_SSID`, default `192.168.4.1`) for fully standalone use. On this no-internet AP,
  phones may drop off after a minute — see "Staying connected" below.

A station that later loses the network just auto-reconnects; the mode is chosen once per boot.

## Load media

Two ways to get files onto the card:

- **Pull the card:** copy H.264 files on from a computer (FAT32), optionally in folders — the
  server browses the tree. Then insert the card and power the C3.
- **Over WiFi via WebDAV** (see below) — add/delete/rename files on the running box, no card
  removal.

## WebDAV (manage files over WiFi)

A read-write WebDAV share is exposed at **`http://192.168.4.1/dav`** (no login). Join the C3's
AP, then map it:

- **Android:** Solid Explorer / CX File Explorer / Material Files → add a WebDAV/network
  location → host `192.168.4.1`, path `/dav`, port `80`, no username/password.
- **macOS:** Finder → *Go → Connect to Server* → `http://192.168.4.1/dav`.
- **Browser:** open `http://192.168.4.1/dav/` for a plain clickable listing.

You can upload, download, make folders, delete, and rename. Uploads show up in DLNA browsing
immediately (the folder's index is refreshed on write).

> Windows *Map network drive* is not yet supported (it needs WebDAV class-2 `LOCK`, a later
> addition). Use a phone app or Finder for now.

### File index

To avoid re-scanning the SD on every browse, the first listing of a folder writes a small
`index.xml` under **`/sdcard/.info/`** (mirroring the tree) and later listings read that.
It's rebuilt automatically when you change a folder over WebDAV, and the whole `.info` tree is
**cleared on every boot** so a card edited externally (pulled and changed on a computer)
re-indexes cleanly after a power-cycle. The `.info` folder, `System Volume Information`, and
`$RECYCLE.BIN` are hidden from listings.

## Staying connected (no internet on the AP)

Because the AP has no internet, phones drop it after ~a minute and fall back to cellular —
which interrupts loading/streaming. There's no reliable firmware trick for this (modern
devices validate over cert-pinned HTTPS, which can't be faked — the same wall that blocks
Roku). The dependable fix is on the client: **remove its cellular fallback while streaming.**

- **Phone (best):** turn on **Airplane mode, then re-enable WiFi** and join `c3-media`. With
  no cellular to switch to, the phone stays on the no-internet AP. (Or just toggle **Mobile
  data off**.)
- Or tap **"stay connected / always connect"** on the "Wi-Fi has no internet access" prompt.
- Laptops generally stay connected without any of this.

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

- **Windows WebDAV write** — advertise `DAV: 1, 2` and fake `LOCK`/`UNLOCK` so Windows
  *Map network drive* can upload.
- **Bigger stream buffers** — the index lives on the SD, not RAM, so freed heap can grow the
  `media_stream` read buffer for smoother playback.

## Layout

```
main/
  app_main.c        boot: nvs -> wifi -> sdcard -> mdns -> http -> ssdp
  wifi.c            station-with-AP-fallback bring-up
  sdcard.c          SDSPI FATFS mount
  http_server.c     shared esp_http_server + route registration
  media_stream.c    GET /media with HTTP Range (media_send_file shared with WebDAV)
  content_dir.c     path/MIME/objectID helpers
  content_index.c   lazy per-folder SD index (.info/*/index.xml)
  webdav.c          read-write WebDAV at /dav
  dlna_ssdp.c       SSDP discovery responder + announcer
  dlna_upnp.c       device/SCPD XML + SOAP Browse
  dlna_didl.c       DIDL-Lite generation + DLNA.ORG flags
  templates.h       static UPnP XML
```

Protocol formats (SSDP templates, device/SCPD XML, DIDL-Lite) follow
[`pschatzmann/arduino-dlna-server`](https://github.com/pschatzmann/arduino-dlna-server) as a
reference; the implementation here is native ESP-IDF C.
