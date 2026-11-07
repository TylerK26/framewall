# framewall

Stream your Linux desktop to a VR headset as a **monitor wall**: your real
screen plus any number of **virtual monitors**, captured together as **one**
Moonlight stream and shown in the headset's theater/cinema screen.

Built for the **Steam Frame**, with a KDE Plasma laptop or desktop as the host.

```
 ┌──────────────┬──────────────┐
 │  virtual 1   │  virtual 2   │     2 × 1920×1080 virtual monitors
 │  1920×1080   │  1920×1080   │   + the laptop's 1920×1200 screen
 └──────┬───────┴───────┬──────┘   = one 3840×2280 stream
        │    laptop     │
        │   1920×1200   │
        └───────────────┘
```

- **One Sunshine, one Moonlight.** No juggling a stream per monitor.
- **Virtual monitors on demand.** They exist only while you're connected and
  are removed when you close the stream. Count, resolution, refresh rate and
  layout are set in one config file, and each preset shows up as an app in
  Moonlight.
- **Full resolution on the Steam Frame.** The client lifts gamescope's
  1920×1080 limit for the Moonlight window, so a 3840×2280 wall isn't squeezed
  down before it reaches the headset.
- **Controller passthrough.** Launched from Steam, the headset's controllers
  appear on the PC as an Xbox controller.

## How it works

- **Host** (`host/framewall`): creates KWin virtual monitors with
  `krfb-virtualmonitor`, lays them out around your physical screen, and runs a
  dedicated Sunshine instance. Sunshine needs a small patch
  (`sunshine/0001-kwingrab-workspace-capture.patch`). It adds
  `output_name = workspace`, which captures the bounding box of all screens
  through KWin's `stream_region` instead of a single monitor.
- **Client** (`client/framewall`): runs Moonlight on the headset with the
  right resolution and frame rate, read from the preset's app name. Under
  gamescope it sets `GAMESCOPE_XWAYLAND_MODE_CONTROL` so Moonlight gets a
  screen as large as the wall.

## Requirements

**Host**
- Linux with **KDE Plasma 6 on Wayland** (tested on 6.8, CachyOS)
- `krfb` (for `krfb-virtualmonitor`) and `kscreen` (`kscreen-doctor`)
- A hardware encoder Sunshine supports (tested: Intel Arc iGPU via VAAPI)
- The patched Sunshine, built with `sunshine/build.sh` (installs to
  `~/.local/opt/sunshine-wall`; your regular Sunshine package stays as is)
- The screen you include must use **100% scaling**

**Client**
- Steam Frame (SteamOS), with the **Moonlight** flatpak
  (`com.moonlight_stream.Moonlight`)
- Should also work on other gamescope/SteamOS devices and on plain Linux;
  only the Steam Frame is tested

A fast link matters: the Steam Frame's 6 GHz wireless adapter works well.

## Setup

### 1. Host: build the patched Sunshine

```sh
git clone https://github.com/<you>/framewall.git
cd framewall
./sunshine/build.sh          # add --cuda for NVENC (needs the CUDA toolkit)
```

### 2. Host: configure and start

```sh
mkdir -p ~/.local/bin ~/.config/framewall
install -m 755 host/framewall ~/.local/bin/framewall
cp host/config.example ~/.config/framewall/config
$EDITOR ~/.config/framewall/config     # presets, encoder, GPU, port
framewall install
```

`framewall install` prints the Moonlight app names, the web UI address, and
the ports to open in your firewall. For example, with ufw and the default port:

```sh
sudo ufw allow 48384:48410/tcp
sudo ufw allow 48384:48410/udp
```

It runs as a systemd user service (`framewall.service`) from login. Run
`framewall install` again after changing the config; pairing is kept.

### 3. Headset: Moonlight and the launcher

1. Install Moonlight (Discover in Desktop mode, or
   `flatpak install flathub com.moonlight_stream.Moonlight`).
2. In Moonlight, add the host by IP and port, e.g. `192.168.1.20:48389`, and
   pair it. Enter the PIN at `https://localhost:48390` on the PC.
3. Copy `client/framewall` to the headset, e.g. `~/framewall`, and make it
   executable. Optionally copy `client/framewall.conf.example` to
   `~/.config/framewall.conf` to pick a default preset and bitrate.
4. In Steam: **Add a Non-Steam Game** → browse to `~/framewall` → add it, and
   set its controller layout to **Gamepad**.

Launch it from Steam. To switch presets, set `APP` in
`~/.config/framewall.conf` (`~/framewall --list` shows the names), or give
the app name as a launch option.

## Configuration

Host (`~/.config/framewall/config`, see `host/config.example`):

| Setting | Meaning |
|---|---|
| `PRESETS` | One line per wall: `"COUNT WIDTHxHEIGHT REFRESH LAYOUT"`, e.g. `"2 1920x1080 108 top"` |
| `PHYSICAL` | Screen to include: `auto` (primary), an output name, or `none` |
| `ENCODER` / `ADAPTER` | Sunshine encoder (`vaapi`, `nvenc`, …) and GPU render node |
| `PORT` | Sunshine base port (separate from a normal Sunshine on 47989) |
| `NAME` | Host name shown in Moonlight |

Layouts:
- `top`: virtual monitors in a row, physical screen centred below. With two
  1080p monitors over a 16:10 laptop the wall is close to 16:9, so it **fills a
  theater screen**.
- `row`: physical screen in the middle, virtual monitors left and right. Wide,
  so it's letterboxed on a 16:9 screen.
- `grid`: all screens in a near-square grid.

Client (`~/.config/framewall.conf` on the headset): `HOST`, `APP`,
`BITRATE_KBPS`.

## Tips

- **Match the refresh rate to the headset.** If a preset's rate is higher than
  the headset's, the headset GPU decodes frames it never shows, and head
  movement can stutter. On the Steam Frame, 108 Hz with a 108 Hz preset was the
  sweet spot for a 3840×2280 wall. 144 Hz stuttered.
- **Sharper text:**
  - Use a larger theater screen.
  - Raise `BITRATE_KBPS` (e.g. `150000`).
  - Switch the host's font smoothing to **grayscale**: RGB subpixel fringes
    smear after video compression and VR scaling.
- **Hybrid laptops:** encode on the integrated GPU (`vaapi`). It keeps the
  discrete GPU free for games and asleep when idle. An idle NVENC Sunshine
  keeps an NVIDIA GPU awake.

## Known limitations

- KDE Plasma (KWin) only: the capture and the virtual monitors use KWin
  protocols.
- Screens must use 100% scaling.
- On the Steam Frame the laser pointer doesn't line up with the desktop on the
  enlarged Moonlight screen (gamescope maps it using its normal 1920×1080).
  The gamepad works normally.
- `krfb-virtualmonitor` also opens a VNC port per virtual monitor (5911,
  5912, …) protected by a random password; keep those ports closed in your
  firewall.
- Don't add or remove other monitors while a wall is streaming: KWin reshuffles
  the layout and the mouse mapping goes wrong until you reconnect.

## License

GPL-3.0-or-later. The Sunshine patch is a derivative of
[Sunshine](https://github.com/LizardByte/Sunshine) (GPL-3.0).
