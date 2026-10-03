/*
 * Setup over the console port (USB): the web flasher and the cloud console
 * configure a board right after flashing it, or any time it is plugged in,
 * without the Wi-Fi hotspot. Newline-delimited JSON both ways, documented in
 * docs/serial-setup-protocol.md. The same fields and rules as the captive
 * portal (setup_fields.h), saved the same way (provisioning_apply()).
 *
 * Runs as a small task for as long as the board is awake; a save restarts the
 * board, or ends the portal when that is what is open.
 */
#pragma once

void serial_setup_start(void);
