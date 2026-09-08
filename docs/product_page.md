# HexHound

## Cyber recon pet. USB dongle. Actually scans.

Your next security demo isn't a PowerPoint. It's alive, it fits on a keychain, and it just found three open access points in your client's lobby.

---

## What It Does

- **Scans your environment** for rogue APs, duplicate SSIDs, and BLE tracker candidates - passively, continuously, in your pocket
- **Evolves as it works** - five stages from Egg to Sentinel, each unlocking new recon capabilities
- **Drops USB HID missions** on any host machine - security tips, WiFi audit reports, lock-screen reminders, or a quiet note that says "your physical security has a gap"
- **Fits on a keychain.** Runs on USB power on the T-Dongle reference build, or from USB/battery on supported Waveshare variants. No app. No cloud. No permissions dialog.

---

## The Five Stages

### Stage 1: Egg
*Incubating. Learning. Waiting.*

Your HexHound starts dormant. Interact with it - press the button, plug it in, run your first WiFi scan. It's watching. Once it's seen enough, it hatches.

### Stage 2: Packet Pup
*WiFi reconnaissance unlocked. Curious and hungry.*

The Pup scans 2.4GHz for access points. It flags open networks and duplicate SSIDs. Every patrol feeds it. It wants to learn your environment.

### Stage 3: Beacon Beast
*BLE detection online. Alert and territorial.*

Now it listens for Bluetooth Low Energy beacons too. Fitness trackers, AirTags, rogue BLE devices - the Beast catalogues everything nearby. Patrols get deeper.

### Stage 4: Gremlin Mode
*USB HID missions active. Mischievous and sharp.*

The Gremlin can type. Plug it into a host machine and execute security awareness missions - drop a WiFi audit report, lock an unattended workstation, or leave a note that makes someone rethink their desk security. Mischief stat goes up. So does awareness.

### Stage 5: Sentinel
*Full spectrum. Calm authority.*

The final form. Anomaly detection, pattern recognition across patrols, the full mission arsenal. The Sentinel has seen your network. It knows what belongs and what doesn't.

And it keeps progressing. Sentinel unlocks Mastery ranks, continued discovery tracking, and perk unlocks so the pet still has reasons to patrol after final evolution.

---

## Demo Scenario

You walk into a client's conference room for a security assessment kickoff meeting. While they're getting coffee, you plug HexHound into the USB port on the shared laptop.

It runs a patrol. Twelve WiFi networks. Two with the same SSID - one of them isn't theirs. Four BLE devices, including one that's been following the same MAC rotation pattern for the last hour.

You execute Mission 3: WiFi Audit Report. HexHound types the scan results directly into the laptop's open Notepad. When the client sits down, the audit is already on screen.

No slides. No setup. No "let me share my screen." Just the results, typed live by a device the size of a thumb drive.

That's the conversation starter a slide deck will never be.

---

## Built For

### Use Cases

- **Pentest kickoff demos** - show clients real findings in the first five minutes
- **Security awareness training** - gamified learning that people actually remember
- **CTF events and workshops** - interactive hardware challenge platform
- **Personal EDC recon** - passive environmental awareness wherever you go
- **Red team physical assessments** - HID payload delivery with plausible cover

### Works With

- **Host OS:** Windows, macOS, Linux (for USB HID missions)
- **WiFi:** Any 2.4GHz environment (passive scanning, no association)
- **BLE:** Bluetooth 5.0 LE (passive scanning)
- **Power:** Any USB-A port (bus powered, ~100mA draw)
- **Storage:** Onboard flash for persistent pet state and config, optional MicroSD for removable logs

### Supported Hardware

- **Reference hardware:** LilyGo T-Dongle S3
- **Also supported:** Waveshare ESP32-S3-LCD-1.47B, Waveshare ESP32-S3-Touch-LCD-1.47
- **Touch:** Supported on the Waveshare touch variant
- **Battery:** Supported on the Waveshare variants with onboard measurement

---

## Technical Specs

| Component | Detail |
|-----------|--------|
| Platform | ESP32-S3 family |
| Reference board | LilyGo T-Dongle S3 |
| Alternate supported boards | Waveshare ESP32-S3-LCD-1.47B, Waveshare ESP32-S3-Touch-LCD-1.47 |
| Display | 80x160 ST7735 on T-Dongle, 172x320 ST7789 on Waveshare |
| LED | APA102 RGB on T-Dongle reference hardware |
| WiFi | 2.4GHz 802.11 b/g/n (passive scan) |
| Bluetooth | BLE 5.0 (NimBLE stack, passive scan) |
| USB | Native USB, HID keyboard emulation |
| Storage | Onboard flash persistence plus optional MicroSD on supported boards |
| Input | Button on all supported boards, plus capacitive touch on the Waveshare touch model |
| Power | USB bus powered on T-Dongle, battery-capable on supported Waveshare boards |
| Dimensions | Vary by board |
| Weight | Varies by board |

---

## Open Source

HexHound is open source under the MIT License. The firmware, sprites, documentation, and hardware reference are all on GitHub.

We accept contributions: new missions, sprite art, hardware variant reports, and bug fixes. If you get HexHound running on a different ESP32-S3 board, we want to hear about it.

**GitHub:** the HexHound repository

---

## Get One / Build One

### Build It Yourself

Total cost: **~$20**

| Part | Cost |
|------|------|
| LilyGo T-Dongle S3 | ~$15 |
| Waveshare ESP32-S3 1.47-inch boards | Varies by seller |
| MicroSD card (8GB+, optional) | ~$5 |

Clone the repo. Flash with PlatformIO. An SD card is optional. Done.

Full build instructions and bill of materials on GitHub.

### Pre-Configured Units

For organizations running security awareness programs, HexHound units can be pre-configured with custom mission payloads, branded splash screens, and bulk pricing.

**Contact:** open a GitHub Issue.

---

*Built by the HexHound authors*
