<div align="center">

# 🥁 DrumGroover

**A drum groove generator and TR-808 style drum synth, as a VST3 plugin.**

![Version](https://img.shields.io/badge/version-0.8.1%20alpha-orange.svg)
![Platform](https://img.shields.io/badge/platform-Windows%2011-blue.svg)
![Format](https://img.shields.io/badge/format-VST3-green.svg)
![License](https://img.shields.io/badge/license-MIT-lightgrey.svg)

Built with [iPlug2](https://github.com/iPlug2/iPlug2)

</div>

It is a groove generator. A drum loop is made of several parts: a verse, a chorus, a turnaround, breaks, an intro and an outro.

It is a sequencer. You arrange the song parts in order.

It is a randomizer. You can randomize every instrument.

It is a drum VST. The 808 engine is a TR-808 clone based on the circuit of the [Yocto V2.0](https://github.com/e-licktronic/Yocto-V2.0) hardware clone.

And it plays nice with your DAW. Export the song or single parts as MIDI, or drag them straight into your project.

 ⚠️ **Alpha (first public release).** It works and I use it, but expect rough edges. Windows 11, 64 bit, VST3 only for now. The macOS code paths exist but have never been built or run; see [Status](#-status).

## 📸 Preview

| Note Editor | Song Generator |
| :---: | :---: |
| ![Note Editor](img/dg_note_editor.png) | ![Song Generator](img/dg_song_generator.png) |

| Mixer | Drum Engine |
| :---: | :---: |
| ![Mixer](img/dg_mixer.png) | ![Drum Engine](img/dg_drum_engine.png) |

## 🎛️ What it does

- **Groove generator.** Six instrument groups plus custom percussion, each with knobs for density, variation and grid. Turn a knob and the pattern regenerates; 🎲 rerolls it.
- **Edit by hand.** Click, paint and drag notes in the note editor. A group you edit is locked, so rerolling leaves it alone.
- **Blocks and timeline.** Verse, chorus, bridge, breaks, fills, intro and outro. The song generator arranges them into a full song, or you drag them onto the timeline yourself.
- **Plays with your DAW.** Follows the host transport and tempo, or plays on its own. MIDI notes play the kit.
- **Two sound engines.** A TR-808 clone based on the Yocto V2.0 circuit, and a simpler Lo-Fi engine.
- **Mixer.** Faders, pan, effects, reverb and delay, a master limiter, and auto gain.
- **MIDI export.** Export the song or single blocks as `.mid`, or drag them into your DAW.
- **Everything is saved in your project.**

<details>
<summary><b>All the details</b></summary>

- **Groove generator.** Six instrument groups (kick, snare, hi-hat, ride, toms, cymbals) plus extra percussion on custom tracks (high conga, claves, hand clap, maracas, cowbell, PC-2). Each group has its own knobs: *Subdiv* sets the grid the pattern is generated on, from 8th notes up to 64ths, *Dens* decides how many of the steps get a hit, and *Var* is the chance of an alternate articulation such as a rim shot, an open hi-hat or a ride bell. The snare also has *Ghost* for quiet notes between the main hits. The pattern regenerates while you turn a knob, and the 🎲 button rerolls a group with a new random seed. Every instrument has its own phrase length, so kick, hi-hat and percussion can loop against each other.
- **Edit by hand.** Click, paint, select and drag notes in the note editor, edit velocities in their own lane, copy and paste, and mark ranges that play triplets instead of straight notes. A group you edit by hand is locked, so rerolling and the knobs leave it alone until you hand it back.
- **Blocks and timeline.** Patterns live in blocks: verse, chorus, bridge, breaks, fills, intro and outro. Every block has its own length, feel (straight, swing or half-time), knob values and an optional turnaround fill every N bars. The song generator arranges your blocks into a complete structure such as 12 bar, AABA, ABAB, ABCABC or standard pop, or you drag the blocks onto the timeline yourself. Every clip shows a preview of its notes, and you can set a loop range.
- **Playback.** The plugin follows your DAW's transport and takes the tempo from it. It also has its own Play button. Edits are picked up while it plays. MIDI notes sent to the plugin play the matching instrument, so you can play the kit from a keyboard.
- **Two sound engines.**
  - *808*: the drum voices are models of the TR-808 circuits, based on the [Yocto V2.0](https://github.com/e-licktronic/Yocto-V2.0), simulated in NGSpice ([yocto_tr808_ngspice](https://github.com/ReinerBforartists/yocto_tr808_ngspice)) and calibrated against recordings of the Yocto 2. Kick, snare, rim shot, claves, hand clap, maracas, toms and congas, cowbell, PC-2 percussion, hi-hat, ride and cymbals. Every instrument has the knobs of its circuit (tone, decay, level and so on), some have a morph pad that blends between four presets, and the hi-hat, ride and cymbals play several cached variants in rotation so repeated hits do not sound identical.
  - *Lo-Fi*: a simpler synthesis engine without circuit controls.
- **Mixer.** One channel per instrument with fader, pan, mute and solo, a level meter and four effect slots (EQ, compressor, saturation, transient shaper or limiter). Reverb and delay buses with a send per channel, a kit gain, and a master with EQ and look-ahead limiter. *Auto gain* plays the song and sets the faders so the peaks land on a target level. Mixer presets save the whole mixer.
- **MIDI export.** Export the song as one `.mid` file, or every block as its own file. Or drag a clip or block from the timeline directly into your DAW's playlist.
- **Project state.** Patterns, song, sound settings and mixer are saved inside the host project. You can also save and load the project as a separate `.json` file.

</details>

## 💡 How it came about

It started as a small HTML tool that randomized drum patterns for Superior Drummer. Then it became a VST plugin. Since there was a plugin anyway, I put a drum kit on top of it: a TR-808 clone based on the circuit of the Yocto V2.0. I simulated the circuit in NGSpice, tuned it against recordings of the Yocto 2, and rebuilt it in C++. The NGSpice simulation is published as its own repository, [yocto_tr808_ngspice](https://github.com/ReinerBforartists/yocto_tr808_ngspice): the full circuit as one self-contained netlist, with example renders. It is the reference the C++ voices are checked against. Everything runs in real time except the hi-hat, ride and cymbals, which were too slow; they are rendered ahead of time and played from a cache.

## 📋 Requirements

- Windows 11, 64 bit, and a VST3 host. Windows 10 is not supported.
- The Microsoft Edge WebView2 Runtime (included with Windows 11; if it is missing, the plugin says so and offers the download page). The user interface is a web page shown in a WebView2 control; it is compiled into the plugin DLL, so no files besides the `.vst3` are needed.
- A CPU with AVX2 is not required. A build with `-DDGG_AVX2=ON` is faster at baking.

## 🚀 First start

Hi-hat, ride and cymbal sounds are rendered once and cached in `%APPDATA%\DrumGroover\Cache`. The first start shows a short "Preparing the sound cache" dialog. On a recent desktop CPU this takes a few seconds. The cache size limit is set in the Drum Engine tab (Cache panel).

## 🔨 Building from source

DrumGroover is built with [iPlug2](https://github.com/iPlug2/iPlug2), which CMake downloads on the first configure (unmodified, pinned to the revision DrumGroover is built against, kept out of this repository). Everything that differs from stock iPlug2 lives in this repository (`source/platform/DggWebView_win.cpp`, replacing iPlug2's Windows WebView backend via `CMakeLists.txt`).

```bash
git clone https://github.com/ReinerBforartists/Drumgroover
cd Drumgroover
```

git must be installed; the first `cmake` run needs a network connection to fetch iPlug2 into `iPlug2/`.

Get the VST3 SDK for iPlug2 as described in iPlug2's documentation (`iPlug2/Dependencies/IPlug/download-iplug-sdks.sh`, or place the SDK in `iPlug2/Dependencies/IPlug/VST3_SDK`). Then, with Visual Studio 2022 and CMake 3.16 or newer:

```bash
mkdir build
cd build
cmake -G "Visual Studio 17 2022" -A x64 ..
cmake --build . --config Release
```

The first configure downloads the WebView2 SDK and WIL (header only), so it needs internet access.

The web view's developer tools (right-click, Inspect) are off by default; switch them on with Options > Developer tools.

During development you can point the plugin to the web files on disk instead of the embedded copy: set the environment variable `DGG_WEB_ROOT` to the `resources/web` folder, then JavaScript, CSS and HTML changes take effect without a rebuild.

## 📁 Repository layout

| Path | Content |
| --- | --- |
| `source/` | Plugin class (`DrumGroover.cpp/.h`), schedulers, groove generator, MIDI export |
| `source/dsp/` | Drum voices, mixer and the sound cache |
| `source/platform/` | Windows WebView2 backend, macOS file dialogs |
| `resources/web/` | The user interface (HTML, CSS, JavaScript) |
| `cmake/` | Build helper that embeds the web files into the DLL |
| `iPlug2/` | iPlug2 framework (fetched by CMake, not part of the repository) |

The NGSpice simulation of the circuit that the 808 engine is based on is not part of this repository. It lives in [yocto_tr808_ngspice](https://github.com/ReinerBforartists/yocto_tr808_ngspice) (one netlist, a script that converts the simulation output to WAV, and example renders). You do not need it to build or use the plugin; it is the physical reference for the voice models in `source/dsp/` and the place to look if you want to see or change the circuit itself.

## 🧪 Status

- Windows 11 VST3: built and used. Alpha, expect rough edges.
- Windows only for now. macOS: the file dialogs and the WKWebView loading path are written but never built. No other plugin formats. Tested with the DAW the author uses; other DAWs should work but haven't been tried.
- Bug reports and test results from other DAWs are welcome.

## 🙏 Credits

- [Yocto V2.0](https://github.com/e-licktronic/Yocto-V2.0) by e-licktronic: the 808 engine is based on its circuit and tuned against recordings of the Yocto 2. No Yocto files are part of this repository.
- [yocto_tr808_ngspice](https://github.com/ReinerBforartists/yocto_tr808_ngspice): the NGSpice simulation of that circuit, published as a separate repository (MIT).
- [iPlug2](https://github.com/iPlug2/iPlug2) by Oliver Larkin and contributors: the plugin framework (WDL license / MIT, see the framework's own license files).
- [nlohmann/json](https://github.com/nlohmann/json): JSON parsing (MIT, comes with iPlug2).
- Microsoft Edge WebView2 SDK and WIL: the web view the user interface runs in.
- Steinberg VST3 SDK (MIT).
- [Nava](https://github.com/e-licktronic/Nava-v1.0) by e-licktronic: the TR-909 engine (in development) is based on its circuit. No Nava files are part of this repository.

All third-party components keep their own licenses.

### Third-party samples

The TR-909 engine (in development) needs waveforms for the hi-hat, crash and ride voices, which the original machine plays from a ROM. Roland's ROM data and recordings of it are **not** used. The waveforms are replaced by recordings of ordinary cymbals and hi-hats from [Freesound](https://freesound.org), all under the [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) public domain dedication, converted to the 6-bit, 30 kHz format of the original data path.

| Voice | Sound | Uploader | Licence | Source |
| --- | --- | --- | --- | --- |
| Closed hi-hat | "new 13 hat closed" (Freesound 112491, 12" Paiste hi-hat from the pack "John Bonham's Kit") | cdiupe | CC0 1.0 | <https://freesound.org/people/cdiupe/sounds/112491/> |

This list covers the samples chosen so far; open hi-hat, crash and ride will be added here with the same information once their sources are fixed and their licences checked on the Freesound page.

## 📜 License

DrumGroover is released under the [MIT License](LICENSE).
