# Muxella virtual audio

Fork of https://github.com/tarekwasfy01/Virtual-Cables at commit
8a819c29971d30a73955178ac8a3aac00b1a5230. Original BSD-2-Clause license
and copyright are retained in LICENSE.txt and distributed with Muxella.

Six local USB Audio Class 1.0 device pairs: Muxella Main, Spotify, Discord,
and Muxella Out B1/B2/B3. Playback is forwarded to the matching recording
endpoint. The host defaults to lossless packed 24-bit stereo PCM at 48 kHz;
`--bits 16`, `--bits 24`, and `--bits 32` select the device format before
enumeration. There are no paid feature gates. Restart/re-attach devices after
changing depth; playback and capture must use the same format.
The Muxella menu exposes all three depths and remembers the chosen depth.

Muxella starts the host automatically and closes it on exit. B buses select
playback endpoints; other programs select the corresponding recording endpoint
as their microphone. Main/Spotify/Discord application outputs go to the named
playback devices and Muxella receives their recording devices.

The Microsoft-signed usbip-win2 transport is still required. Use the virtual
audio menu in Muxella to download/install the official current x64 release.
Versions older than 0.9.8.0 are refused because of the upstream memory-corruption
issue. After upgrading an already installed transport, use Reconnect virtual
cables in Muxella's menu (or restart Muxella).
The helper does not install a custom audio driver or enable Windows test mode.
Windows supplies usbaudio.sys. Driver installation and device attachment use
Windows UAC; the mixer and cable audio server stay unelevated.

The local server uses 127.0.0.1:3241 to avoid other USB/IP servers. Cleanup only
detaches that server's ports. USB/IP sessions are closed during shutdown.
Do not run the upstream application on this port simultaneously.

Build: `go build -trimpath -ldflags="-H windowsgui" -o ../../build/muxella_cables.exe ./cmd/virtualcables`
from virtual_audio/src. No test or audio validation has been run for this port.
