# Tentacle Monster Roleplay ESP32

Camera-assisted roleplay tooling for Windows, iPhone Safari, and an ESP32-S3
vibration bridge.

The project has three small parts:

- A local HTTPS camera bridge. An iPhone opens a web page, selects front/rear
  camera, and uploads frames to the Windows project folder.
- A TCP-to-serial PowerShell bridge for an ESP32-S3 controller.
- Small command-line helpers for bounded roleplay feedback patterns.

This repository is source-available for non-commercial use only. See
[LICENSE](LICENSE).

## What This Is For

The intended workflow is a roleplay game loop:

1. A phone camera provides the current real-world scene.
2. An AI/game master reads the latest frame and interprets visible props,
   posture, notes, screens, or room state as game context.
3. The AI/game master advances a fictional scene.
4. Optional ESP32-S3 feedback commands provide physical effects.

The runtime frame files are deliberately ignored by git. Do not commit private
camera captures.

## Requirements

- Windows 10 or later
- Node.js
- PowerShell
- OpenSSL available as `openssl.exe`
- iPhone Safari on the same local network
- Optional: ESP32-S3 serial bridge hardware for feedback control

No npm dependencies are required.

## Files

- `server.js` - HTTPS camera bridge on port `7777`.
- `public/index.html` - iPhone camera control page.
- `start.bat` - generate local certificate if needed and start the bridge.
- `stop.bat` - stop the camera bridge.
- `status.bat` - show bridge status, connected phone, and latest frame metadata.
- `esp32-bridge.ps1` - TCP-to-serial bridge for ESP32-S3 commands.
- `vibration-control.py` - CLI client for the bridge.
- `random-wave.py` - bounded random feedback pattern runner.
- `stop-random-wave.bat` - request random feedback stop and send `STOP`.
- `剧本.txt` - example roleplay scenario.

Generated runtime files:

- `latest.jpg` - latest uploaded camera frame.
- `latest.json` - metadata for the latest uploaded frame.
- `server.log`, `server.err.log`, `server.pid` - local runtime files.
- `certs/*.pem`, `certs/openssl.generated.cnf` - local HTTPS certificate files.

These generated files are excluded by `.gitignore`.

## Start The Camera Bridge

Run:

```bat
start.bat
```

The script will:

1. Generate a self-signed local HTTPS certificate if `certs/key.pem` and
   `certs/cert.pem` do not exist.
2. Start `node server.js` in the background.
3. Print local network URLs such as:

```text
https://YOUR-LAN-IP:7777/
```

Open that HTTPS URL on iPhone Safari. Because the certificate is self-signed,
Safari may show a warning. Continue to the site for local use.

## iPhone Camera Page

On the iPhone page:

1. Choose rear or front camera.
2. Choose the capture interval.
3. Tap `Start`.
4. Allow camera access.

The page uploads JPEG frames to the local Node.js server. The server overwrites
`latest.jpg` and `latest.json` with each new frame.

## Stop The Camera Bridge

Run:

```bat
stop.bat
```

It stops the PID recorded in `server.pid` and also falls back to stopping a Node
process listening on port `7777`.

## Check Status

Run:

```bat
status.bat
```

It reports:

- Whether port `7777` is listening.
- Whether a phone client is connected.
- Latest frame metadata from `latest.json`.
- Whether `latest.jpg` exists and when it was updated.

## ESP32-S3 Serial Bridge

Start the PowerShell bridge in a separate terminal:

```powershell
powershell -ExecutionPolicy Bypass -File .\esp32-bridge.ps1 -SerialPort COM3
```

Options:

```powershell
-SerialPort COM3
-Baud 115200
-ListenAddress 127.0.0.1
-ListenPort 25363
```

The bridge accepts short TCP commands and forwards them to the serial device:

- `PING`
- `STATUS`
- `SCAN`
- `SERVICES`
- `SET <0-100>`
- `HIT <damage>`
- `STOP`

## Vibration CLI

Use:

```powershell
py .\vibration-control.py --ping
py .\vibration-control.py --status
py .\vibration-control.py --set 20
py .\vibration-control.py --hit 3
py .\vibration-control.py --stop
```

`--set` directly sets an intensity level. `--hit` sends a bounded event-style
command and lets the device firmware map it to feedback behavior.

## Random Wave

Run a bounded random pattern:

```powershell
py .\random-wave.py --duration 60 --min 5 --max 35
```

Stop early:

```bat
stop-random-wave.bat
```

The random runner always sends `STOP` when it exits.

## AI Image Workflow

There are two supported image workflows.

### 1. Direct Uploaded Images

In a chat UI that supports image input, send an image directly. It may appear as
something like:

```text
[Image #1]
```

The AI can inspect that image and respond with description, translation,
scene interpretation, or game state updates.

### 2. Local Camera Frame

When the iPhone camera page is running, the AI/game master can inspect:

```text
latest.jpg
```

Example prompt:

```text
Observe the scene from latest.jpg and continue the roleplay.
```

The AI should treat visible objects as authoritative and avoid inventing objects
that are not visible.

## Roleplay Safety And Runtime Notes

- This project controls local hardware. Keep feedback bounded by duration and
  maximum intensity.
- Keep a stop command available.
- Do not commit generated camera frames, logs, certificates, or local metadata.
- Review scripts before running them with hardware attached.

