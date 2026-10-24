# Play controls

This work adds native desktop controls and a separate iPhone/iPad touch frontend.
The game still receives its original Switch controller input; no game assets or
HUD addresses are patched for controls. The iOS runtime and device build must
be verified separately from the touch model or a UIKit demonstration.

## Desktop behavior

- WASD moves; mouse motion looks around while capture is enabled with F1.
- F1 toggles capture. Escape releases the cursor and opens the journal while
  captured; with the cursor free, Escape goes back. Tab opens the character menu
  and releases capture. Focus loss clears desktop input and restores the cursor.
- Space jumps, E interacts, R readies/sheathes, F changes perspective, Shift
  sprints, Ctrl/C sneaks, Z shouts, T waits, Q opens favorites, 1/2 are hotkeys.
- Captured left/right mouse buttons operate the right/left hand; middle mouse
  changes perspective. With the cursor free, left/right clicks confirm/back.
- Menus use arrows or the wheel and Enter/click to select the current item.
  Clicking arbitrary menu text does not provide native pointer hit testing in
  the Switch UI. The window title explains capture and menu controls.
- Cmd shortcuts are reserved for macOS. Mouse capture never survives focus loss
  or application shutdown. Mouse motion stops producing look input when idle.

Bindings come from `interface/controls/nintendox/controlmap.txt` and `gamepad.txt`
inside the installed game's Interface archive. Jump is Nintendo X; ready weapon
is Nintendo Y. The game retains its controller button prompts.

Software-keyboard requests open a native text dialog on either platform, with
the game's initial text and length limits. Confirm returns UTF-8 or UTF-16 in
the original SDK format; Cancel leaves the guest output unchanged. The UI stays
responsive while only the requesting guest thread waits. App interruptions
cancel the dialog and release input.

## Touch behavior

The landscape iPhone/iPad layout has floating movement and look sticks that
appear under the finger on the left and right halves of the screen, a
right-edge column of Sneak, Jump, Use and Attack icon buttons above a clear
lower-right Look corner, a Pause button, safe-area margins, and
support for simultaneous movement, looking and attacking. Sprint is held
automatically while moving, except while Sneak is toggled on. Menus replace
Pause with Back in the same top-left corner. Each finger owns its control until
release or cancellation. Resize and app interruptions cancel touch input. The layout and
button mapping are Skyrim-specific; the repository's Diablo touch/HUD bridge is
not used.

Keyboard, touch and optional stdin input retain separate held and pending
states. Button masks combine. Each stick uses the first nonneutral vector in
desktop, touch, then external-input priority; left and right sticks are chosen
independently. Resetting one source preserves the others.

## Verification

Required checks cover action mappings, modifiers, capture transitions, mouse
motion/idle reset, independent input sources, touch ownership, simultaneous
controls, layout bounds, cancellation and resizing. Native AppKit and UIKit
adapters must be exercised separately from pure state tests. Successful iOS
controls tests do not prove Skyrim runs on a phone; that requires signed guest
loading, Metal/audio output, and real game input on the device or simulator.

Current checks: the full macOS suite passes, including AppKit events, source
composition, software-keyboard ABI, signed loading and real rendered frames.
Native keyboard input navigated the visible game through New Game into the cart;
captured mouse motion turned the camera, and Escape opened the journal and
restored the cursor. Native text confirmation/cancellation also passed in a
separate AppKit dialog test. Character naming in the full game is not yet verified.

The current 12-test UIKit controls suite passes on the iPad Simulator. Earlier
physical iPhone and iPad controls tests verified actual HID readback. The tests
cover touch composition, lifecycle, text entry and setup retry. The iPad's full
asset set was copied and hash-verified, and real finger input reaches the guest.
An add-on shop action exposed an unsupported Nintendo applet path; it now
returns safely in the offline host, with a forced-entry ABI regression test.
The user historically confirmed physical-iPad gameplay at approximately 5 fps.
Reaching 30+ fps remains a separate performance target. The real-game Journal
smoke passes on the iPad Simulator with normalized screenshots and matching
menu-touch evidence.

Current physical-device validation is recorded in
[Simulator gameplay parity](../docs/SIMULATOR-GAMEPLAY.md). Historical user
confirmation is not a substitute for rerunning the controls and Release Journal
checks on the current build. Screenshots use XCUITest attachments.
