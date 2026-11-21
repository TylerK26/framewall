# framewall

**DISCLAIMER: This whole repository is vibecoded, I have not reviewed everything that claude wrote, proceed with caution and I am not responsible for any damages caused to your computer or your headset.**

Stream your Linux desktop to a VR headset as a **monitor wall**: your real
screens plus any number of **virtual monitors**, captured together as **one**
stream and shown as separate curved panels side by side around you.

Built for the **Steam Frame**, with a KDE Plasma laptop or desktop as the host.

- **One stream for the whole wall.** No juggling a stream per monitor.
- **Virtual monitors on demand.** They exist only while you're connected and
  are removed when you close the stream. Count, resolution and refresh rate
  are set in one config file.
- **Smooth at 144 Hz.** framewall-vr is a native OpenXR app: SteamVR places
  the panels itself at display time, so head movement stays smooth whatever
  the stream does, and each frame is on the panels ~1.5 ms after it's decoded.
- **Your room or a dark void** around the panels (passthrough), switchable
  from an in-headset menu.
- **Controllers:** a laser pointer drives the PC's mouse, an on-screen
  keyboard types on the PC, or the controllers act as an Xbox controller
  (with rumble). The PC's own mouse and keyboard keep working too.

## How it works

- **Host** (`host/framewall`): creates KWin virtual monitors with
  `krfb-virtualmonitor`, lays them out left and right of your physical screens
  (which keep their arrangement, and get it back afterwards), and runs a
  dedicated Sunshine instance. Sunshine needs a small patch
  (`sunshine/0001-kwingrab-workspace-capture.patch`). It adds
  `output_name = workspace`, which captures a region spanning several screens
  through KWin's `stream_region` instead of a single monitor. The region is the
  wall's exact rectangle, which framewall writes to a file named by
  `SUNSHINE_KWIN_WORKSPACE_REGION_FILE`. Without that file it falls back to the
  bounding box of all screens.
- **Headset** (`vr/`, `framewall-vr`): shows every monitor of the wall as its
  own panel, side by side on a curve around you. It's a native OpenXR app
  (Vulkan) for the Steam Frame built on Moonlight's streaming library
  (moonlight-common-c), and reuses the Moonlight app's pairing. The wall is
  decoded on the headset's hardware decoder; each frame is sampled straight
  from the decoder's buffer and converted in one GPU pass (~0.5 ms) as soon
  as it's decoded, into an image that every panel shows its own region of.
  The panels are OpenXR cylinder layers, which SteamVR composites itself.
- **Panel editor** (`host/framewall-panels`): a small window on the PC with
  sliders for the panels' sizes and placement, applied live in the headset.

## Requirements

**Host**
- Linux with **KDE Plasma 6 on Wayland** (tested on 6.8, CachyOS)
- `krfb` (for `krfb-virtualmonitor`) and `kscreen` (`kscreen-doctor`)
- A hardware encoder Sunshine supports (tested: Intel Arc iGPU via VAAPI)
- The patched Sunshine, built with `sunshine/build.sh` (installs to
  `~/.local/opt/sunshine-wall`; your regular Sunshine package stays as is)
- The physical screens you include must use **100% scaling**

**Headset**
- Steam Frame (SteamOS)
- The **Moonlight** flatpak (`com.moonlight_stream.Moonlight`), once, to pair
  with the host: framewall-vr uses its pairing

A fast link matters: the Steam Frame's 6 GHz wireless adapter works well.

## Setup

### 1. Host: build the patched Sunshine

```sh
git clone https://github.com/TylerK26/framewall.git
cd framewall
./sunshine/build.sh          # add --cuda for NVENC (needs the CUDA toolkit)
```

### 2. Host: configure and start

```sh
mkdir -p ~/.local/bin ~/.config/framewall
install -m 755 host/framewall ~/.local/bin/framewall
cp host/config.example ~/.config/framewall/config
$EDITOR ~/.config/framewall/config   # presets, encoder, GPU, port
framewall install
```

`framewall install` prints the wall presets, the web UI address, and the
ports to open in your firewall. For example, with ufw and the default port:

```sh
sudo ufw allow 48384:48410/tcp
sudo ufw allow 48384:48410/udp
```

It runs as a systemd user service (`framewall.service`) from login. Run
`framewall install` again after changing the config; pairing is kept.

### 3. Headset: pair with Moonlight

