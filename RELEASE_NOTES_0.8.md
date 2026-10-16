# DrumGroover 0.8 alpha – first public release

> **This is an alpha version.** It works and I use it, but expect bugs and rough edges, and things may change between versions.

DrumGroover is a VST3 plugin that writes drum patterns for you and plays them with a TR-808 style drum synth. Tell it how busy, how varied and how loose the groove should be, then arrange the results into a song and drag them into your DAW.

## How it came about

It started as a small HTML tool that randomized drum patterns for Superior Drummer. Then it became a VST plugin. Since I already had a plugin, I put a drum kit on top of it: a TR-808 clone based on the circuit of the [Yocto V2.0](https://github.com/e-licktronic/Yocto-V2.0) by e-licktronic. I simulated the circuit in NGSpice, tuned it against recordings of the Yocto 2, and then rebuilt it in C++.

## What you get

- **A groove generator.** Pick density, variation, subdivision and feel, and every instrument gets its own pattern. Each instrument has its own phrase length, so kick, hi-hat and percussion can loop against each other. You can edit anything by hand in the note editor.
- **Blocks and a timeline.** Build main, break and fill blocks and put them in order on the timeline. Every clip shows a small preview of its notes.
- **Two sound engines.**
  - *808*: based on the TR-808 circuits as built in the Yocto V2.0, simulated in NGSpice and ported to C++. All instruments run in real time except the hi-hat, ride and cymbals. I couldn't get those fast enough, so they are rendered ahead of time and played from a cache.
  - *Lo-Fi*: a simpler, rougher alternative.
- **A mixer.** One channel per instrument with fader, pan, mute and solo, EQ and limiter slots, reverb and delay sends, and a master limiter. Mixer presets included.
- **MIDI export.** Save the whole song or single blocks as `.mid`, or drag clips straight from the timeline into your DAW.
- **Everything is saved in your project.** Patterns, song, engine settings and mixer come back when you reopen it.

## Install

1. Copy `DrumGroover.vst3` to `C:\Program Files\Common Files\VST3\`.
2. Rescan plugins in your DAW.

DrumGroover is built and tested for **Windows 11**. Its interface needs the Microsoft Edge WebView2 Runtime, which Windows 11 includes.

Windows 10 is not supported, but may work if the runtime is installed. If it is missing, the plugin tells you and offers the download page ([WebView2 Runtime](https://developer.microsoft.com/en-us/microsoft-edge/webview2/), "Evergreen Bootstrapper").

The first start renders the hi-hat, ride and cymbal sounds once into the cache (`%APPDATA%\DrumGroover\Cache`). That takes a few seconds. The cache size can be set in the Drum Engine tab.

## Good to know

- Windows 11, 64 bit, VST3 only for now. There are no macOS or Linux builds yet.
- Tested with the DAW the author uses. Other DAWs should work but haven't been tried.
- Alpha: expect rough edges.

Bug reports and test results from other DAWs are very welcome.
