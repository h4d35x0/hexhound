# Hardware Playbook

Portable, project-agnostic knowledge for turning a firmware idea into a real small ESP32 device.
Distilled from an earlier conference-badge effort that failed, plus the bench bring-ups that worked.
The point of this doc: never re-learn these the hard way. Copied verbatim into every hardware
project so it travels with the work, not buried in a dead repo.

Last distilled: 2026-07-21.

---

## 0. The meta-lessons (read these first; they are the expensive ones)

1. **Prove the product on off-the-shelf hardware BEFORE you cut a custom PCB.**
   The single biggest waste on the SAO was committing to a custom board before the concept was
   proven in-hand. Buy a dev board that already has the MCU + display you want, load the firmware,
   put it in a printed shell, and confirm the PRODUCT works and feels right. Only then spin a custom
   PCB, and only to make it smaller / manufacturable / sellable. A custom board should be the LAST
   step, not the first.

2. **Question the ENVELOPE before you design inside it.**
   Weeks were lost shrinking artwork to fit a board-width limit that was never actually a limit.
   Before you lay anything out, verify the real physical constraints (board size, keep-outs, tall-
   component clearance, connector mating) against the actual spec/drawing/datasheet, not an assumption.

3. **Do not marry a form factor that fights the product.**
   The SAO's mono display + tiny envelope actively worked against a color, animated concept. Pick the
   body that serves the product (display type, size, MCU headroom), then design the board around it.
   If reusing an existing board means gutting what makes the product good, don't reuse it.

4. **Ground truth beats the tool's convenient answer.**
   Every time a tool's return value disagreed with reality, reality won. The PCB canvas drew parts
   that had no drill; the design tool's connectivity model said "connected" when the fab's own check
   said "open." Trust the manufacturing output (Gerber/drill/flying-probe files), the DRC engine, and
   the bytes on disk over any in-tool claim.

5. **Three strikes, then question the PLACEMENT, not the route.**
   When a trace won't route after a few honest tries, stop retrying the trace. The fix is almost
   always moving the parts so the topology is easy, not a cleverer path. The SAO burned an entire
   night grinding one net that was unroutable because the parts were in the wrong place.

6. **A component carries EVERY net on its pads.** Before relocating a part to help one net, check
   where its OTHER nets have to go. A ballast for a display-corner LED has a cathode net (to the LED)
   and an anode net (to the driver); put it where BOTH can route, not where only the net in front of
   you is happy.

---

## 1. Firmware architecture (do this from line one)

Several firmwares built this way use it, and it is the reason they are portable:

- **Ports-and-adapters / HAL boundary.** `core/` (game logic, classification, state, scan parsing)
  is PURE, hardware-free code. All hardware lives behind interfaces under `hal/` (display, radio,
  buttons, accel, haptic, storage). The core imports zero hardware.
- **Consequence you get for free:** the core is unit-testable on a laptop (CPython or native C++
  host build), and you can build a desktop simulator (SDL) that renders every UI screen with
  injected fake data. You debug the product on your PC and only flash for hardware-specific bugs.
- **When you port to new silicon or a new display, you rewrite adapters, not the brain.** Budget the
  port as "new HAL implementations + UI re-layout," never "rewrite the app."

---

## 2. ESP32 RF gotchas (2.4 GHz WiFi + BLE)

- **WiFi + BLE coexistence boot-loop.** With both radios active, `esp_wifi_set_ps(WIFI_PS_NONE)`
  aborts inside `coex_core_enable` and boot-loops. Use `WIFI_PS_MIN_MODEM` instead. This bit PTR on
  the S3 and will bite any dual-radio build (C3/S3 alike).
- **Single radio = time-share.** One 2.4 GHz radio cannot do WiFi-promiscuous and BLE scan at full
  rate simultaneously. Design a time-share (WiFi N seconds / BLE M seconds). The C3 forces this; the
  S3 has more headroom but the pattern is the same.
- **Passive / RX-only is the ethic AND the safe default.** Both products are receive-only: passive
  AP scan (`WiFi.scanNetworks()`), NimBLE advertised-device listen. No deauth, no injection, no probe
  attack. This keeps them legal, con-appropriate, and marketable. Keep it a hard invariant, not a
  config toggle.
- **Antenna is the only real range lever; SoC RX sensitivity is fixed (~-97 dBm).** A small internal
  ceramic/flex antenna covers ~10-30 m in a crowded band (right for personal/near-field threat use).
  An external antenna adds ~3-5 dBi (~1.5-2x range) but hurts inconspicuousness. For a survey/wardrive
  goal you want the external; for "what's near me" the internal is correct. A concealed u.FL pad lets
  one board do both (internal default, snap on a stick for range mode).
- **Memory in dense RF.** In airport/con-density RF the contact/device store balloons. Cap it and age
  entries aggressively. On a no-PSRAM part (C3) this matters much more than on an S3 with PSRAM.

---

## 3. PCB design in EasyEDA Pro (only if/when you cut a custom board)

Reach for a custom PCB LAST (see meta-lesson 1). When you do, drive EasyEDA by SCRIPT, not by
clicking the canvas.

- **Advanced (A) > Run Script(S)... = the full `eda.*` JS API** (Monaco editor). The canvas is a
  Chromium surface with NO accessible UI elements, so pixel-clicking it is hopeless. The API reads
  and writes the design as data.
- **The working loop:** put the JS on the clipboard (`Set-Clipboard`), click ON a line of code in the
  editor (not empty space), Ctrl+A then Ctrl+V (paste via clipboard; never type - Monaco mangles
  typed brackets), then Run(F9). Read results back with the message box.
