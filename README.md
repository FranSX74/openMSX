# openmsx-midiexport

**openMSX with a built-in MIDI exporter for the PSG, the SCC and the MSX-Music.**
**openMSX con un exportador MIDI integrado para el PSG, el SCC y el MSX-Music.**

This is [openMSX](https://openmsx.org) (branch `midi-export`, based on the
official `master`) plus one feature: record what the emulated sound chips play
and save it as a Standard MIDI File, one track per chip channel. Years ago fMSX
could do this; this brings it back to openMSX, with better note detection.

*(Español más abajo / [jump to the Spanish section](#español).)*

- [English](#english)
- [Español](#español)

---

## English

### What it does

While recording, openMSX logs every write to the sound chip registers together
with its exact emulation time. When you stop, the log is converted to MIDI. The
conversion is done offline, so it can look ahead and clean up the result.

| Chip | Channels | MIDI result |
|---|---|---|
| PSG (AY-3-8910 / YM2149) | 3 tone + noise | 3 melodic tracks, noise as GM percussion |
| SCC / SCC+ (Konami) | 5 | 5 melodic tracks, GM program chosen from the wave form |
| MSX-Music (YM2413 / OPLL) | 9 (or 6 + 5 rhythm) | 9 melodic tracks + a rhythm track as GM percussion |

### Using it

**Console / script**

```
midi_record start              ;# automatic name: ~/.openMSX/midi/openmsx0001.mid
midi_record start my_song.mid  ;# explicit name (relative names go in the midi folder)
midi_record stop               ;# stop and write the file(s)
midi_record toggle             ;# start (automatic name) or stop
midi_record status
midi_record arpeggio on|off    ;# arpeggios as chords (default) or as fast notes
```

**GUI:** *Tools → Capture → MIDI (PSG)...* opens a small window with a name
field, Start/Stop and "Open MIDI folder". If the file exists you are asked
before overwriting.

**Key binding:** `ALT+F6` runs `midi_record toggle`. It is defined in the new
script `share/scripts/midi_record.tcl`. If you keep your own copy of the openMSX
scripts in `~/.openMSX/share/scripts`, copy that file there too.

Tip: start the recording a bit before the music begins, and stop after it
ends. Many games take several seconds before the music starts.

### Output files

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

### How the conversion works

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
- **Arpeggios** (runs of at most 60 ms notes cycling through 2-4 pitches, at
  least 6 notes) are written as one sustained chord, because a chip voice that
  cycles through a chord every frame is heard as that chord. Slow patterns and
  sweeps are left alone. `midi_record arpeggio off` writes every note instead.
- **PSG noise** becomes GM drums in 6 bands (hi-hats, snare, toms, bass drum).
- **MSX-Music**: the 15 built-in instruments are mapped to General MIDI
  programs, key-on edges are tracked so a quick off/on of the same note is a new
  note, and the rhythm mode drums go to the percussion channel.
- **SCC**: the 32-sample wave form is analysed (square, saw, sine/triangle,
  other) to pick a GM program. The volume is scaled by the wave amplitude.
  The three SCC modes (real, compatible, SCC+) are handled.

### Known limitations

- The built-in vibrato of the YM2413 is not visible in the registers, so it is
  not in the MIDI. The OPLL carrier multiplier and the user-defined patch are
  not translated (the user patch plays as Square Lead).
- The SCC deformation register (rotate / reduced frequency) is ignored.
- PSG tones driven faster than the 5 ms grid (some digital effects) are lost.
- Only one machine is recorded at a time (the current one).
- It records what is *written to the chips*, not the final mix: it does not
  work for samples (PCM / DAC tricks), only for the FM/PSG/SCC voices.

### Building

This is a normal openMSX tree; see the original [`README`](README) and the
openMSX documentation.

- **Linux:** `make` (or `meson`) as usual. Nothing new is required. This is
  the platform where it was developed and tested. It should also build on
  macOS, but I have not tried.
- **Windows:** the Visual Studio project files (`build/msvc/openmsx.vcxproj`
  and `.filters`) include the new `sound/MidiRecorder.{cc,hh}`, and the code
  uses only portable C++ (`std::numbers::pi`, `FileOperations::openOfStream`).
  I have **not** been able to build it on Windows myself; if you do, please
  report any problem.

### Playing the files

Any General MIDI player works. On Linux, for instance:

```
fluidsynth -a pipewire /usr/share/soundfonts/FluidR3_GM.sf2 song.mid
```

Good editors to inspect the tracks: Rosegarden, MuseScore, Ardour, LMMS.

### Tested with

Circus Charlie and Dragon Quest (PSG), Aleste (PSG + MSX-Music) and
Gradius 2 (PSG + SCC), on C-BIOS machines.

### Credits and license

openMSX is © the openMSX developers and licensed under the GNU GPL v2; this
repository keeps that license (see [`doc/GPL.txt`](doc/GPL.txt)). The new code is released under the same
license.

The idea comes from the MIDI export of fMSX (Marat Fayzullin). The conversion
rules (vibrato as pitch bend, stability filter before changing note, noise bands
to drums) were inspired by the *MIDI-PAC* firmware of the
[MSX PicoVerse](https://github.com/cristianoag/msx-picoverse-public) project.
No code was copied from either project (the PicoVerse code is under a
non-commercial license that is incompatible with the GPL); the parameters and
the code here are my own work.

---

## Español

### Qué hace

Mientras grabas, openMSX apunta cada escritura a los registros de los chips de
sonido junto con su instante exacto de emulación. Al parar, convierte ese
registro a MIDI. La conversión se hace al final, así que puede mirar hacia
delante y limpiar el resultado.

| Chip | Canales | Resultado MIDI |
|---|---|---|
| PSG (AY-3-8910 / YM2149) | 3 de tono + ruido | 3 pistas melódicas, el ruido como percusión GM |
| SCC / SCC+ (Konami) | 5 | 5 pistas melódicas, el programa GM sale de la forma de onda |
| MSX-Music (YM2413 / OPLL) | 9 (o 6 + 5 de ritmo) | 9 pistas melódicas + una de ritmo como percusión GM |

### Cómo se usa

**Consola / script**

```
midi_record start              ;# nombre automático: ~/.openMSX/midi/openmsx0001.mid
midi_record start mi_cancion.mid  ;# nombre propio (los relativos van a la carpeta midi)
midi_record stop               ;# parar y escribir el/los archivo(s)
midi_record toggle             ;# empezar (nombre automático) o parar
midi_record status
midi_record arpeggio on|off    ;# arpegios como acordes (por defecto) o como notas rápidas
```

**Interfaz:** *Tools → Capture → MIDI (PSG)...* abre una ventana con el nombre,
Start/Stop y "Open MIDI folder". Si el archivo existe, pregunta antes de
sobrescribir.

**Atajo de teclado:** `ALT+F6` ejecuta `midi_record toggle`. Está en el script
nuevo `share/scripts/midi_record.tcl`. Si tienes una copia propia de los
scripts en `~/.openMSX/share/scripts`, copia también ese archivo allí.

Consejo: empieza a grabar un poco antes de que arranque la música y para
después de que acabe. Muchos juegos tardan varios segundos en empezar a sonar.

### Archivos de salida

Un Standard MIDI File de **formato 1**, con 500 ticks por negra a 120 bpm (1 ms
por tick exacto).

- Pista 0: tempo y compás.
- Una pista por cada canal de chip que sonó: `PSG A`, `PSG B`, `PSG C`,
  `SCC 1`...`SCC 5`, `MSX-Music 1`...`MSX-Music 9`.
- `PSG noise` y `MSX-Music rhythm`: pistas aparte, las dos en el canal de
  percusión General MIDI (10).
- Los canales MIDI se asignan al final, solo a las voces que sonaron, saltando
  el 10. El orden es siempre PSG, SCC, MSX-Music.
- Si se usan más de 15 voces melódicas, el resultado se **reparte en dos
  archivos**: `nombre.mid` (PSG + SCC) y `nombre-msxmusic.mid` (MSX-Music).
- Las pistas en silencio no se escriben.

### Cómo se convierte

El estado de los chips se muestrea cada 5 ms a partir de las escrituras
registradas.

- **El tono** se calcula con los registros reales de periodo/frecuencia y el
  reloj del chip. Suena la nota MIDI más cercana y la fracción va como **pitch
  bend** (±2 semitonos, configurado con RPN 0).
- **Vibrato y deslizamientos**: una desviación de hasta 0,75 semitonos respecto
  a la nota que suena es solo pitch bend, nunca nota nueva.
- **Cambios de nota** dentro de ±2 semitonos deben mantenerse 60 ms para contar
  como nota nueva; los saltos mayores (arpegios, efectos) son inmediatos.
- **El volumen** pasa a CC#11 (expresión) y a la velocity de cada nota.
- **La envolvente del PSG** se evalúa exacta (las 16 formas). Las envolventes
  repetidas muy rápidas se dejan a volumen constante. Si el tono y el ruido
  están apagados, la frecuencia de repetición de la envolvente es la altura
  (bajo "buzzer") y se escribe como una nota de Synth Bass.
- **Los arpegios** (tandas de notas de como mucho 60 ms que ciclan entre 2 y 4
  alturas, al menos 6 notas) se escriben como un único acorde sostenido, porque
  una voz del chip que recorre un acorde en cada fotograma se oye como ese
  acorde. Los patrones lentos y los barridos no se tocan. `midi_record arpeggio
  off` escribe todas las notas.
- **El ruido del PSG** pasa a batería GM en 6 bandas (hi-hats, caja, toms, bombo).
- **MSX-Music**: los 15 instrumentos internos se asignan a programas General
  MIDI, se vigilan los flancos de key-on para que un apagado/encendido rápido
  de la misma nota cuente como nota nueva, y la batería del modo ritmo va al
  canal de percusión.
- **SCC**: se analiza la forma de onda de 32 muestras (cuadrada, sierra,
  seno/triángulo, otra) para elegir el programa GM. El volumen se escala por la
  amplitud de la onda. Funcionan los tres modos del SCC (real, compatible, SCC+).

### Limitaciones conocidas

- El vibrato interno del YM2413 no se ve en los registros, así que no llega al
  MIDI. El multiplicador del portador y el parche de usuario no se traducen (el
  parche de usuario suena como Square Lead).
- El registro de deformación del SCC (rotación / frecuencia reducida) se ignora.
- Los tonos del PSG que cambian más deprisa que la rejilla de 5 ms (algunos
  efectos digitales) se pierden.
- Solo se graba una máquina a la vez (la actual).
- Se graba lo que se *escribe en los chips*, no la mezcla final: no sirve para
  samples (trucos de PCM/DAC), solo para las voces FM/PSG/SCC.

### Compilar

Es un árbol normal de openMSX; mira el [`README`](README) original y la
documentación de openMSX.

- **Linux:** `make` (o `meson`) como siempre. No hace falta nada nuevo. Es la
  plataforma donde se desarrolló y se probó. Debería compilar también en macOS,
  pero no lo he probado.
- **Windows:** los proyectos de Visual Studio (`build/msvc/openmsx.vcxproj` y
  `.filters`) incluyen los nuevos `sound/MidiRecorder.{cc,hh}`, y el código usa
  solo C++ portable (`std::numbers::pi`, `FileOperations::openOfStream`). **No**
  he podido compilarlo en Windows; si lo haces, avísame de cualquier problema.

### Reproducir los archivos

Sirve cualquier reproductor General MIDI. En Linux, por ejemplo:

```
fluidsynth -a pipewire /usr/share/soundfonts/FluidR3_GM.sf2 cancion.mid
```

Buenos editores para revisar las pistas: Rosegarden, MuseScore, Ardour, LMMS.

### Probado con

Circus Charlie y Dragon Quest (PSG), Aleste (PSG + MSX-Music) y Gradius 2
(PSG + SCC), en máquinas C-BIOS.

### Créditos y licencia

openMSX es © los desarrolladores de openMSX y se distribuye bajo la GNU GPL v2;
este repositorio mantiene esa licencia (mira [`doc/GPL.txt`](doc/GPL.txt)). El código nuevo se publica con la
misma licencia.

La idea viene de la exportación MIDI de fMSX (Marat Fayzullin). Las reglas de
conversión (vibrato como pitch bend, filtro de estabilidad antes de cambiar de
nota, bandas de ruido a batería) se inspiran en el firmware *MIDI-PAC* del
proyecto [MSX PicoVerse](https://github.com/cristianoag/msx-picoverse-public).
No se ha copiado código de ninguno de los dos (el código de PicoVerse tiene una
licencia no comercial incompatible con la GPL); los parámetros y el código de
aquí son propios.
