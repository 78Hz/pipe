# PIPE

A physically modelled pipe organ plugin (CLAP, VST3, LV2, and AU on macOS) for Windows, macOS and Linux.

Every sound comes from a model of a real pipe: an air jet striking the upper lip of a flue pipe, or a brass tongue beating in a reed boot, feeding a resonator of the right length and width. Each pipe's dimensions are worked out from its voicing (scale, mouth, cut-up, toe pressure), and the plugin checks that the pipe could actually be built.

## Download

Get the zip for your system from the [Releases page](https://github.com/78Hz/pipe/releases) (or the latest build under *Actions → Build plugins → Artifacts*).

| System | Copy these | To |
|---|---|---|
| Windows | `Pipe.clap` | `C:\Program Files\Common Files\CLAP\` |
| | `Pipe.vst3` | `C:\Program Files\Common Files\VST3\` |
| | `Pipe.lv2` | `%APPDATA%\LV2\` |
| macOS | `Pipe.clap` | `~/Library/Audio/Plug-Ins/CLAP/` |
| | `Pipe.vst3` | `~/Library/Audio/Plug-Ins/VST3/` |
| | `Pipe.component` (AU) | `~/Library/Audio/Plug-Ins/Components/` |
| | `Pipe.lv2` | `~/Library/Audio/Plug-Ins/LV2/` |
| Linux | `Pipe.clap` | `~/.clap/` |
| | `Pipe.vst3` | `~/.vst3/` |
| | `Pipe.lv2` | `~/.lv2/` |

**macOS:** the builds are not notarised. After copying, clear the download quarantine once in Terminal:

```
xattr -dr com.apple.quarantine ~/Library/Audio/Plug-Ins/*/Pipe.*
```

## The organ

Three divisions, 27 ranks:

- **Pedal:** Contrabass 16', Subbass 16', Octavbass 8', Gedacktbass 8', Choralbass 4', Posaune 16'
- **Great:** Bourdon 16', Principal 8', Rohrflöte 8', Octave 4', Spitzflöte 4', Quinte 2⅔', Superoctave 2', Mixtur IV, Trompete 8'
- **Swell (enclosed):** Gedackt 8', Salicional 8', Voix céleste 8', Principal 4', Flûte harmonique 4', Nasard 2⅔', Doublette 2', Tierce 1⅗', Hautbois 8', Tremulant

Couplers: Swell to Great, Great to Pedal, Swell to Pedal. Drawing a stop while keys are held makes those pipes speak, as on a slider chest.

**Wind:** a blower feeds a reservoir and a regulator for each chest. Big chords draw wind, so the pressure sags and bounces. Flexibility, concussion bellows and the tremulant shape this.

## Playing with MPE and polyphonic aftertouch

No particular controller is assumed. The Expression screen shows what each MPE zone is actually sending, the live values of the last note, and where each mechanism is right now.

Expression runs through a **modulation matrix** of 10 rows. Each row takes a source (strike velocity, MPE pressure, poly aftertouch, CC74, per-note bend, release velocity, or any CC) from one zone or both. It picks a part of the source's travel, shapes it with a curve and smoothing, and sends it with an amount of ±1 to something an organ builder could actually make:

| Destination | Mechanism |
|---|---|
| Pallet open | a proportional pallet valve under the pipe |
| Wind boost | extra wind at the pipe foot, up to the *boost ceiling* (flues overblow above 100 mm) |
| Jet aim | the jet's aim at the upper lip, from breathy to bright |
| Pitch | a sliding stopper or mouth shade, limited per pipe family |
| Chiff, pallet speed, close speed | how the valve opens and closes |
| Swell box, tremulant depth / speed | global: follow a CC or the last note played |

Rows aimed at the same destination add together. If a note sends no data for a row, that row leaves the voiced default alone, so a plain keyboard still plays at the voiced 78 mm. Click a cell to change it, drag the amount, or press **LEARN** and move a control.

The defaults suit an Osmose in MPE mode and most other MPE controllers:

- The first stage of key travel (MPE pressure) opens the pallet; it's fully open at 90 %.
- The aftertouch stage (CC74 in the lower zone) and poly aftertouch boost the wind.
- Bend moves the pitch. CC74 in the upper zone aims the jet, and CC11 drives the swell box.

Use the Osmose's MIDI port 1 (firmware 2.3 numbering) in MPE mode.

Turn off *Strict physical limits* to use your controller's full bend range.

**Routing:** a DAW gives a plugin a single MIDI stream, so divisions are chosen by MPE zone:

- The **lower zone** plays the Great.
- The **upper zone** plays the Swell.
- An optional **pedal split key** sends low notes to the Pedal.

The zones follow the MPE Configuration Message your controller sends. To play two controllers into one instance, put one in the lower zone and one in the upper zone. If a controller can't change zones, use a separate instance for each.

## The building

The Room screen offers two kinds of room:

- **Measured rooms (convolution):** St Andrew's Lyndhurst, St Margaret's York, York Minster, and the Jack Lyons Concert Hall. These are first-order B-format impulse responses from OpenAIR. Rooms measured at several seats choose the seat nearest your *distance*.
- **Lite rooms (algorithmic):** village church, abbey nave, cathedral, concert hall, and dry. They use far less CPU.

Microphones can be AB (a spaced pair), ORTF, or binaural for headphones (a fixed head, no head tracking).

**Your own impulse responses:** choose CUSTOM IR, then LOAD IR FILE to load a WAV file. It can be mono, stereo, or 4-channel B-format; pick FuMa or AmbiX ordering for B-format. By default the recording's own direct sound is cut and the organ model supplies it. Untick *direct sound from the model* to hear the file exactly as recorded. The file's path is saved with the project and presets, not the audio, so keep the file where it is.

The convolution adds no latency. It runs on a background thread, so changing rooms takes a moment and then crossfades.

## Tuning

- **Historical:** 12-TET, Werckmeister III, ¼-comma meantone, Kirnberger III, Vallotti
- **Equal divisions:** 19, 22, 24, 31, 41 and 53-EDO, plus any custom 5–72-EDO
- **Scala:** `.scl` scales with optional `.kbm` keyboard maps; the file contents are saved with the preset

There are three key mappings, set per zone:

- **Subset:** each key plays one pitch of the chain of fifths. Hold CC66, or tick *flats instead of sharps*, to swap C♯/D♭ and the other pairs.
- **Sequential:** each key plays the next step, which suits grid controllers.
- **Scala KBM:** the loaded keyboard map decides.

Every pipe can be retuned individually in cents. Pipe temperature (flue pipes go sharp as they warm; reeds barely move) and a random age drift are also available. Mutations and mixture ranks are tuned pure against their key, as on real organs.

## Presets

- **Factory presets:** 12, from Plein Jeu to 31-EDO and expressive MPE setups.
- **User presets:** saved as text files (`.pipe`) in `Documents/Pipe Presets`. When saving, tick which parts to include: stops, voicing, tuning, wind, room, expression, routing.
- **Combinations 1–7:** store stop settings only, for quick changes while playing.
- **Host project:** the full state is also saved with your DAW project.

## Building from source

```
git clone --recursive https://github.com/78Hz/pipe
cd pipe
python3 tools/prepare_irs.py --out build/irs     # downloads the measured rooms (needs numpy, scipy)
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DPIPE_IR_DIR=$PWD/build/irs
cmake --build build --config Release
./build/pipe_render_test        # offline engine checks
```

Without `PIPE_IR_DIR`, the build has only the lite rooms. `--synthetic` makes stand-in rooms for offline testing.

On Linux, install `libgl1-mesa-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libdbus-1-dev` first. Built plugins land in `build/bin`.

`tools/calibrate.cpp` regenerates `src/engine/CalibrationTables.hpp`. That table holds the pitch each pipe model actually sounds at its voiced point, which is how pipes stay in tune while still bending in pitch with wind pressure. Rerun it whenever a model constant in `PipeModels.hpp` changes.

## Credits

- Built with [DPF](https://github.com/DISTRHO/DPF) (ISC licence) and [PFFFT](https://github.com/marton78/pffft) (BSD licence).
- Measured rooms: impulse responses from the [OpenAIR library](https://www.openair.hosted.york.ac.uk/), Audiolab, University of York, licensed [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). St Andrew's Church, Lyndhurst, and York Minster were recorded by Damian T. Murphy. St Margaret's Church, York (National Centre for Early Music) was recorded by Aglaia Foteinou and Simon Shelley. The Jack Lyons Concert Hall is at the University of York. The responses are converted to horizontal B-format at 48 kHz, trimmed, and normalised.
- The interface font is VT323 by Peter Hull, under the SIL Open Font License (`resources/OFL-VT323.txt`).
- The reed resonator follows Perry Cook's waveguide approach.
- The 31-tone organ precedent is Adriaan Fokker's instrument of 1950.