- **Output channel is `eda.sys_MessageBox.showInformationMessage(text)` ONLY.** `navigator.clipboard`
  hangs (doc not focused), `document` is absent (no DOM), toasts render off-screen. Keep the message
  short; keep brackets/pipes/colons out of it so it reads back cleanly. ALWAYS close the message box
  before pasting the next script - a modal box silently eats the next paste.
- **Units are MIL.** `layer 1 = top, 2 = bottom, 12 = through-hole`. Pads key on `padNumber` (not
  `pinNumber`). `mil(x_mm) = 3965 + x_mm*39.3701` style transforms show up in every script.
- **Traps that cost real time:**
  - **Two `modify()` back-to-back: the second silently no-ops.** Do one, read it back, then the next.
  - **`modify({layer:2})` mirrors TOP-BOTTOM** (Y flips about the part center). For a header mating a
    fixed socket this inverts the pinout (fix it in the schematic).
  - **EasyEDA AUTOSAVES the .eprj2.** "I won't save yet" is not a plan - copy the file and md5 it
    before edits; restore by killing the app and copying the backup over the working file.
  - **The DRC panel goes STALE** while a dialog is open. `pcb_Drc.check()` returns only a boolean;
    read counts from the panel AFTER a fresh reopen, or re-run Check DRC on the clean canvas.
  - **Connectivity is LAYER-KEYED.** Two pieces of copper at the same X/Y on DIFFERENT layers are NOT
    connected without a via. Any home-grown connectivity check must key nodes by (x, y, layer) and
    only bridge layers through a via/hole; otherwise it will report "connected" while the fab's DRC
    reports "open." This one caused hours of chasing phantom connections.
  - **Create scripts execute but their result message box often renders BEHIND the editor** - don't
    trust the message; verify every mutation via a fresh Check DRC.
- **Verify from the fab output, not the canvas.** Export the Gerber/drill zip and read
  `FlyingProbeTesting.json` - it lists every pad's net, coords, layer, and hole size = machine-
  readable ground truth. The canvas will happily draw alignment circles with no drill; the fab files
  won't lie.

---

## 4. Flash + bring-up

- **If the board has native USB (any ESP32-S3), just use USB-C.** S3 has full USB-OTG: DFU/flash and
  even HID over the USB pins. This is a huge simplification over the SAO, which had NO native USB and
  had to flash over UART pads. Prefer an S3 with USB-C exposed.
- **If flashing over UART (no native USB):** use a USB-to-UART adapter (CP2102 works). If the adapter
  has no DTR/RTS, enter download mode MANUALLY (hold BOOT to GND, tap EN/RESET to GND, release BOOT,
  flash). TX/RX cross: board TXD -> adapter RXD, board RXD -> adapter TXD, common GND.
- **Power during bench bring-up from a BENCH PSU at the board's real rail voltage** (set current
  limit, watch draw). Do NOT power from the adapter's 3V3 pin (brownout + wrong voltage). Cross TX/RX
  and share GND only.
- **Bring-up order that works (proven on the XIAO):** rebuild from the simplest working thing, one
  subsystem at a time, on hardware. Single always-on LED -> display -> sensor -> radio -> full app.
  Do NOT debug the whole stack at once, and do NOT run an on-device `while True` over a serial link
  during wiring (jostling resets the MCU and locks the REPL; use short FINITE tests).
- **Measure the power budget on the bench** (the SAO drew ~74-84 mA @ 3.0V with a power-hungry OLED;
  a reflective Sharp/low-power display draws far less). Know the number before you spec a battery.

---

## 5. Ordering (JLCPCB assembled)

- **Lead time measured: ~14 calendar days order -> in-hand** for a small assembled pilot. Pad to ~3
  weeks for a bigger batch.
- Flow: EasyEDA `Export > Order PCB/FPC at JLCPCB...` (starts the flow, does not charge). On the JLC
  page turn ON PCB Assembly, pull in BOM + CPL, and use the **PCBA placement preview as the pre-order
  checkpoint** - confirm every part is in stock, LED polarity is right, and DNP parts are marked DNP.
- **Consigned parts (e.g. a specialty display panel) are NOT in the JLC order** - buy them separately
  and hand-attach. Budget that separately.
- **Do a fold/bench test of any flex/glass BEFORE ordering a batch** if the design bends anything -
  there is often no bend-radius spec and you find the failure only by trying it.

---

## 6. The specific "do not repeat" list from the SAO

- Do not populate a mating connector on the same face as the display if the two must face opposite
  ways when it plugs in (the connector-orientation defect that killed the pilots).
- Do not trust a footprint's drawn alignment circles to have a drill - verify NPTH in the drill file.
- Do not put pads/parts where their pads clear a neighbor but the part BODY overlaps it - check the
  courtyard, not just the pads.
- Do not reuse a paid-for board just because it exists (sunk cost). If it fights the product, cut a
  new one shaped for the product.
- Do not let a custom-board problem block a working firmware idea - the firmware is the value; the
  board is a delivery vehicle you can swap.

---

## 7. The shared-platform idea (why both pocket devices can be ONE board)

PTR (threat radar) and HexHound (recon pet) need the same core hardware: an ESP32-S3 + a 240x240
color IPS + LiPo + charger + accelerometer (LIS3DH) + 1-2 buttons + a tiny haptic + a good internal
antenna (with an optional concealed u.FL). Design and validate ONE board; give each product its own
enclosure and its own firmware load. Two finished products off one bring-up. See each project's
HARDWARE-PLAN.md for the product-specific enclosure, antenna, and firmware-port details.
