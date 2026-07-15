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
  higher sustained throughput) or **Content-Length (raw socket)** (cleaner seeking).

### Switching the transfer method (A/B testing)

- **Live, applies to DLNA too:** open **`http://<ip>/tx?m=chunked`** or **`?m=clen`** in a
  browser to flip the method for all subsequent playback (DLNA browse, WebDAV, direct URL) —
  no rebuild. `http://<ip>/tx` reports the current method; `?m=default` returns to the
  menuconfig choice. This is the way to A/B when playing via VLC's UPnP browser, since the
  DLNA `<res>` URL is fixed and can't take a query flag from the GUI.
- **Per-request (typed URLs only):** append `?tx=chunked` / `?tx=clen`, e.g.
  `http://<ip>/media?path=/movie.mp4&tx=clen`.

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

## Watch it — the built-in web player (recommended)

Open **`http://<ip>/`** in any phone/laptop browser (`http://192.168.4.1/` in AP mode, or
`http://c3-media.local/` in station mode). You get a simple file browser: tap a folder to
navigate, tap a video to play it full-screen in the browser's HTML5 player. Playback starts
in ~2 seconds.

This is the fastest way to watch, and it deliberately sidesteps DLNA: DLNA control points
(VLC's UPnP browser, BubbleUPnP, etc.) "extract metadata" by pre-scanning each file before
playing, which can take minutes over WiFi. The web player just streams via `/media` and
starts immediately. (DLNA is still available — see below — it's just slower to start.)

Note: browsers play **H.264 MP4/MOV/WebM** natively; **MKV/AVI won't play in-browser** (use
MP4, or DLNA/VLC for those). Keep bitrate under your link speed (~2 Mbit/s) for smooth play.

## Settings page (edit WiFi + WebDAV login in the browser)

Open **`http://<ip>/settings`** (linked from the gear on the home page) to edit, without a
rebuild:

- **Access-point** name + password + channel (the fallback network the box hosts),
- **Home WiFi** name + password (station mode — join your router if reachable),
- **WebDAV / settings username + password** (the login below).

Values are stored in NVS and survive reboots (first boot uses the `menuconfig` defaults).
**Saving reboots the device** to apply the WiFi changes — if you changed the AP name/password,
reconnect to the new network afterwards. A bad home-WiFi password just falls back to hosting
the AP, so you can't lock yourself out of the radio.

**The login:** leave the **WebDAV password blank to disable authentication** (the default —
handy on first setup). Once you set a password, both **`/settings`** and **`/dav`** prompt for
the username/password; the media browser (`/`) and direct `/media` streaming stay open so
playback needs no login.

## WebDAV (manage files over WiFi)

A read-write WebDAV share is exposed at **`http://<ip>/dav`**. If you set a WebDAV
username/password on the settings page, use it when connecting; otherwise leave credentials
blank. Join the C3's AP (or your home WiFi in station mode), then map it:

- **Android:** Solid Explorer / CX File Explorer / Material Files → add a WebDAV/network
  location → host `<ip>`, path `/dav`, port `80`, credentials as set (or none).
- **macOS:** Finder → *Go → Connect to Server* → `http://<ip>/dav`.
- **Windows:** *Map network drive* — see below (needs one registry tweak).
- **Browser:** open `http://<ip>/dav/` for a plain clickable listing.

You can upload, download, make folders, delete, and rename. Uploads show up in DLNA browsing
immediately (the folder's index is refreshed on write). Uploads are written to a hidden
`.<name>.part` temp file and renamed into place only on full success, so a cancelled or failed
transfer **never leaves a partial file** on the card.

> While a WebDAV transfer is in progress the box can't serve a DLNA/stream request at the same
> time — the single HTTP worker handles one request at a time. Uploads and playback don't
> overlap; this is expected.

### Windows *Map network drive*

Windows only maps a WebDAV share once it sees class-2 lock support (which the server now
advertises) and, for plain HTTP, once Basic auth over HTTP is enabled:

1. Enable Basic-over-HTTP: in `regedit` set
   `HKLM\SYSTEM\CurrentControlSet\Services\WebClient\Parameters\BasicAuthLevel` to **`2`**.
   (Windows disables Basic auth on non-HTTPS by default.)
2. Optionally raise the download size cap `FileSizeLimitInBytes` (same key) from the ~50 MB
   default to e.g. `0xffffffff` if you'll pull large files.
3. Restart the **WebClient** service (`services.msc`, or `net stop webclient && net start
   webclient`) so the changes take effect.
4. *This PC → Map network drive* → Folder `http://<ip>/dav` → tick *Connect using different
   credentials* if you set a WebDAV login, and enter it.

Set a WebDAV username/password on the settings page first — Windows won't send a blank
credential over Basic. A phone app (Solid Explorer) or macOS Finder needs none of this.

### If an upload fails / the serial log floods with `media:` reads

Some file managers (notably **CX File Explorer**) generate thumbnails by reading *every* video
in the folder you have open — head + `moov` tail of each file, on repeat. On this tiny
single-worker server that burst can starve a simultaneous upload and make the copy fail, while
the serial log shows a stream of `media: 200 …`/`206 …` lines for the folder's files. It's the
same "pre-scan everything" behaviour that makes DLNA slow to start.

Fixes:
- **Turn off thumbnails** in the file manager (CX → *Settings → Display → Show thumbnails* off).
- Or **copy into a folder with no videos** (make a fresh folder and paste into it) — the app
  only scans the folder currently open.
- The firmware also carries a larger socket pool (`max_open_sockets` in `http_server.c`, backed
  by `CONFIG_LWIP_MAX_SOCKETS`) so the scan burst is less likely to reset a live upload, and
  logs every WebDAV request (`webdav: PUT/LOCK/PROPFIND …`) so a failed transfer is diagnosable
  on the serial monitor.

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

- **Bigger stream buffers** — the index lives on the SD, not RAM, so freed heap can grow the
  `media_stream` read buffer for smoother playback.
- **exFAT cards** — deferred; the IDF 5.1 image is built for FAT32.

## Layout

```
main/
  app_main.c        boot: nvs -> config -> wifi -> sdcard -> mdns -> http -> ssdp
  config.c          runtime WiFi + WebDAV settings in NVS (namespace "cfg")
  auth.c            HTTP Basic auth for /settings + /dav (blank pass = disabled)
  wifi.c            station-with-AP-fallback bring-up (creds from config)
  sdcard.c          SDSPI FATFS mount
  http_server.c     shared esp_http_server + route registration
  media_stream.c    GET /media with HTTP Range (media_send_file shared with WebDAV)
  content_dir.c     path/MIME/objectID helpers
  content_index.c   lazy per-folder SD index (.info/*/index.xml)
  webdav.c          read-write WebDAV at /dav (class 2, safe PUT)
  webui.c           web media browser/player + /settings page
  dlna_ssdp.c       SSDP discovery responder + announcer
  dlna_upnp.c       device/SCPD XML + SOAP Browse
  dlna_didl.c       DIDL-Lite generation + DLNA.ORG flags
  templates.h       static UPnP XML
```

Protocol formats (SSDP templates, device/SCPD XML, DIDL-Lite) follow
[`pschatzmann/arduino-dlna-server`](https://github.com/pschatzmann/arduino-dlna-server) as a
reference; the implementation here is native ESP-IDF C.
