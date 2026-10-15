# DrumGroover 0.8.0

First public release: a drum groove generator and TR-808 style drum synthesizer as a VST3 instrument.

## Highlights

- Groove generator with per-instrument phrase length, editable in the note editor
- Blocks and a timeline to arrange a song, with note previews in the clips
- Two engines: 808 (models of the TR-808 circuits) and Lo-Fi
- Mixer with EQ and limiter slots, reverb/delay buses and a look-ahead master limiter
- MIDI export of the song or single blocks, and drag-out of clips into the DAW
- Everything is saved in the host project

## Install

Copy `DrumGroover.vst3` to `C:\Program Files\Common Files\VST3\` and rescan plugins in your host.
Requires the Microsoft Edge WebView2 Runtime (included with current Windows 11).

On the first start the plugin renders its hi-hat, ride and cymbal sounds once into `%APPDATA%\DrumGroover\Cache` (a few seconds).

## Tested / not tested

- Tested: Windows 11, VST3.
- Not tested: macOS, other hosts, machines without the WebView2 Runtime.

Feedback and bug reports are welcome.
