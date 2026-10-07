# Prebuilt bundle

`partid-1.0.0-system-update.irisfw` is the whole application, built from the
source in this repository. Flash it and skip the toolchain entirely.

From a Vibe workspace root, with the board on USB

```powershell
.\m.ps1 iris system-update --bundle path\to\prebuilt\partid-1.0.0-system-update.irisfw
```

It writes the application and the UI resources, then validates both. Vibe Mode
lives in the `factory` partition and is left alone, so you keep a way back.

Then set a network and a key. See the main README under
[Give it a network and a key](../README.md#give-it-a-network-and-a-key).

## Verify it first

```powershell
Get-FileHash .\partid-1.0.0-system-update.irisfw -Algorithm SHA256
```

```
2c4287dd967d089fd4c896959e3a06761cece7be9c362822ffda49a1734bcc13
```

## Why there are no raw bin files here

High Speed USB on this board belongs to ESP-Iris. Writing the flash directly
with esptool fights it, so the supported route is the bundle above. Nothing is
lost by it. The bundle carries the partition table, the application and the UI
resources together, and it is written atomically.

## What is inside

Built at version 1.0.0 against ESP-IDF master for ESP32-S31. The application
image is 2133616 bytes and the UI resource image is 183312 bytes.

No credentials are in this bundle. None are compiled in anywhere. The wifi
password and the API key go in at runtime over ESP-Iris RPC or through the
setup portal.