1. Install Moonlight (Discover in Desktop mode, or
   `flatpak install flathub com.moonlight_stream.Moonlight`).
2. In Moonlight, add the host by IP and port, e.g. `192.168.1.20:48389`, and
   pair it. Enter the PIN at `https://localhost:48390` on the PC.
   I recommend using the ip given by the 6ghz wireless dongle for the lowest latency

That's all Moonlight is needed for: framewall-vr streams on its own.

### 4. Headset: framewall-vr

On the headset (Desktop mode or SSH), download
`framewall-vr-VERSION-steamos-aarch64.tar.gz` from the
[latest release](https://github.com/TylerK26/framewall/releases/latest) and
run its installer:

```sh
tar xf framewall-vr-*-steamos-aarch64.tar.gz
./framewall-vr-*-steamos-aarch64/install.sh
```

Or build it from source instead:

```sh
git clone https://github.com/TylerK26/framewall.git ~/src/framewall
~/src/framewall/vr/build.sh
```

That downloads its dependencies (moonlight-common-c and the OpenXR SDK) and
builds it. Either way it's installed to `~/.local/opt/framewall-vr` with the
launcher at `~/framewall-vr`. In desktop mode, right click `~/framewall-vr`
(click the thumbstick) and click "Add to Steam", then launch it from Steam.
It picks the host's preset matching the headset's refresh rate.

- **Refresh rate:** SteamVR starts a new app at 72 Hz. In SteamVR's video
  settings, set framewall-vr's refresh rate to what you want (144 Hz works
  smoothly on the Steam Frame), and make sure the host has a preset at that
  rate (`config.example` has 90, 108, 120 and 144). The per-app render
  resolution doesn't matter: the panels are shown at the headset's own
  resolution regardless.
