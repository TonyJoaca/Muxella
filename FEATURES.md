# Mixer controls and recording

Each input strip and output bus has an **ENABLED / DISABLED** button below its
device selector. Disabled channels stop their WASAPI stream and skip resampling,
effects, metering, and routing work. Their device choice, effects, levels, routes,
and mute state are retained, and the enabled state is saved with settings.
Re-enabling reconnects only the selected device. A missing device remains idle.
Disabled channels produce silence in any active recording, preserving timing.
The shared virtual-cable service remains available to Windows applications.

The mixer also skips DSP on disconnected inputs and buses with no active routes
or consumers. A bus selected for recording is kept available even without an
output device. Connected outputs receive silence when idle, keeping their
streams stable.

Meter polling runs at roughly 30 Hz while the mixer is visible and 2 Hz while
hidden or minimized. Hidden meters are not updated. The terminal retains the
latest 2,000 lines and does not keep an undo history.

The footer and audio health panel show combined process CPU and RAM working
sets for the GUI, engine, and cable host/broker. Sampling runs in the background
every 2 seconds while visible and every 5 seconds while hidden. CPU is normalized
to total logical processor capacity. Shared memory pages can be counted more
than once, and a marker identifies inaccessible process counters. Windows audio
services and driver work outside those processes are not included.

Gain and pan changes ramp over 20 ms. Mute, solo, and route changes ramp over
10 ms. At a steady setting, audio uses the selected gain without additional
processing from the transition ramps.

**APPS** opens Windows playback-session controls. Each row has volume and mute,
with its playback device displayed. Apps with multiple sessions may have
multiple rows. Start playback if an app has not created a session yet.
Exclusive-mode playback may bypass session volume controls.

**RECORD** opens WAV recording. Choose any combination of input strips and
output buses and a destination folder, then start. Each session gets a new
timestamped folder with a separate stereo 32-bit float WAV for every selected
track, using the engine sample rate. Inputs are recorded after strip effects,
gain, mute, and solo; buses are recorded after routing and bus processing.

Closing the recording dialog keeps recording. Stop and save finalizes the WAV
headers. Exiting Muxella or restarting the engine also finishes the recording.
Recordings never start automatically when loading settings.

Recording writes happen on a separate disk thread. If the disk falls behind
and the recording buffers fill, complete blocks are dropped and replaced with
silence so the tracks keep the same timing. The dialog reports dropped frames
and write errors. Files larger than 4 GB use RF64 headers; the editor importing
them must support RF64.
