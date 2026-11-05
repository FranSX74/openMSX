# MIDI export, MIDI Tap and built-in synthesizer

openMSX can turn what the PSG, the SCC and the MSX-Music play into MIDI in two ways: it can record the music into a Standard MIDI File (the `midi_record` command), and it can send it to a synthesizer or a MIDI program while the game runs (the `MIDI_Tap` extension). A built-in synthesizer (`midi-out-synth`) can play that MIDI inside openMSX, so no external synthesizer is needed.

## What it does

While recording, openMSX logs every write to the sound chip registers together
with its exact emulation time. When you stop, the log is converted to MIDI. The
conversion is done offline, so it can look ahead and clean up the result.

| Chip | Channels | MIDI result |
|---|---|---|
| PSG (AY-3-8910 / YM2149) | 3 tone + noise | 3 melodic tracks, noise as GM percussion |
| SCC / SCC+ (Konami) | 5 | 5 melodic tracks, GM program chosen from the wave form |
| MSX-Music (YM2413 / OPLL) | 9 (or 6 + 5 rhythm) | 9 melodic tracks + a rhythm track as GM percussion |

## Using it

**Console / script**

```
midi_record start              ;# automatic name: ~/.openMSX/midi/openmsx0001.mid
midi_record start my_song.mid  ;# explicit name (relative names go in the midi folder)
midi_record stop               ;# stop and write the file(s)
midi_record toggle             ;# start (automatic name) or stop
midi_record status
midi_record detail full|simple ;# full (default): as the chips; simple: fewer, longer notes
midi_record percussion on|off  ;# detect falling-tone drums and write them as drum hits (off)
midi_record arpeggio on|off    ;# arpeggios as chords (default) or as fast notes
midi_record volume psg|scc|opll 0-127  ;# channel volume of each chip (defaults 85, 120, 100)
midi_record live raw|clean     ;# real-time output only, see "Real-time output"
```

Every setting command without its value prints the current value.

**GUI:** *Tools → Capture → MIDI (PSG)...* opens a small window with a name
field, Start/Stop and "Open MIDI folder". If the file exists you are asked
before overwriting.

**Key binding:** `ALT+F6` runs `midi_record toggle`. It is defined in the new
script `share/scripts/midi_record.tcl`. If you keep your own copy of the openMSX
scripts in `~/.openMSX/share/scripts`, copy that file there too.

Tip: start the recording a bit before the music begins, and stop after it
ends. Many games take several seconds before the music starts.

## Output files

A Standard MIDI File, **format 1**, 500 ticks per quarter note at 120 bpm
(exactly 1 ms per tick).

- Track 0: tempo / time signature.
- One track per chip channel that was actually used: `PSG A`, `PSG B`, `PSG C`,
  `SCC 1`...`SCC 5`, `MSX-Music 1`...`MSX-Music 9`.
- `PSG noise` and `MSX-Music rhythm`: separate tracks, both on the General MIDI
  percussion channel (10).
- MIDI channels are assigned at the end, only to voices that played, skipping
  channel 10. The order is always PSG, SCC, MSX-Music.
- If more than 15 melodic voices were used, the output is **split in two
  files**: `name.mid` (PSG + SCC) and `name-msxmusic.mid` (MSX-Music).
- Silent tracks are not written.

## How the conversion works

The chip state is sampled every 5 ms from the logged register writes.

- **Pitch** is computed from the chip's real period/frequency registers
  and clock. The nearest MIDI note is played, the fraction goes to **pitch
  bend** (range ±2 semitones, configured with RPN 0).
- **Vibrato and slides**: a deviation of up to 0.75 semitones from the sounding
  note is only pitch bend, never a new note.
- **Note changes** within ±2 semitones must be stable for 60 ms before they
  become a new note; bigger jumps (arpeggios, sound effects) are immediate.
- **Volume** becomes CC#11 (expression) and note-on velocity.
- **PSG hardware envelope** is evaluated exactly (all 16 shapes). Very fast
  repeating envelopes are rendered with a constant volume. When the tone and
  noise are off, the envelope repetition rate is the pitch (buzzer bass) and
  it is written as a Synth Bass note.
- **Repeated notes:** when the volume of a sounding voice jumps up by 4 or more
  levels (out of 15) the game is attacking the same note again, so a new note
  is written instead of a volume change.
  The same goes for a flutter: a game that alternates the volume every frame
  (down 2+ levels, up 2+ levels) is heard as very fast repeated notes, and
  that is what is written.
- **Pitch-drop drums** (optional, off by default): many games make a snare or
  bass drum with a tone that falls fast (3+ notes of at most 40 ms, dropping 6+
  semitones), on the PSG and on the SCC (which then gets a *drums* track). With `midi_record percussion on` (or the *Detect percussion*
  checkbox) they are written as a drum hit on the drum channel (snare if the run
  starts at note 55 or higher, bass drum below) instead of melody notes.
  Arpeggios are converted to chords first. The guess can be wrong, which is why
  it is optional. In `simple` detail, with this off, those short notes are
  dropped like any note under 60 ms.
