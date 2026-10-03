# Tesserae serial setup protocol

Status: protocol 1, firmware 1.43.0 and later.

A board running Tesserae firmware can be given its Wi-Fi and its server over
the same USB cable it was flashed through, so the web flasher at
tesserae.ink/flash and the Tesserae Cloud setup page can finish the job
without the Wi-Fi hotspot. The hotspot (captive portal) and the Companion BLE
setup remain; this is a third way in, for a board that is plugged into a
computer with a browser that has Web Serial (Chrome or Edge on a desktop).

The channel is the board's console port at 115200 baud: UART0 through the
CH340 bridge on the reTerminals and the classic ESP32 boards, the native
USB-Serial-JTAG port on the XIAO ePaper boards, the C3 and the PaperS3. On a
board whose console is UART0, a host attached to the native USB port instead
hears nothing; the flasher then says so and points at the hotspot.

## Framing

Newline-delimited JSON, one object per line, both directions. The console is
shared with the log, so a host keeps only lines that begin with `{` and carry
`"tesserae":"setup"`, and ignores every other line. The board ignores lines
that are not such objects. Values are strings unless stated; the board reads
a flat object and skips nested values.

## Messages from the board

On boot the board announces itself, and it answers `get` with the same shape:

```json
{"tesserae":"setup","event":"hello","ok":true,"protocol":1,"fw":"1.43.0",
 "kind":"seeed_reterminal_e1001","mac":"aa:bb:cc:dd:ee:ff","mode":"local",
 "ssid":"home","server_url":"http://tesserae.local:8765","relay_url":"",
 "paired":false,"portal":true,
 "networks":[{"ssid":"home","rssi":-51,"secure":true}]}
```

- `event` is `hello` at boot and `info` in answer to `get`.
- `mode` is `local`, `cloud` or `relay`: what the stored settings amount to.
- `paired` says whether the board holds a server token or a relay pairing.
- `portal` says whether the Wi-Fi hotspot setup page is open right now.
- `networks` is the hotspot's scan of nearby networks, or `[]` when the board
  did not scan (it scans only when the hotspot opens).
- The Wi-Fi password, the server token and any pairing code never leave the
  board.

An error, for any command:

```json
{"tesserae":"setup","ok":false,"error":"A claim code from Tesserae Cloud is required. ..."}
```

## Commands from the host

`{"tesserae":"setup","cmd":"get"}` asks for the info line above.

`{"tesserae":"setup","cmd":"set", ...}` saves settings and restarts the board.
The fields are those of the hotspot form and follow the same rules
(src/setup_fields.c):

| field | modes | notes |
| --- | --- | --- |
| `ssid` | all | required |
| `pass` | all | omitted or empty keeps the stored password |
| `mode` | all | `local` (default when omitted), `cloud` or `relay` |
| `server_url` | local | required; `http://` is assumed on a bare host |
| `pairing_code` | local, cloud | optional for a local server; required for the cloud (the claim code) |
| `relay_url` | relay | optional; defaults to `https://relay.tesserae.ink`; must be https |
| `relay_code` | relay | required |

The cloud's address is fixed by the firmware (`SETUP_CLOUD_URL`); a
`server_url` sent with `mode: "cloud"` is ignored. The answer, before the
restart:

```json
{"tesserae":"setup","ok":true,"saved":true,"mode":"cloud","restarting":true}
```

A `set` while the hotspot page is open ends the hotspot the same way a form
post does. A `set` on a board that is awake and working restarts it at once;
the request it was in the middle of is abandoned, which is harmless.

`{"tesserae":"setup","cmd":"restart"}` restarts the board.

## A host's session, after a flash

1. The flasher resets the board into the new firmware and closes its port.
2. It reopens the same port at 115200 baud. A board on native USB disappears
   and comes back during the reset; the host waits for it and, when it needs
   to, asks for the port again.
3. It sends `get` every second or two until an info or hello line arrives,
   for up to about twenty seconds. The hello line may already have gone by.
4. It shows the Wi-Fi form, with `networks` as suggestions when there are any.
5. It sends `set` and waits for `saved`. The board restarts, joins the Wi-Fi
   and pairs; the cloud setup page sees the panel appear through its own poll.
