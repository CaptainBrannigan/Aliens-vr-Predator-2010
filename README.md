# Aliens vs Predator VR

An unofficial VR mod for **Aliens vs Predator** (2010, Rebellion, DirectX 11). The game has no VR support of its own.
This mod is a proxy `dxgi.dll` that hooks the game's rendering and input and talks to SteamVR directly from inside
the game: stereo 3D for all three species, head-tracked look and movement, the Marine's gun and the arms of the
Marine and the Predator on your motion controllers, and an in-headset settings menu.

**Status: 1.5, a hobby reverse-engineering project.** See [Known issues](#known-issues) and [Planned](#planned) for
what isn't done yet.

> **SteamVR is the only runtime targeted.** The mod talks to SteamVR through its `openvr_api.dll` and nothing else: it
> doesn't use OpenXR. **Virtual Desktop users on VDXR need to be in SteamVR:** start SteamVR, then launch the game from
> there. VDXR on its own won't work.

## Requirements

- Aliens vs Predator (the Steam PC release), its **DirectX 11** version, `AvP_DX11.exe`. The DirectX 9 `AvP.exe` isn't
  supported.
- SteamVR **2.17 or newer** (the release that added a 32-bit runtime), with a headset and motion controllers.
  Developed and tested on a Meta Quest 3 over Steam Link with Touch controllers; other headsets should work. Quest, Rift,
  Valve Index, Steam Frame, Pico, Vive (wands, Cosmos, Focus 3), HP Reverb G2 and Windows MR controllers get default
  bindings (Virtual Desktop included); others need a binding made in SteamVR.
- `openvr_api.dll` (32-bit) from your SteamVR install.
- Nothing to set in the game: the mod switches the game's controller option on at launch; after that it's yours to
  change.

## Installing

Copy into the game's install folder, next to `AvP_DX11.exe`:

- `dxgi.dll` from [Releases](../../releases) (or build it yourself, see [DEVELOPMENT.md](DEVELOPMENT.md)).
- `openvr_api.dll`, copied from `<Steam>\steamapps\common\SteamVR\bin\win32\openvr_api.dll`.

Start SteamVR, then launch the game's DirectX 11 version. **Turn the game's motion blur off** in its graphics
options: it compares each frame with the previous one, and in VR those are the two eyes.

The mod creates `avpvr_settings.ini` and its SteamVR action and binding files next to the DLL on first launch. The
settings start from the author's own tuned values, per species; an existing `avpvr_settings.ini` is kept. Settings are
changed from inside the headset (see [Settings menu](#settings-menu)).

The mod never starts SteamVR itself: it checks for it every few seconds and switches VR on once it's running. Without
it the game shows on the monitor as usual.

**Controller bindings:** in SteamVR's controller bindings for the game, pick **"AvP VR defaults"**. If you previously
saved your own binding for the game, SteamVR keeps using that copy and won't pick up new actions from a newer
version of the mod until you switch back to the defaults (or add the new actions to your copy).

**Editing bindings by hand:** if SteamVR won't let you edit a binding, copy your controller's `avpvr_bindings_<type>.json`
from the game folder to `avpvr_custom_bindings_<type>.json` in the same folder, change the copy, and restart the game. The
default files are rewritten every launch, so edit only the copy. Your controller's type is in `avpvr_input.log`, and
deleting the copy goes back to the defaults.

To remove the mod, delete `dxgi.dll` from the game folder.

## Features

### Rendering
- **Stereo 3D** for the Marine, the Alien and the Predator, single player and multiplayer: the game renders each eye
  with its own camera, at the headset's real, asymmetric field of view, with full lighting and shadows.
- **HUD** in a head-locked panel that fuses in both eyes, with adjustable size, distance and height. The Marine's
  screen-centre reticle is hidden (you aim with the gun), and a duplicate of the motion tracker the game leaves at
  its flat-screen spot is removed.
- **Menus, cutscenes, movies and loading screens** are shown on a floating screen in front of you, in full colour
  (also the multiplayer menu, which doesn't pause the game).
- The **Alien's fisheye** lens effect is off in VR by default (it warps each eye's image on its own); the setting
  "Screen warp effects" brings it back.

### Head tracking
- **Head look:** your head aims, for all three species; the right stick turns smoothly. The view stays upright on
  Alien walls and ceilings, and roll comes from your head only. A per-species look offset lines up where the game
  aims with where you look.
- **Positional tracking (6DoF):** lean, crouch and peek around corners. Walk around your room and the character
  **steps under your head** once you're a set distance away, through the game's own movement, so walls and collision
  still apply (the distance and step speed are settings). Your own stick movement carries your play space along.
- **Recenter** from the settings menu, or touch both thumbrests (Index: trackpads) at once.

### Motion controllers
- **Arms on your hands** (Marine and Predator): each hand sits on its controller and the arms bend to reach it
  (two-bone IK). **The elbows follow the roll of your wrists** the way real ones do: palm down swings them out, palm
  up brings them in, less so the closer your hand is to your body. The body position, torso turn, each shoulder's
  position and the elbow model are adjustable per species, and the hand calibration (offset and rotation) is one set,
  mirrored for the left hand.
- **The game's animations can play on top:** attacks, reloads and recoil move your hands from where the controllers
  hold them (on for the Predator; off by default for the Marine, whose gun it would sway).
- **The Marine's gun is in your hand, and shots leave the gun** along its barrel, independent of where you look. Each
  weapon keeps its own grip and shot-line calibration; weapons are recognised automatically.
- **The muzzle flash stays on the gun in your hand** (setting "Muzzle flash on the gun"), with its own offsets per
  weapon.
- **The Marine's flashlight can go where you want** (setting "Flashlight on"): the game's own spot, your head, chest,
  either hand, or the weapon (from the muzzle flash, along the shots), with its own offsets.
- **Laser sight** along the shot line, for calibrating (off by default; it shows through walls).
- **Two-handed aiming** (Marine): grip with your off hand on the barrel and the gun points along the line between
  your hands.
- **Holsters:** grip with your palm at a spot on your body (stomach, hips, chest, either shoulder) to press that
  spot's button. The Marine's defaults pick weapons, and the weapon goes to the hand that grabbed it, as if left-handed
  mode switched on or off (setting "Weapon goes to the hand that grabs it"). Each spot's position and button are in the
  Holsters settings; the Predator and Alien spots start unassigned. "Show the grab zones" draws every spot and the
  two-handed grab in the headset while you set them up.
- **Left-handed mode** (setting "Left-handed", off by default; one setting for all species): the arms play each
  other's animations mirrored, the Marine's whole upper body mirrors with them, its gun goes in your left hand, and the
  triggers and grips swap so your left hand fires. The gun itself isn't mirrored.
- **Rumble:** the game's own rumble (gunfire, explosions, hits) on the hand holding the weapon -- the one that last
  took it from a holster, or your main hand until then -- and on the off hand while it grips the barrel, on controllers
  that have it
  (settings "Controller rumble" and "Rumble strength"). It stays on with the camera shake turned off ("Keep the game
  camera's motion"), and off whenever the game itself wouldn't rumble (its vibration or controller option off). A binding of your own saved in SteamVR needs the two Rumble actions bound to the hands' haptics.

### Controls (default bindings)

The controllers act as an Xbox pad for the game; every action can be rebound in SteamVR. Touch covers Quest 2 / 3 / Pro
and Rift. Virtual Desktop's Index emulation of Touch controllers uses the Index column (physical X / Y are its left
A / B). The other controllers are in the second table; anything else needs a binding made in SteamVR.

| Game (Xbox pad) | Touch | Index |
|---|---|---|
| Sticks, stick clicks | Sticks, stick clicks | Thumbsticks, stick clicks |
| LT / RT | Triggers | Triggers |
| LB / RB (the off-hand grip on the Marine's gun barrel is the two-handed grab instead, and a grip in a holster spot presses that spot's button) | Grips | Squeeze the grips |
| A / B | A / B | Right A / B |
| X / Y | X / Y | Left A / B |
| D-pad left / up / right / down | Double press X / Y / B / A | Double press left A / left B / right B / right A |
| Back / Start | Double tap the left / right thumbrest | Double tap the left / right trackpad, or hold the left / right stick click (0.8 s) |
| Recenter the view | Touch both thumbrests at once | Touch both trackpads at once |

Triggers are LT / RT on every controller. Grips are LB / RB too, except on Steam Frame.

| Game (Xbox pad) | Steam Frame | Pico, Vive Focus 3, Vive Cosmos, HP Reverb G2 | Windows MR | Vive wands |
|---|---|---|---|---|
| Sticks | Sticks | Sticks | Sticks | Trackpads (touch) |
| Stick clicks | Stick clicks | Stick clicks | Stick clicks | Click the middle of the trackpads |
| A / B / X / Y | A / B / X / Y | A / B / X / Y | Click the right trackpad south / east / west / north | Click the right trackpad south / east / west / north |
| D-pad | D-pad | Double press A / B / X / Y as on Touch | Click the left trackpad | Click the left trackpad |
| LB / RB | Bumpers | Grips | Grips | Grips |
| Back / Start | View / Menu, or double tap the left / right thumbrest | Hold the left / right stick click (0.8 s) | Left / right menu button | Left / right menu button |
| Recenter the view | Touch both thumbrests at once | Settings menu | Settings menu | Settings menu |

Virtual Desktop's emulation doesn't report trackpad touches by default, so there Back / Start are on the held stick
clicks and recenter is in the settings menu.

### Settings menu

Open the SteamVR dashboard and pick the **AvP VR** tab. Every setting is there (nothing needs editing in the ini),
grouped into View, HUD, Controls, Head look, Hands, Hand placement, Holsters, Weapon, Aiming, Flashlight and Game
controllers, and changes apply and
save immediately. The page scrolls with the buttons at the top, or your stick if SteamVR passes it on. Settings are kept
**per species**, switched automatically with the species you play, and the gun and shot values also **per weapon**;
rows that don't apply to the species you're playing are hidden.

## Known issues

- **Motion controllers not reaching the game** (only turning works): the game binds pad buttons only to a controller
  Windows lists to it, so a PC without an Xbox pad got none. The mod now lists the motion controllers to the game as an
  Xbox pad once they're running. Not yet confirmed by a player who had the problem.
- **Other game controllers are hidden from the game in VR** (DualShock, wheel, HOTAS, a real Xbox pad), since their
  button layout would also be bound to the same buttons. The Game controllers group lists the motion controllers and
  every controller Windows reports (Steam Input's virtual pads count as one), each with whether its input reaches the
  game, and for each a "Hide from the game in VR" toggle: turn it off to have the game read that controller instead.
  In flat play nothing is hidden.
- **Default bindings other than Touch are new and untested on real hardware.** The hand calibration was tuned on Touch,
  so other controllers may need adjusting in "Hand placement" and the gun offsets.
- **The Alien has no hands on the controllers yet**, and the Alien's and Predator's attacks are on buttons (grips,
  triggers), not your hand movements.
- **As the Alien, some distant things shift slightly against your head movement** (for example the view through
  windows), while nearby walls and characters stay put. Being investigated.
- **Blood splatter sometimes shows in the left eye only** (reported by Virtual Desktop users). Being investigated.
- **The Marine's chest and stomach holsters** press D-pad right and down by default; what those select hasn't been
  checked. A D-pad spot counts as a weapon grab even if the game selects nothing, so the hands can swap on an empty slot.
- **Multiplayer** works a solo survival multiplayer gamemode, but hasn't been tested with a second player: what others see of your head-driven
  look and aim isn't verified.
- Predator vision modes and zoom haven't been checked in VR.
- Look objectives may have difficulty triggering due to a secondary "wide culling" calculation done to prevent pop in. Togglong that off in the settings should allow you to proceed.
- The laser sight shows through walls (no hit test).

## Planned

- **Manual reloading:** reload the Marine's weapons with your hands.
- **Physical gestures:** swing your hands to attack (the Predator's wrist blades, the Alien's claws, the Marine's
  melee).
- The Alien's hands on the controllers.

## Credits

Built with [MinHook](https://github.com/TsudaKageyu/minhook) and the [OpenVR](https://github.com/ValveSoftware/openvr)
SDK. Not affiliated with Rebellion, SEGA, 20th Century Studios or Valve.
