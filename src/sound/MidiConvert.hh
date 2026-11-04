#ifndef MIDICONVERT_HH
#define MIDICONVERT_HH

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace openmsx::midi {

// This is the part of the MIDI export that does not depend on the emulator:
// the event lists, the state machines that turn the sampled state of a chip
// voice into notes, the post-processing of a track and the writing of a
// Standard MIDI File. The emulator side is in MidiRecorder.

// ---------------------------------------------------------------------------
// Conversion parameters
// ---------------------------------------------------------------------------

// MIDI timing: 500 ticks per quarter note at 120 bpm -> exactly 1000 ticks/s.
inline constexpr unsigned MIDI_DIVISION = 500;
inline constexpr unsigned TICKS_PER_SECOND = 1000;

// The chip state is sampled on a fixed grid of 5 ms (200 Hz).
inline constexpr double GRID_SECONDS = 0.005;

// Pitch bend range configured on every channel (RPN 0), in semitones.
inline constexpr double BEND_RANGE = 2.0;
// Deviations up to this many semitones from the sounding note are rendered as
// pitch bend only (vibrato, slides), never as a new note.
inline constexpr double BEND_ONLY_WINDOW = 0.75;
// A new note (nearer than 'JUMP_SEMITONES') must be stable for this many grid
// steps (3 frames at 50 Hz = 60 ms) before it is accepted as a new note.
inline constexpr int STABLE_STEPS = 12;
// Bigger jumps are accepted immediately (arpeggios, sound effects).
inline constexpr double JUMP_SEMITONES = 2.0;
// PSG periods below this value are inaudible / not representable.
inline constexpr unsigned MIN_PERIOD = 10;
// A volume that jumps up by at least this many levels (0..15) on a sounding
// voice is a new attack of the same pitch: written as a new note, not as a
// volume change (repeated notes, rapid retriggers).
inline constexpr int REATTACK_LEVELS = 4;
// A flutter (the game alternates the volume every frame) is a series of
// attacks too: a drop of this many levels followed by a rise of as many.
inline constexpr int FLUTTER_LEVELS = 2;
// ... but not within this many grid steps (15 ms) of the start of the note.
inline constexpr int REATTACK_MIN_STEPS = 3;
// Notes shorter than this are glitches (the game updates the chip at most
// every frame, 20 ms; a pitch read between the writes of the low and the high
// byte of a period shows up as a 5 ms note) and are always dropped.
inline constexpr uint32_t GLITCH_NOTE_TICKS = 10;
// Pitch-drop percussion: a fast descending run of short notes (a drum made
// with a falling tone) can be written as a drum hit instead.
inline constexpr uint32_t SWEEP_NOTE_MAX_TICKS = 40;  // longest note in the run
inline constexpr uint32_t SWEEP_GAP_MAX_TICKS = 10;   // longest gap between its notes
inline constexpr uint32_t SWEEP_MAX_TICKS = 400;      // longest run
inline constexpr size_t SWEEP_MIN_NOTES = 3;
inline constexpr int SWEEP_MIN_DROP = 6;              // semitones from first to last
inline constexpr int SWEEP_SNARE_MIN_PITCH = 55;      // starts higher: snare, else bass drum
inline constexpr uint8_t DRUM_SNARE = 38;
inline constexpr uint8_t DRUM_BASS = 36;
inline constexpr uint32_t SWEEP_DRUM_TICKS = 60;      // length of the drum note
// Simple detail: notes shorter than this are dropped.
inline constexpr uint32_t SIMPLE_MIN_NOTE_TICKS = 60;
// General MIDI "Square Lead".
inline constexpr uint8_t PROGRAM_SQUARE_LEAD = 80;
// General MIDI "Synth Bass 1": used for buzzer bass (pitch given by the envelope).
inline constexpr uint8_t PROGRAM_SYNTH_BASS = 38;
// Drum hits last at most this long.
inline constexpr int DRUM_MAX_STEPS = 12; // 60 ms
// Envelope cycles shorter than this are "buzzer" sounds: they are rendered
// with a constant volume instead of a million volume changes.
inline constexpr double BUZZ_CYCLE_SECONDS = 0.02;
// A repeating envelope with a cycle shorter than this, combined with a tone
// that is a harmonic of it (or no tone), is a buzzer bass: its pitch is the
// envelope repetition rate.
inline constexpr double BUZZ_BASS_CYCLE_SECONDS = 0.06;
// Arpeggios: runs of very short notes that cycle through a few pitches are
// written as one sustained chord (when enabled).
inline constexpr uint32_t ARP_NOTE_MAX_TICKS = 60;   // longest note in an arpeggio
inline constexpr uint32_t ARP_GAP_MAX_TICKS = 10;    // longest gap between its notes
inline constexpr size_t ARP_MIN_NOTES = 4;           // at least this many notes (and two cycles)
inline constexpr size_t ARP_MAX_PITCHES = 4;         // at most this many different pitches
// Number of MIDI channels available for melodic voices (16 minus drums).
inline constexpr size_t MELODIC_CHANNELS = 15;
inline constexpr uint8_t DRUM_CHANNEL = 9;

// YM2413 (MSX-Music)
inline constexpr double OPLL_CLOCK = 3579545.0;
// General MIDI programs (0-based) for the 15 built-in OPLL instruments.
// Index 0 is the user-defined patch, for which we have no good match.
inline constexpr std::array<uint8_t, 16> OPLL_PROGRAMS = {
	80, // 0 User        -> Square Lead
	40, // 1 Violin      -> Violin
	24, // 2 Guitar      -> Acoustic Guitar (nylon)
	1,  // 3 Piano       -> Bright Acoustic Piano
	73, // 4 Flute       -> Flute
	71, // 5 Clarinet    -> Clarinet
	68, // 6 Oboe        -> Oboe
	56, // 7 Trumpet     -> Trumpet
	19, // 8 Organ       -> Church Organ
	60, // 9 Horn        -> French Horn
	50, // 10 Synthesizer-> Synth Strings 1
	6,  // 11 Harpsichord-> Harpsichord
	11, // 12 Vibraphone -> Vibraphone
	38, // 13 Synth Bass -> Synth Bass 1
	32, // 14 Wood Bass  -> Acoustic Bass
	27, // 15 Elec Guitar-> Electric Guitar (clean)
};
// Frequency multipliers (MULT) of the operators, in half units (index = MULT
// register value, 0 means x1/2).
inline constexpr std::array<unsigned, 16> OPLL_MULT_HALVES = {
	1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 20, 24, 24, 30, 30};
// MULT of {modulator, carrier} of the 15 built-in instruments (index 1..15,
// from the instrument ROM).
inline constexpr std::array<std::array<uint8_t, 2>, 16> OPLL_ROM_MULT = {{
	{0, 0},                                   // user: read from registers
	{1, 1}, {3, 1}, {3, 1}, {1, 1}, {2, 1},   // violin guitar piano flute clarinet
	{1, 2}, {1, 1}, {3, 1}, {1, 1}, {1, 1},   // oboe trumpet organ horn synthesizer
	{3, 1}, {7, 1}, {1, 0}, {1, 1}, {1, 1}}}; // harpsichord vibraphone synth-bass wood-bass e-guitar
// A user patch whose modulator is quieter than this (total level, 0.75 dB
// steps) does not modulate audibly: the carrier alone sets the pitch.
inline constexpr unsigned OPLL_WEAK_MODULATOR_TL = 40;

// GM percussion notes for the 5 OPLL rhythm instruments, in the order of
// the bits of register 0x0E (bit 0 first): hi-hat, top cymbal, tom-tom,
// snare drum, bass drum.
inline constexpr std::array<uint8_t, 5> OPLL_DRUMS = {42, 49, 47, 38, 36};

// ---------------------------------------------------------------------------
// MIDI event list
// ---------------------------------------------------------------------------

struct Event {
	uint32_t tick;
	std::array<uint8_t, 3> data;
	uint8_t size;
};

// A MIDI track. The channel of channel messages is filled in when the file is
// written (the events are generated with channel 0).
struct Track {
	enum class Group : uint8_t { PSG_SCC, MSX_MUSIC };

	Track(std::string name_, Group group_, bool melodic_)
		: name(std::move(name_)), group(group_), melodic(melodic_) {}

	void add(uint32_t tick, uint8_t status, uint8_t d1, uint8_t d2) {
		events.push_back({tick, {status, d1, d2}, 3});
	}
	void add2(uint32_t tick, uint8_t status, uint8_t d1) {
		events.push_back({tick, {status, d1, 0}, 2});
	}
	void setup(uint8_t program, uint8_t pan) {
		add2(0, 0xC0, program);
		add(0, 0xB0, 7, 100);
		add(0, 0xB0, 10, pan);
		add(0, 0xB0, 11, 0);
		// RPN 0: pitch bend range
		add(0, 0xB0, 101, 0);
		add(0, 0xB0, 100, 0);
		add(0, 0xB0, 6, uint8_t(BEND_RANGE));
		add(0, 0xB0, 38, 0);
		add(0, 0xB0, 101, 127);
		add(0, 0xB0, 100, 127);
	}

	std::string name;
	Group group;
	bool melodic;
	int channel = -1;
	size_t used = 0; // number of notes/drum hits
	std::vector<Event> events;
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

/** Time in seconds to MIDI ticks. */
[[nodiscard]] uint32_t toTick(double seconds);
/** Chip volume 0..15 (15 = loudest) to MIDI expression (CC#11). */
[[nodiscard]] uint8_t exprForLevel(unsigned level);
/** Chip volume 0..15 to MIDI note-on velocity. */
[[nodiscard]] uint8_t velocityForLevel(unsigned level);
/** Frequency in Hz to a fractional MIDI note number (A4 = 69). */
[[nodiscard]] double fineNoteForFrequency(double freq);

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------

/** State machine that turns a (pitch, volume) stream, sampled on the grid,
  * into notes, pitch bends and volume changes. */
struct ToneVoice {
	int note = -1;
	int pendNote = -1;
	int pendCount = 0;
	int lastExpr = -1;
	int lastWheel = -1;
	int program = -1;
	int lastLevel = -1; // level at the previous step while sounding
	int age = 0;        // grid steps since the current note started
	bool lastChangeWasDrop = false;
	bool detailed = true; // false: no re-attacks, no pitch bend

	/** @param active    is the voice sounding at this moment
	  * @param fine      pitch as fractional MIDI note number
	  * @param level     volume 0..15
	  * @param forceNew  start a new note even if the pitch is the same
	  * @param prog      MIDI program, or -1 to leave unchanged */
	void update(Track& t, uint32_t tick, bool active, double fine, unsigned level,
	            bool forceNew, int prog);
	/** Release the sounding note, if any. */
	void finish(Track& t, uint32_t tick) const;
};

/** A drum track: notes on the percussion channel. */
struct DrumVoice {
	int drum = -1;
	int age = 0;

	void hit(Track& t, uint32_t tick, int note, unsigned level);
	void release(Track& t, uint32_t tick);
	/** Call once per grid step. */
	void tick(Track& t, uint32_t tick);
};

// ---------------------------------------------------------------------------
// Post-processing of a track
// ---------------------------------------------------------------------------

/** Replace arpeggios (see ARP_* constants) in a melodic track by chords. */
void collapseArpeggios(Track& t);

/** Move the fast descending runs of short notes of a voice to the drum track
  * as drum hits (see SWEEP_* constants). Returns the number of hits. */
size_t sweepsToDrums(Track& voice, Track& drums);

/** Remove the notes that are shorter than 'minTicks'. */
void dropShortNotes(Track& t, uint32_t minTicks);

// ---------------------------------------------------------------------------
// Standard MIDI File
// ---------------------------------------------------------------------------

/** Append a MIDI variable-length quantity. */
void putVlq(std::string& s, uint32_t v);

/** The bytes of a format 1 Standard MIDI File: a tempo track followed by the
  * given tracks. */
[[nodiscard]] std::string buildMidiFile(const std::vector<const Track*>& tracks,
                                        uint32_t endTick);

// ---------------------------------------------------------------------------
// Chip models
// ---------------------------------------------------------------------------

/** Level (0..15) of the PSG envelope generator after 'step' steps. */
[[nodiscard]] unsigned envelopeLevel(unsigned shape, uint64_t step);

/** Pick a General MIDI program that resembles an SCC 32-sample wave form. */
struct WaveInfo {
	uint8_t program;
	unsigned peak; // 0..128
};
[[nodiscard]] WaveInfo classifyWave(const uint8_t* raw);

/** GM percussion note for a PSG noise period. */
[[nodiscard]] uint8_t drumForNoise(unsigned noisePeriod);

/** Pitch offset in semitones between the F-number frequency and the pitch that
  * is heard for an OPLL channel. The spectrum of two operators consists of
  * the multiples of the greatest common divisor of their MULT values, so e.g.
  * the synthesizer bass (carrier MULT 1/2) sounds an octave lower.
  * @param inst the instrument number (0 = user patch)
  * @param regs the OPLL registers 0x00..0x3F (the user patch is read from them) */
[[nodiscard]] double opllPitchOffset(unsigned inst, const std::array<uint8_t, 64>& regs);

} // namespace openmsx::midi

#endif
