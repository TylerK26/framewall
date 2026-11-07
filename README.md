# framewall

**DISCLAIMER: This whole repository is vibecoded, and while I have some programming experience, I did not review everything that claude wrote, proceed with caution and I am not responsible for any damages caused to your computer or your headset.**

Stream your Linux desktop to a VR headset as a **monitor wall**: your real
screens plus any number of **virtual monitors**, captured together as **one**
Moonlight stream and shown in the headset's theater/cinema screen.

Built for the **Steam Frame**, with a KDE Plasma laptop or desktop as the host.

- **One Sunshine, one Moonlight.** No juggling a stream per monitor.
- **Virtual monitors on demand.** They exist only while you're connected and
  are removed when you close the stream. Count, resolution, refresh rate and
  layout are set in one config file, and each preset shows up as an app in
  Moonlight.
- **Full resolution on the Steam Frame.** With Steam's *Maximum game
  resolution* raised to the wall's size, Moonlight draws the wall 1:1 instead
  of squeezing it into 1920×1080.
- **Controller passthrough.** Launched from Steam, the headset's controllers
  appear on the PC as an Xbox controller, and the laser pointer works as a mouse
  that lines up with the desktop. Moonlight runs in its absolute ("remote
  desktop") mouse mode; set `ABSOLUTE_MOUSE=0` for mouse-look games.

## How it works

- **Host** (`host/framewall`): creates KWin virtual monitors with
  `krfb-virtualmonitor`, lays them out around your physical screens (which keep
  their arrangement, and get it back afterwards), and runs a dedicated Sunshine
  instance. Sunshine needs a small patch
  (`sunshine/0001-kwingrab-workspace-capture.patch`). It adds
  `output_name = workspace`, which captures a region spanning several screens
  through KWin's `stream_region` instead of a single monitor. The region is the
  wall's exact rectangle, which framewall writes to a file named by
  `SUNSHINE_KWIN_WORKSPACE_REGION_FILE`. Without that file it falls back to the
  bounding box of all screens.
- **Client** (`client/framewall`): runs Moonlight on the headset with the
  right resolution and frame rate, read from the preset's app name.

## Requirements

**Host**
- Linux with **KDE Plasma 6 on Wayland** (tested on 6.8, CachyOS)
- `krfb` (for `krfb-virtualmonitor`) and `kscreen` (`kscreen-doctor`)
- A hardware encoder Sunshine supports (tested: Intel Arc iGPU via VAAPI)
- The patched Sunshine, built with `sunshine/build.sh` (installs to
  `~/.local/opt/sunshine-wall`; your regular Sunshine package stays as is)
- The physical screens you include must use **100% scaling**

**Client**
- Steam Frame (SteamOS), with the **Moonlight** flatpak
  (`com.moonlight_stream.Moonlight`)
- Should also work on other gamescope/SteamOS devices and on plain Linux;
  only the Steam Frame is tested

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
   I recommend using the ip given by the 6ghz wireless dongle for the lowest latency
3. Copy `client/framewall` to the headset, e.g. `~/framewall`, and make it
   executable. Optionally copy `client/framewall.conf.example` to
   `~/.config/framewall.conf` to pick a default preset and bitrate.
4. In desktop mode, right click ~/framewall (Click thumbstick), and click "Add to Steam"

5. In Steam's settings, set **Maximum game resolution** to the smallest option
   that is at least as large as the wall: **3840×2400** for a 3840×2280 wall.
   `framewall install` prints the size to pick for each preset. Steam's default
   (1920×1080) makes Moonlight shrink the wall to half size, and a much larger
   option (e.g. 5120×2880) costs the headset GPU extra work, which can make head
   movement stutter.

Launch it from Steam. To switch presets, set `APP` in
`~/.config/framewall.conf` (`~/framewall --list` shows the names), or give
the app name as a launch option.

## Configuration

Host (`~/.config/framewall/config`, see `host/config.example`):

| Setting | Meaning |
|---|---|
| `PRESETS` | One line per wall: `"COUNT WIDTHxHEIGHT REFRESH LAYOUT"`, e.g. `"2 1920x1080 108 top"` |
| `PHYSICAL` | Physical screens in the wall: `all` (default), `primary`, a list of output names, or `none`. Screens left out keep working where they are. |
| `ENCODER` / `ADAPTER` | Sunshine encoder (`vaapi`, `nvenc`, …) and GPU render node |
| `PORT` | Sunshine base port (separate from a normal Sunshine on 47989) |
| `NAME` | Host name shown in Moonlight |

Layouts (the physical screens move as one block, keeping their arrangement):
- `top`: virtual monitors in a row above the physical screens. With two 1080p
  monitors over a 16:10 laptop the wall is close to 16:9, so it **fills a
  theater screen**.
- `row`: physical screens in the middle, virtual monitors left and right. Wide,
  so it's letterboxed on a 16:9 screen.
- `grid`: virtual monitors in rows above the physical screens, with the column
  count that makes the wall closest to 16:9.

**Multiple monitors.** All physical screens are included by default, in the
arrangement you have, with the virtual monitors added around them; your
arrangement is restored when the stream ends. Set `PHYSICAL` to include only
some of them: the others keep working where they are, and the wall is placed
to their right. Multi-monitor layouts are tested with simulated screens only.

Client (`~/.config/framewall.conf` on the headset): `HOST`, `APP`,
`BITRATE_KBPS`, `ABSOLUTE_MOUSE`.

## Tips

- **Steam's Maximum game resolution:** pick the smallest option that fits the
  wall (see Setup). Too small scales the wall down; much larger costs headset
  GPU time.
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
- `krfb-virtualmonitor` also opens a VNC port per virtual monitor (5911,
  5912, …) protected by a random password; keep those ports closed in your
  firewall.
- Don't add or remove other monitors while a wall is streaming: KWin reshuffles
  the layout and the mouse mapping goes wrong until you reconnect.

## License

GPL-3.0-or-later. The Sunshine patch is a derivative of
[Sunshine](https://github.com/LizardByte/Sunshine) (GPL-3.0).