- **SCC deformation register:** the 4-bit and 8-bit frequency modes change the
  pitch the chip really plays, and are followed (including a read of the
  register, which acts as a write of 0xFF). The wave rotation modes cannot be
  represented in MIDI and are ignored.
- **Glitches:** notes shorter than 10 ms are dropped. The chips are updated
  at most every 20 ms, so those come from reading a period between the writes
  of its two bytes.
- **Detail.** `full` (default) writes everything above. `simple` is meant for
  tracks that are easier to read and edit: no repeated notes from volume
  attacks or flutter (the volume stays a volume change), no pitch bend, no
  notes shorter than 60 ms, and arpeggios are always chords. It is also a
  selector in the *Capture MIDI* window.
- **Arpeggios** (runs of at most 60 ms notes cycling through 2-4 pitches: at least
  4 notes for 2 pitches, and always two full cycles) are written as one sustained chord, because a chip voice that
  cycles through a chord every frame is heard as that chord. Slow patterns and
  sweeps are left alone. `midi_record arpeggio off` writes every note instead.
- **Channel volume (CC#7)** is set once per voice, per chip: PSG 85, SCC 120,
  MSX-Music 100. The SCC waves are quieter than the PSG squares and the drums of
  a General MIDI sound font, so the SCC gets more. Change it with
  `midi_record volume` (in the real-time output the open channels are updated at
  once). The best balance also depends on the instruments your synthesizer
  plays on each channel.
- **PSG noise** becomes GM drums in 6 bands (hi-hats, snare, toms, bass drum).
- **MSX-Music**: the 15 built-in instruments are mapped to General MIDI
  programs, key-on edges are tracked so a quick off/on of the same note is a new
  note, and the rhythm mode drums go to the percussion channel.
  The pitch that is heard is not always the F-number frequency: the spectrum of
  the two operators consists of multiples of the greatest common divisor of
  their MULT values, so the synthesizer bass sounds an octave lower. This is
  applied to the built-in instruments and to the user patch (a user patch with a
  very quiet modulator takes the pitch of the carrier alone).
- **SCC**: the 32-sample wave form is analysed (square, saw, sine/triangle,
  other) to pick a GM program. The volume is scaled by the wave amplitude.
  The three SCC modes (real, compatible, SCC+) are handled.

## Known limitations

- The built-in vibrato of the YM2413 is not visible in the registers, so it is
  not in the MIDI. The user-defined patch of the YM2413 plays as Square Lead (it
  is used by games for sound effects).
- The rotation modes of the SCC deformation register cannot be written in MIDI
  and are ignored.
- The YM2413 rhythm "snare" is a General MIDI snare, even if a game uses it as a
  quiet hi-hat. The instruments of the MSX-Music are respected on purpose.
- PSG tones driven faster than the 5 ms grid (some digital effects) are lost.
- Only one machine is recorded at a time (the current one).
- It records what is *written to the chips*, not the final mix: it does not
  work for samples (PCM / DAC tricks), only for the FM/PSG/SCC voices.

## Playing the files

Any General MIDI player works. On Linux, for instance:

```
fluidsynth -a pipewire /usr/share/soundfonts/FluidR3_GM.sf2 song.mid
```

Good editors to inspect the tracks: Rosegarden, MuseScore, Ardour, LMMS.

## Real-time output (MIDI Tap)

The `MIDI_Tap` extension sends the music to a MIDI out connector **while the
game runs**, so you can hear it on a synthesizer, record it in a sequencer, or
route it to any MIDI program. It converts PSG, SCC and MSX-Music with the same
rules as the export.

**Quick start on Linux** (with [FluidSynth](https://www.fluidsynth.org) or
[QSynth](https://qsynth.sourceforge.io) running, any soundfont):

```
openmsx -machine msx2 -ext MIDI_Tap -cart game.rom
```

then in the openMSX console (F10):

```
plug MIDI-Tap-out <TAB>                    ;# lists the MIDI ports you can plug
plug MIDI-Tap-out "FLUID Synth (12345)"    ;# e.g. the name of your synthesizer
set master_volume 0                        ;# optional: mute openMSX, hear only the synth
midi_record live clean                     ;# optional: cleaner output (see below)
```

**On Windows** the MIDI ports are called `midi-out` (the MIDI mapper),
`midi-out-0`, `midi-out-1`... in the order Windows lists them; the description
of each one is the name of the device (with `_` instead of spaces). To hear it
on the synthesizer that comes with Windows:

```
machine_info pluggable midi-out-0            ;# Microsoft_GS_Wavetable_Synth
plug MIDI-Tap-out midi-out-0
```

The ports are listed when the machine starts: a virtual port created later
(for instance with [loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html),
to send the music to a sequencer) is only seen by a machine started after it.

**On macOS** the ports are the CoreMIDI destinations, named like the device
with ` OUT` at the end. macOS has no synthesizer that shows up as one, so start
one first, for instance FluidSynth with a SoundFont:

```
fluidsynth -is -m coremidi -a coreaudio MySoundFont.sf2
```

```
machine_info pluggable                     ;# look for "FluidSynth virtual port (...) OUT"
plug MIDI-Tap-out "FluidSynth virtual port (1234) OUT"
```

`plug MIDI-Tap-out midi-out-logger` writes the raw MIDI bytes to the file of the
setting `midi-out-logfilename` instead (useful to test, not to listen).
The extension is in `share/extensions/MIDI_Tap.xml`; if you keep your own copy
of the openMSX extensions in `~/.openMSX/share/extensions`, copy it there too.

**Which MIDI channel is what.** Each chip gets a block of channels (one per
voice) when one of its voices plays its first note, skipping channel 10, which
is always the drums (PSG noise and the MSX-Music rhythm). A chip that is only
initialized (at reset, or by a game that does not use it) gets no channels.
Example: a game with PSG and SCC uses channels 1-3 for the PSG, 4-8 for the
SCC, and 10 for the drums. Voices beyond 15 are not sent. Each channel gets its
program, pan and volume when it is opened.

**Two modes** (`midi_record live raw|clean`):

| | `raw` (default) | `clean` |
|---|---|---|
| Delay | none | 0.3 s |
| Glitch notes (< 10 ms) | sent | removed |
| Arpeggios | many fast notes | one chord (like the export) |
| Good for | playing along, lowest latency | listening, recording in a sequencer |

`clean` looks 0.3 s into the past to decide, which is why it has a delay. If you
record the output in a sequencer, shift the recording 0.3 s earlier. The
setting `midi_record arpeggio off` also applies to `clean`. Percussion detection
and the `simple` detail level are only for the export.

**Volumes.** Same as in the export: `midi_record volume psg|scc|opll 0-127`,
applied at once to the open channels.

**Silence at the end.** When you close openMSX, reset or power off the MSX, or
load a state, "All Sound Off" and "All Notes Off" are sent on all 16 channels,
so no note is left hanging in the synthesizer.

**Savestates and rewind.** They work: the registers of the chips and the MIDI
channels are saved, and after a load or a `reverse goback` the synthesizer is
silenced and the voices start their notes again from where the music is.
The notes that were sounding start again (you hear them re-attacked).
Remember that `reverse goback` leaves the emulation paused (`set pause off`).
The `midi-out-logger` does not reopen its file after a load, so it is not
suitable to test this; a real MIDI port is.

**Limitations.** Tested on Linux with ALSA (FluidSynth/QSynth), on Windows
(Microsoft GS Wavetable Synth, and loopMIDI to check the bytes) and on macOS
(CoreMIDI with FluidSynth; macOS has no built-in synthesizer that shows up as
a MIDI out port, so you need one, e.g. FluidSynth with a SoundFont). The notes
are sent when the emulation produces them, so their timing follows the
emulation, which runs in short bursts (on Windows the notes were measured
within ±25 ms of their time). If a game plays digital samples, they are not in
the MIDI. The SCC has no drum track (its percussion stays as notes).

## Built-in synthesizer (midi-out-synth)

You do not need an external synthesizer to hear the MIDI. openMSX has a MIDI
out device that plays it with a SoundFont (an `.sf2` file) and mixes the result
with the rest of the sound of the emulated machine:

```
set midi-out-synth-soundfont ~/Music/SF2/GeneralUser-GS.sf2
plug MIDI-Tap-out midi-out-synth
```

It is a MIDI out device like the ones for ALSA, CoreMIDI or Windows, so it can
be plugged into any MIDI out connector: the `MIDI-Tap-out` of the MIDI Tap, or
the one of an MSX-MIDI cartridge. The MIDI messages act at their exact emulation
time, so the music stays in sync with the machine (also with `reverse`, which
loads the SoundFont again). The notes that sound are not saved in a savestate:
after a load the synthesizer starts silent and the notes begin again.

Things to know:

- **The SoundFont** is not included. A small General MIDI / GS one works well:
  [GeneralUser GS](https://github.com/mrbumpy409/GeneralUser-GS) (about 30 MB;
  see its license). Any `.sf2` file can be used. The file is read when the
  device is plugged.
- **Volume.** The synthesizer is a sound device of the mixer, called
  `MIDI-synth`: its volume is the setting `MIDI-synth_volume`, and its sound can
  be recorded or muted like the other devices. To hear only the synthesizer and
  not the original PSG, set `PSG_volume 0`.
- **What is supported.** Notes, program and bank select, volume, pan,
  expression, sustain, pitch bend and its range, all sound off / all notes off
  and "General MIDI System On". Channel 10 plays the drum kits. It has no reverb
  or chorus, and no SoundFont modulators (the instruments sound a bit drier than
  in a full synthesizer such as FluidSynth).
- **How it is made.** The synthesizer is
  [TinySoundFont](https://github.com/schellingb/TinySoundFont) by Bernhard
  Schelling (MIT license), a single header file in `src/3rdparty/TinySoundFont`.

## Credits

The idea comes from the MIDI export of fMSX (Marat Fayzullin). The conversion
rules (vibrato as pitch bend, stability filter before changing note, noise bands
to drums) were inspired by the *MIDI-PAC* firmware of the
[MSX PicoVerse](https://github.com/cristianoag/msx-picoverse-public) project.
No code was copied from either project.
