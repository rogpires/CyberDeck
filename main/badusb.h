#pragma once

// Bad USB: the deck acts as a USB keyboard and "types" a DuckyScript into the computer it is plugged into.
// A tool for automation and *authorized* testing only. It never types on its own: it lists scripts from
// /sd/badusb, shows the chosen one, and injects only after an explicit confirmation on the screen.
//
// Turning on USB HID replaces the USB serial console until the next reboot (the port becomes a keyboard).

// Creates /sd/badusb with an example script if it is missing
void badusb_init(void);

// Full-screen flow: pick a script, preview it, confirm, inject. Driven by touch or keyboard.
void badusb_browse(void);
