# Tesserae BLE setup protocol

Status: development contract, protocol major 2.

BLE is a bounded local setup and maintenance channel. It does not replace the
REST transport used for normal display operation. On supported Seeed displays
with a Refresh button, a brand-new or factory-reset display opens its captive
portal first and advertises only after a physical maintenance-mode action. A
registered display whose Wi-Fi was cleared from Companion instead returns to
Bluetooth recovery first. Other board families retain captive-portal setup and
do not link the NimBLE setup stack until their physical controls are designed
and validated separately.

## Physical authorization and lifetime

- A brand-new or factory-reset supported Seeed display opens its captive portal
  AP first. The portal is immediately usable at `192.168.4.1` and offers
  Bluetooth as an explicit Companion-app alternative.
- Clearing Wi-Fi from authenticated Companion maintenance preserves the server
  registration and reopens Bluetooth recovery automatically. A BLE timeout or
  failed/cancelled recovery falls back to the captive portal.
- Holding Refresh for three seconds stops the AP and opens BLE setup for five
  minutes. A BLE timeout or failed/cancelled session returns to the AP.
- On an onboarded display, releasing Refresh after 3-19 seconds opens
  maintenance mode and holding for 20 seconds keeps the existing factory-reset
  behavior.
- Advertising stops after timeout, successful setup, or reboot.
- The QR setup secret and numeric passkey are generated for one boot and are
  never persisted.
- Closing Companion does not consume the QR code. The same displayed code can
  authenticate another connection until the bounded maintenance session ends.

## Discovery

Primary service UUID: `7a5e0001-7b6d-4f8b-9c2e-1d0a5a110001`.

CoreBluetooth exposes ten bytes of service data after removing the service
UUID:

1. protocol major (`2`)
2. state flags (`bit 0 = setup required`, `bit 1 = maintenance`)
3. stable hardware code from the registry below
4. the final three bytes of the Wi-Fi station MAC
5. four-byte ephemeral setup session ID

Hardware code registry:

| Code | Hardware catalog kind | Status |
| ---: | --- | --- |
| `1` | `seeed_reterminal_e1001` | Enabled |
| `2` | `seeed_reterminal_e1002` | Enabled |
| `3` | `seeed_reterminal_e1003` | Enabled |
| `4` | `seeed_reterminal_e1004` | Enabled |
| `5` | `seeed_ee02` | Enabled |
| `6` | `seeed_ee04_75` | Enabled |
| `7` | `seeed_ee04_73e6` | Enabled |
| `8` | `seeed_xiao_75` | Reserved; BLE not enabled |
| `9` | `waveshare_133e6` | Reserved; BLE not enabled |
| `10` | `waveshare_photopainter_73` | Reserved; BLE not enabled |
| `11` | `seeed_ee03` | Enabled |

The advertisement does not expose SSID, server URL, room name, password, full
MAC address, or device token.

## GATT characteristics

All characteristics share the service UUID prefix above:

| Suffix | Name | Access |
| --- | --- | --- |
| `0002` | Device info | public read |
| `0003` | QR control | public write; accepts authenticated AES-GCM frames only |
| `0004` | Passkey control | authenticated-encrypted write |
| `0005` | Events | read + notify |

Device info is a compact JSON object containing protocol version, setup session
identifier, a fresh 16-byte connection nonce, model, firmware version, device
identifier, and mode. It contains no stored credential. The connection nonce
changes after every BLE reconnect.

## QR setup

The display renders both a QR code and a six-digit fallback passkey. The QR
payload is:

```text
tesserae://setup?v=2&id=<device-id>&sid=<8-hex>&key=<base64url-32-byte-secret>
```

The app selects the BLE peripheral whose device-info `id` and `sid` match the
QR. For each BLE connection, both sides derive an AES-256-GCM key using
HKDF-SHA256 with the QR secret as input key material, `sid || connection_nonce`
as salt, and `tesserae-ble-connection-key-v2` as info. The 12-byte GCM nonce is
`sid (4 bytes) || direction (1 byte) || zero (3 bytes) || counter (4 bytes)`.
Counters are strictly increasing per direction and reset with each derived
connection key. The frame header and counter are authenticated as AAD. A frame
captured from an earlier connection therefore cannot be replayed after a
reconnect, while the same on-screen QR remains usable for the whole session.

The numeric fallback uses LE Secure Connections with MITM protection and a
display-only passkey. iOS owns the system passkey prompt; the app never stores
the passkey.

## Message transport

Messages are UTF-8 JSON and may be split into multiple GATT writes or
notifications. Every authenticated frame includes a message id, zero-based
chunk index, chunk count, and payload. Receivers reject duplicate counters,
out-of-order chunks, oversized messages, and more than one partial message.

App operations:

- `scan`: return nearby SSIDs one event at a time, followed by `scan_complete`.
- `stage`: stage SSID, password, server URL, and one-time firmware pairing code
  in RAM.
- `apply`: connect with staged Wi-Fi, verify the Tesserae Companion capability
  endpoint, then persist configuration and reboot.
- `diagnostics`: return firmware, current SSID, IP, RSSI, heap, and recent
  maintenance events. Never return passwords or device tokens.
- `reboot`: reboot without changing configuration.
- `clear_wifi`: erase Wi-Fi credentials and reboot into maintenance recovery
  when an existing server registration is preserved.
- `factory_reset`: erase NVS and reboot into setup.

Configuration is not written until both Wi-Fi association and Tesserae server
verification succeed. A failed repair therefore leaves the previously working
configuration intact.

## Server registration

An authenticated Companion requests a single-use code from
`POST /api/app/v1/device-pairings` (`device_setup` capability and
`device_setup:write` scope). The app sends that code to the display inside the
secured BLE channel. After reboot, firmware redeems it through the existing
`POST /api/v1/device/register` endpoint. The app never receives the resulting
firmware device token.
