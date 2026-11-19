framewall-vr @VERSION@ for the Steam Frame (SteamOS, aarch64)
===========================================================

The headset side of framewall: shows the wall streamed from your PC as
separate curved panels side by side around you. The PC side (patched
Sunshine and the `framewall` host script) is set up from the repository:

    https://github.com/TylerK26/framewall

Install (Desktop mode Konsole, or SSH):

    tar xf framewall-vr-@VERSION@-steamos-aarch64.tar.gz
    ./framewall-vr-@VERSION@-steamos-aarch64/install.sh

That installs to ~/.local/opt/framewall-vr and puts the launcher at
~/framewall-vr. In Desktop mode, right click ~/framewall-vr, click
"Add to Steam", then launch it from Steam. SteamVR starts new apps at
72 Hz: set framewall-vr's refresh rate in SteamVR's video settings.

Before the first start, pair the headset with the host once using the
Moonlight flatpak (com.moonlight_stream.Moonlight); framewall-vr reuses
that pairing. See the repository's README for the full setup, controls
and settings (~/.config/framewall-vr.conf; framewall-vr.conf.example in
this folder documents it).

Update: extract a newer release and run its install.sh. Settings are kept.

Contents
  framewall-vr            the app
  libopenxr_loader.so.1   Khronos OpenXR loader (finds SteamVR's runtime)
  framewall-vr-launcher   launcher template (install.sh writes ~/framewall-vr)
  install.sh              installer
  LICENSES/               licenses of framewall and the bundled code
  SOURCES.txt             exact source versions of this build

framewall is GPL-3.0-or-later. The source of this build is in the
repository at the commit listed in SOURCES.txt.
