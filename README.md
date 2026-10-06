# RKW502A200 ESPHome IR controller

ESPHome IR climate component for the Mitsubishi Heavy Industries **RKW502A200** remote, tested with the **SRK71ZK-S** air conditioner. The implementation is specific to the observed MHI ZJS 88-bit protocol; it is not a generic Mitsubishi or universal-AC integration.

The remote model is the primary project identifier because this component implements its IR protocol. The verified air-conditioner model is listed separately because protocol compatibility was tested on that unit.

## What it provides

- Home Assistant climate control over ESPHome's native API.
- IR receive/decode of the original remote's full-state frames.
- Horizontal and vertical vane selects using the labels observed on the remote's LCD.
- Persistence of the last learned/selected vane positions across reboots.
- SHTCx temperature and humidity sensor configuration.

Horizontal raw-code mapping, in select order: `Left Left=3`, `Left Centre=4`, `Centre Centre=5`, `Centre Right=6`, `Right Right=7`, `Left Right=8`, `Right Left=9`, `Swing=2`, `Auto=0`. This mapping was corrected against a live remote press; the initial capture marker was an Auto frame, not the first horizontal position.

## Hardware and pins

The checked-in YAML targets a classic ESP32 development board and uses:

| Function | GPIO |
| --- | ---: |
| IR transmitter | 4 |
| IR receiver | 14 |
| I²C SDA | 21 |
| I²C SCL | 22 |

The sensor is configured as SHTCx. Confirm the exact breakout pinout and voltage levels before wiring: ESP32 GPIOs are not 5 V tolerant. In particular, verify the IR receiver output is safe for 3.3 V GPIO.

## Setup

1. Install ESPHome (the ESPHome Device Builder or ESPHome CLI).
2. Copy `secrets.yaml.example` to `secrets.yaml` and set your Wi-Fi SSID/password. Generate a unique 32-byte base64 API key, for example:

   ```powershell
   python -c "import secrets,base64; print(base64.b64encode(secrets.token_bytes(32)).decode())"
   ```

   Put that value in `api_encryption_key`. Never commit `secrets.yaml`.
3. Review the Wi-Fi authentication setting in `mhi-srk71zk-s.yaml`. This device's current access point requires legacy WPA; use WPA2/WPA3 instead where supported.
4. Compile and flash over USB for first setup, then use the ESPHome OTA/API path:

   ```powershell
   esphome run mhi-srk71zk-s.yaml
   ```

   If mDNS does not resolve on your network, specify the serial port or device IP with `--device`.
5. Add the device to Home Assistant through the ESPHome integration and provide the same API encryption key.

## Validation status

The protocol bytes and vane values were captured from the RKW502A200 remote. The horizontal offset was corrected and the corrected firmware compiled, OTA-flashed, and reported `Left Left` for the persisted raw `h=3` frame after reboot. Vane persistence was also verified across an OTA reboot. Final in-room IR range and physical louver response still need validation at the intended placement.
