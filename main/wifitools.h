#pragma once

// Passive Wi-Fi tools (monitor mode, listen only).

// Deauth detector: counts deauthentication/disassociation frames in the air and shows them live.
// Teaches how a deauth attack looks, from the defender's side. Does not transmit anything.
void wifitools_deauth_detector(void);

// Channel analyzer: scans the 2.4 GHz band and shows it as a spectrum (one hump per
// network), counts the networks per channel and recommends the least congested one.
void wifitools_channel_analyzer(void);

// Touch screen to scan, pick a network, type the password and connect.
void wifitools_connect_screen(void);
