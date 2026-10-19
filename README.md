# Aliens vs Predator VR

An unofficial VR mod for **Aliens vs Predator** (2010, Rebellion, DirectX 11). The game has no VR support of its own.
This mod is a proxy `dxgi.dll` that hooks the game's rendering and input and talks to SteamVR directly from inside
the game: stereo 3D for all three species, head-tracked look and movement, the Marine's gun and the arms of the
Marine and the Predator on your motion controllers, and an in-headset settings menu.

**Status: Beta, a hobby reverse-engineering project.** It's playable, but there are rough edges; see
[Known issues](#known-issues) and [Planned](#planned).

## Requirements

- Aliens vs Predator (the Steam PC release), its **DirectX 11** version, `AvP_DX11.exe`. The DirectX 9 `AvP.exe` isn't
  supported.
- SteamVR **2.17 or newer** (the release that added a 32-bit runtime), with a headset and motion controllers.
  Developed and tested on a Meta Quest 3 over Steam Link with Touch controllers; other headsets should work, but only
  the Touch controllers get default bindings.
- `openvr_api.dll` (32-bit) from your SteamVR install.

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
- **Recenter** from the settings menu, or touch both thumbrests at once.

### Motion controllers
- **Arms on your hands** (Marine and Predator): each hand sits on its controller and the arms bend to reach it
  (two-bone IK). The body position, torso turn and elbow angle are adjustable per species, and the hand calibration
  (offset and rotation) is one set, mirrored for the left hand.
- **The game's animations play on top:** attacks, reloads and recoil still move your hands from where the
  controllers hold them (can be switched off).
- **The Marine's gun is in your right hand, and shots leave the gun** along its barrel, independent of where you
  look. Each weapon keeps its own grip and shot-line calibration; weapons are recognised automatically.
- **Laser sight** along the shot line, for calibrating (off by default; it shows through walls).
- **Two-handed aiming** (Marine): grip with your left hand on the barrel and the gun points along the line between
  your hands.

### Controls (Touch, default bindings)

The controllers act as an Xbox pad for the game; every action can be rebound in SteamVR.

| Touch | Game (Xbox pad) |
|---|---|
| Sticks, stick clicks | Sticks, stick clicks |
| Triggers | LT / RT |
| Grips | LB / RB (the left grip on the Marine's gun barrel is the two-handed grab instead) |
| A / B / X / Y | A / B / X / Y |
| Double press X / Y / B / A | D-pad left / up / right / down |
| Hold the left / right thumbrest (0.8 s) | Back / Start |
| Touch both thumbrests at once | Recenter the view |

### Settings menu

Open the SteamVR dashboard and pick the **AvP VR** tab. Every setting is there (nothing needs editing in the ini),
grouped into View, HUD, Controls, Head look, Hands, Hand placement, Weapon and Aiming, and changes apply and save
immediately. Settings are kept **per species**, switched automatically with the species you play, and the gun and
shot values also **per weapon**; rows that don't apply to the species you're playing are hidden.

## Known issues

- **The Alien has no hands on the controllers yet**, and the Alien's and Predator's attacks are on buttons (grips,
  triggers), not your hand movements.
- **As the Alien, some distant things shift slightly against your head movement** (for example the view through
  windows), while nearby walls and characters stay put. Being investigated.
- **Multiplayer** works a solo survival multiplayer gamemode, but hasn't been tested with a second player: what others see of your head-driven
  look and aim isn't verified.
- Predator vision modes and zoom haven't been checked in VR.
- The laser sight shows through walls (no hit test).
- No controller rumble yet.

## Planned

- **Body holsters:** reach to a spot on your body and grip to draw or switch a weapon.
- **Physical gestures:** swing your hands to attack (the Predator's wrist blades, the Alien's claws, the Marine's
  melee).
- The Alien's hands on the controllers.
- Controller rumble.

## Credits

Built with [MinHook](https://github.com/TsudaKageyu/minhook) and the [OpenVR](https://github.com/ValveSoftware/openvr)
SDK. Not affiliated with Rebellion, SEGA, 20th Century Studios or Valve.