- **Controllers:**

  | Input | Does |
  |---|---|
  | Grip (one controller) | Laser on, on that controller; again on the same controller turns it off |
  | Trigger (light pull) or bumper | Left click (on the laser's controller) |
  | Thumbstick click | Right click |
  | Thumbstick | Scroll |
  | Both grips together | Open / close the menu |
  | B or Y (while the laser is on or the keyboard is shown) | Show / hide the on-screen keyboard |

  With the laser off and the menu closed, the controllers are an **Xbox
  controller** on the PC (sticks, triggers, bumpers, stick clicks, A/B/X/Y,
  d-pad, menu = Start, view = Back), with rumble. The grips never reach the
  PC. While the laser is on, or the menu or keyboard is open, the gamepad is
  released.
- **On-screen keyboard** (menu → Show keyboard, or B/Y with the laser on): a
  US keyboard floating low in front of you. Both controllers point at it; a
  trigger or bumper pull presses the key under that controller's dot and
  holds it until you let go, so the PC repeats it and you can type with both
  hands. Shift, Ctrl, Alt and Meta are sticky: tap once for the next key
  only, tap twice quickly to lock one (tap again to release it), e.g. lock
  Alt and tap Tab to switch windows. The laser controller still drives the
  mouse when it points at the panels instead. Hide closes it.
- **Menu** (both grips): passthrough on/off (remembered), recenter the panels
  in front of you, show / hide the keyboard, all panels smaller / bigger, quit (pull twice), and the
  stream's frame rate, latency and connection. While it's open, nothing
  reaches the PC.
- **Passthrough** uses OpenXR's own blend mode (`ALPHA_BLEND`): SteamVR shows
  your room through the app with whatever camera source it uses.
- **Panel sizes and placement:** run `framewall-panels` on the PC while
  streaming (see below). It's a small window with a slider for each panel's
  size, plus distance, height and the gap between panels. The window is on
  your PC's screen, so you can see it in the headset and drag the sliders with
  the laser or the PC's mouse; the panels change as you drag, and the other
  panels move to make room. framewall-vr saves the values on the headset, in
  `~/.config/framewall-vr.conf`.
- That file (created on the first run) can also be edited by hand: save it and
  the panels change right away. It has the stream settings too (preset,
  bitrate, audio, passthrough), which apply on the next start; see
  `client/framewall-vr.conf.example`.
- `~/framewall-vr --list` lists the host's presets, `~/framewall-vr --quit`
  closes a wall left running. The log is `~/.cache/framewall-vr.log`; every
  5 s it shows the headset's and the video's frame rates, missed frames and
  the decoded-to-on-screen time.

To update later: extract a newer release and run its `install.sh`, or, from
source, `git -C ~/src/framewall pull && ~/src/framewall/vr/build.sh`. Your
settings are kept either way.

To make a release tarball, run `vr/package.sh --version vX.Y.Z` on the
headset from a clean checkout; it writes the tarball and its checksum to
`vr/dist/`.

### 5. Host: the panel editor (optional)

`framewall-panels` needs PySide6 or PyQt6 (e.g. `python-pyqt6` on Arch):

```sh
install -m 755 host/framewall-panels ~/.local/bin/framewall-panels
install -Dm 644 host/framewall-panels.desktop ~/.local/share/applications/framewall-panels.desktop
```

It shows up as **Framewall panels** in the app menu, and it can be opened
before or after the stream starts. framewall-vr connects to it at the
host's address on Sunshine's `PORT` + 5 (TCP 48394 by default), which is
inside the firewall range above.

## Configuration

Host (`~/.config/framewall/config`, see `host/config.example`):

| Setting | Meaning |
|---|---|
| `PRESETS` | One line per wall: `"COUNT WIDTHxHEIGHT REFRESH"`, e.g. `"2 1920x1080 144"` |
| `PHYSICAL` | Physical screens in the wall: `all` (default), `primary`, a list of output names, or `none`. Screens left out keep working where they are. |
| `ENCODER` / `ADAPTER` | Sunshine encoder (`vaapi`, `nvenc`, …) and GPU render node |
| `VAAPI_QUALITY` | VA-API encoder preset: `speed` (default; the fastest, which 144 Hz walls need), `balanced`, `quality`, `auto` |
| `FEC_PERCENT` | Error correction share of the stream (Sunshine's default is 20); more rides out bigger bursts of Wi-Fi loss |
| `PORT` | Sunshine base port (separate from a normal Sunshine on 47989) |
| `NAME` | Host name shown in Moonlight |

The wall is side by side: the physical screens in the middle (moving as one
block, keeping their arrangement), the virtual monitors split left and right.

**Multiple monitors.** All physical screens are included by default, in the
arrangement you have; your arrangement is restored when the stream ends. Set
`PHYSICAL` to include only some of them: the others keep working where they
are, and the wall is placed to their right. Multi-monitor layouts are tested
with simulated screens only.

Headset (`~/.config/framewall-vr.conf`): `SIZE`, `SCREENS_SIZE`,
`VIRTUAL1_SIZE`, `VIRTUAL2_SIZE`, ..., `DISTANCE`, `HEIGHT`, `GAP` (applied
live), `PASSTHROUGH` (also in the menu), and `APP`, `BITRATE_KBPS`, `AUDIO`,
`HOST`, `QUIT_ON_EXIT` (applied on the next start).

## Tips

- **Match the refresh rate to the headset.** framewall-vr picks the preset at
  the headset's rate; a stream faster than the headset only costs GPU time.
- **Sharper text:**
  - Raise `BITRATE_KBPS` (e.g. `150000`).
  - Switch the host's font smoothing to **grayscale**: RGB subpixel fringes
    smear after video compression and VR scaling.
- **Brief freezes with blocky images** come from short bursts of Wi-Fi packet
  loss: the picture waits for a new keyframe (Sunshine's VA-API encoder can't
  recover any other way). On a Linux host, the Steam Frame's USB wireless
  adapter currently drops out briefly from time to time, which is the likely
  cause; it isn't framewall's encoder or decoder. A higher `FEC_PERCENT` or a
  lower `BITRATE_KBPS` may soften it.
- **Hybrid laptops:** encode on the integrated GPU (`vaapi`). It keeps the
  discrete GPU free for games and asleep when idle. An idle NVENC Sunshine
  keeps an NVIDIA GPU awake.

## Known limitations

- KDE Plasma (KWin) only: the capture and the virtual monitors use KWin
  protocols.
- Screens must use 100% scaling.
- `krfb-virtualmonitor` also opens a VNC port per virtual monitor (5911,
  5912, …) protected by a random password; keep those ports closed in your
  firewall.
- Don't add or remove other monitors while a wall is streaming: KWin reshuffles
  the layout and the mouse mapping goes wrong until you reconnect.

## License

GPL-3.0-or-later. The Sunshine patch is a derivative of
[Sunshine](https://github.com/LizardByte/Sunshine) (GPL-3.0).
