#include "MidiRecorder.hh"

#include "Command.hh"
#include "CommandException.hh"
#include "FileContext.hh"
#include "FileOperations.hh"
#include "MSXMotherBoard.hh"
#include "TclObject.hh"

#include "strCat.hh"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <fstream>
#include <numbers>
#include <optional>

namespace openmsx {

// ---------------------------------------------------------------------------
// Conversion parameters
// ---------------------------------------------------------------------------

// MIDI timing: 500 ticks per quarter note at 120 bpm -> exactly 1000 ticks/s.
static constexpr unsigned MIDI_DIVISION = 500;
static constexpr unsigned TICKS_PER_SECOND = 1000;

// The chip state is sampled on a fixed grid of 5 ms (200 Hz).
static constexpr double GRID_SECONDS = 0.005;

// Pitch bend range configured on every channel (RPN 0), in semitones.
static constexpr double BEND_RANGE = 2.0;
// Deviations up to this many semitones from the sounding note are rendered as
// pitch bend only (vibrato, slides), never as a new note.
static constexpr double BEND_ONLY_WINDOW = 0.75;
// A new note (nearer than 'JUMP_SEMITONES') must be stable for this many grid
// steps (3 frames at 50 Hz = 60 ms) before it is accepted as a new note.
static constexpr int STABLE_STEPS = 12;
// Bigger jumps are accepted immediately (arpeggios, sound effects).
static constexpr double JUMP_SEMITONES = 2.0;
// PSG periods below this value are inaudible / not representable.
static constexpr unsigned MIN_PERIOD = 10;
// A volume that jumps up by at least this many levels (0..15) on a sounding
// voice is a new attack of the same pitch: written as a new note, not as a
// volume change (repeated notes, rapid retriggers).
static constexpr int REATTACK_LEVELS = 4;
// A flutter (the game alternates the volume every frame) is a series of
// attacks too: a drop of this many levels followed by a rise of as many.
static constexpr int FLUTTER_LEVELS = 2;
// ... but not within this many grid steps (15 ms) of the start of the note.
static constexpr int REATTACK_MIN_STEPS = 3;
// Notes shorter than this are glitches (the game updates the chip at most
// every frame, 20 ms; a pitch read between the writes of the low and the high
// byte of a period shows up as a 5 ms note) and are always dropped.
static constexpr uint32_t GLITCH_NOTE_TICKS = 10;
// Pitch-drop percussion: a fast descending run of short notes (a drum made
// with a falling tone) can be written as a drum hit instead.
static constexpr uint32_t SWEEP_NOTE_MAX_TICKS = 40;  // longest note in the run
static constexpr uint32_t SWEEP_GAP_MAX_TICKS = 10;   // longest gap between its notes
static constexpr uint32_t SWEEP_MAX_TICKS = 400;      // longest run
static constexpr size_t SWEEP_MIN_NOTES = 3;
static constexpr int SWEEP_MIN_DROP = 6;              // semitones from first to last
static constexpr int SWEEP_SNARE_MIN_PITCH = 55;      // starts higher: snare, else bass drum
static constexpr uint8_t DRUM_SNARE = 38;
static constexpr uint8_t DRUM_BASS = 36;
static constexpr uint32_t SWEEP_DRUM_TICKS = 60;      // length of the drum note
// Simple detail: notes shorter than this are dropped.
static constexpr uint32_t SIMPLE_MIN_NOTE_TICKS = 60;
// General MIDI "Square Lead".
static constexpr uint8_t PROGRAM_SQUARE_LEAD = 80;
// General MIDI "Synth Bass 1": used for buzzer bass (pitch given by the envelope).
static constexpr uint8_t PROGRAM_SYNTH_BASS = 38;
// Drum hits last at most this long.
static constexpr int DRUM_MAX_STEPS = 12; // 60 ms
// Envelope cycles shorter than this are "buzzer" sounds: they are rendered
// with a constant volume instead of a million volume changes.
static constexpr double BUZZ_CYCLE_SECONDS = 0.02;
// A repeating envelope with a cycle shorter than this, combined with a tone
// that is a harmonic of it (or no tone), is a buzzer bass: its pitch is the
// envelope repetition rate.
static constexpr double BUZZ_BASS_CYCLE_SECONDS = 0.06;
// Arpeggios: runs of very short notes that cycle through a few pitches are
// written as one sustained chord (when enabled).
static constexpr uint32_t ARP_NOTE_MAX_TICKS = 60;   // longest note in an arpeggio
static constexpr uint32_t ARP_GAP_MAX_TICKS = 10;    // longest gap between its notes
static constexpr size_t ARP_MIN_NOTES = 6;           // at least this many notes
static constexpr size_t ARP_MAX_PITCHES = 4;         // at most this many different pitches
// Number of MIDI channels available for melodic voices (16 minus drums).
static constexpr size_t MELODIC_CHANNELS = 15;
static constexpr uint8_t DRUM_CHANNEL = 9;

// YM2413 (MSX-Music)
static constexpr double OPLL_CLOCK = 3579545.0;
// General MIDI programs (0-based) for the 15 built-in OPLL instruments.
// Index 0 is the user-defined patch, for which we have no good match.
static constexpr std::array<uint8_t, 16> OPLL_PROGRAMS = {
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
// GM percussion notes for the 5 OPLL rhythm instruments, in the order of
// the bits of register 0x0E (bit 0 first): hi-hat, top cymbal, tom-tom,
// snare drum, bass drum.
static constexpr std::array<uint8_t, 5> OPLL_DRUMS = {42, 49, 47, 38, 36};

// ---------------------------------------------------------------------------
// MIDI event list
// ---------------------------------------------------------------------------

namespace {

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

/** Replace arpeggios (see ARP_* constants) in a melodic track by chords. */
void collapseArpeggios(Track& t)
{
	struct Note {
		size_t on, off;
		uint32_t t0, t1;
		uint8_t pitch, vel;
	};
	constexpr size_t NONE = size_t(-1);
	auto& ev = t.events;

	std::vector<Note> notes;
	std::array<size_t, 128> open;
	open.fill(NONE);
	for (size_t i = 0; i < ev.size(); ++i) {
		const auto& e = ev[i];
		uint8_t status = e.data[0] & 0xF0;
		if (status != 0x80 && status != 0x90) continue;
		uint8_t pitch = e.data[1] & 0x7F;
		if (status == 0x90 && e.data[2] != 0) {
			if (open[pitch] != NONE) continue;
			open[pitch] = notes.size();
			notes.push_back({i, NONE, e.tick, e.tick, pitch, e.data[2]});
		} else if (open[pitch] != NONE) {
			notes[open[pitch]].off = i;
			notes[open[pitch]].t1 = e.tick;
			open[pitch] = NONE;
		}
	}

	std::vector<bool> removed(ev.size(), false);
	std::vector<std::vector<Event>> insertBefore(ev.size());
	bool changed = false;

	auto emitChord = [&](size_t a, size_t b) {
		std::array<uint8_t, ARP_MAX_PITCHES> pitches = {};
		size_t distinct = 0;
		unsigned vel = 0;
		for (size_t k = a; k <= b; ++k) {
			bool found = false;
			for (size_t q = 0; q < distinct; ++q) found |= pitches[q] == notes[k].pitch;
			if (!found) pitches[distinct++] = notes[k].pitch;
			vel = std::max<unsigned>(vel, notes[k].vel);
		}
		if (distinct < 2 || b - a + 1 < ARP_MIN_NOTES) return;
		uint32_t t0 = notes[a].t0, t1 = notes[b].t1;
		for (size_t k = a; k <= b; ++k) {
			removed[notes[k].on] = true;
			removed[notes[k].off] = true;
		}
		// the pitch bends of the single notes (events are sorted by tick)
		auto first = std::lower_bound(ev.begin(), ev.end(), t0,
			[](const Event& e, uint32_t tick) { return e.tick < tick; });
		for (auto it = first; it != ev.end() && it->tick < t1; ++it) {
			if ((it->data[0] & 0xF0) == 0xE0) removed[size_t(it - ev.begin())] = true;
		}
		auto& before = insertBefore[notes[a].on];
		before.push_back({t0, {0xE0, 0x00, 0x40}, 3}); // bend centered
		for (size_t q = 0; q < distinct; ++q) {
			before.push_back({t0, {0x90, pitches[q], uint8_t(vel)}, 3});
		}
		auto& after = insertBefore[notes[b].off];
		for (size_t q = 0; q < distinct; ++q) {
			after.push_back({t1, {0x80, pitches[q], 0}, 3});
		}
		changed = true;
	};

	size_t i = 0;
	while (i < notes.size()) {
		// find a run of short notes that follow each other closely
		size_t j = i;
		auto fits = [&](size_t k) {
			return notes[k].off != NONE && notes[k].t1 - notes[k].t0 <= ARP_NOTE_MAX_TICKS;
		};
		if (!fits(i)) { ++i; continue; }
		while (j + 1 < notes.size() && fits(j + 1) &&
		       notes[j + 1].t0 - notes[j].t1 <= ARP_GAP_MAX_TICKS) ++j;
		// cut the run into chords: a pitch that is not part of the current
		// cycle starts a new chord once the cycle has been repeated
		size_t segStart = i;
		std::array<uint8_t, ARP_MAX_PITCHES> set = {};
		size_t distinct = 0;
		bool repeated = false;
		for (size_t k = i; k <= j; ++k) {
			bool found = false;
			for (size_t q = 0; q < distinct; ++q) found |= set[q] == notes[k].pitch;
			if (found) {
				repeated = true;
			} else if (distinct < ARP_MAX_PITCHES && !repeated) {
				set[distinct++] = notes[k].pitch;
			} else {
				emitChord(segStart, k - 1);
				segStart = k;
				set = {};
				set[0] = notes[k].pitch;
				distinct = 1;
				repeated = false;
			}
		}
		if (repeated) emitChord(segStart, j);
		i = j + 1;
	}
	if (!changed) return;

	std::vector<Event> out;
	out.reserve(ev.size());
	for (size_t k = 0; k < ev.size(); ++k) {
		out.insert(out.end(), insertBefore[k].begin(), insertBefore[k].end());
		if (!removed[k]) out.push_back(ev[k]);
	}
	ev = std::move(out);
	t.used = 0;
	for (const auto& e : ev) {
		if ((e.data[0] & 0xF0) == 0x90 && e.data[2] != 0) ++t.used;
	}
}

/** Move the fast descending runs of short notes of a voice to the drum track
  * as drum hits (see SWEEP_* constants). Returns the number of hits. */
size_t sweepsToDrums(Track& voice, Track& drums)
{
	struct Note {
		size_t on, off;
		uint32_t t0, t1;
		uint8_t pitch, vel;
	};
	constexpr size_t NONE = size_t(-1);
	auto& ev = voice.events;

	std::vector<Note> notes;
	std::array<size_t, 128> open;
	open.fill(NONE);
	for (size_t i = 0; i < ev.size(); ++i) {
		uint8_t status = ev[i].data[0] & 0xF0;
		if (status != 0x80 && status != 0x90) continue;
		uint8_t pitch = ev[i].data[1] & 0x7F;
		if (status == 0x90 && ev[i].data[2] != 0) {
			if (open[pitch] != NONE) continue;
			open[pitch] = notes.size();
			notes.push_back({i, NONE, ev[i].tick, ev[i].tick, pitch, ev[i].data[2]});
		} else if (open[pitch] != NONE) {
			notes[open[pitch]].off = i;
			notes[open[pitch]].t1 = ev[i].tick;
			open[pitch] = NONE;
		}
	}

	std::vector<bool> removed(ev.size(), false);
	size_t hits = 0;
	auto fits = [&](size_t k) {
		return notes[k].off != NONE && notes[k].t1 - notes[k].t0 <= SWEEP_NOTE_MAX_TICKS;
	};
	auto emit = [&](size_t a, size_t b) {
		if (b - a + 1 < SWEEP_MIN_NOTES) return;
		if (int(notes[a].pitch) - int(notes[b].pitch) < SWEEP_MIN_DROP) return;
		if (notes[b].t1 - notes[a].t0 > SWEEP_MAX_TICKS) return;
		unsigned vel = 0;
		for (size_t k = a; k <= b; ++k) {
			removed[notes[k].on] = removed[notes[k].off] = true;
			vel = std::max<unsigned>(vel, notes[k].vel);
		}
		uint32_t t0 = notes[a].t0, t1 = notes[b].t1;
		auto first = std::lower_bound(ev.begin(), ev.end(), t0,
			[](const Event& e, uint32_t tick) { return e.tick < tick; });
		for (auto it = first; it != ev.end() && it->tick < t1; ++it) {
			if ((it->data[0] & 0xF0) == 0xE0) removed[size_t(it - ev.begin())] = true;
		}
		uint8_t drum = (notes[a].pitch >= SWEEP_SNARE_MIN_PITCH) ? DRUM_SNARE : DRUM_BASS;
		drums.add(t0, 0x90, drum, uint8_t(vel));
		drums.add(t0 + SWEEP_DRUM_TICKS, 0x80, drum, 0);
		++drums.used;
		++hits;
	};

	size_t i = 0;
	while (i < notes.size()) {
		if (!fits(i)) { ++i; continue; }
		size_t j = i;
		while (j + 1 < notes.size() && fits(j + 1) &&
		       notes[j + 1].t0 - notes[j].t1 <= SWEEP_GAP_MAX_TICKS &&
		       notes[j + 1].pitch < notes[j].pitch) ++j;
		emit(i, j);
		i = j + 1;
	}
	if (hits == 0) return 0;

	std::vector<Event> out;
	out.reserve(ev.size());
	for (size_t k = 0; k < ev.size(); ++k) {
		if (!removed[k]) out.push_back(ev[k]);
	}
	ev = std::move(out);
	voice.used = 0;
	for (const auto& e : ev) {
		if ((e.data[0] & 0xF0) == 0x90 && e.data[2] != 0) ++voice.used;
	}
	std::ranges::stable_sort(drums.events, {}, &Event::tick);
	return hits;
}

/** Remove the notes that are shorter than 'minTicks'. */
void dropShortNotes(Track& t, uint32_t minTicks)
{
	auto& ev = t.events;
	std::vector<bool> removed(ev.size(), false);
	std::array<size_t, 128> open;
	constexpr size_t NONE = size_t(-1);
	open.fill(NONE);
	bool changed = false;
	for (size_t i = 0; i < ev.size(); ++i) {
		uint8_t status = ev[i].data[0] & 0xF0;
		if (status != 0x80 && status != 0x90) continue;
		uint8_t pitch = ev[i].data[1] & 0x7F;
		if (status == 0x90 && ev[i].data[2] != 0) {
			open[pitch] = i;
		} else if (open[pitch] != NONE) {
			if (ev[i].tick - ev[open[pitch]].tick < minTicks) {
				removed[open[pitch]] = removed[i] = true;
				changed = true;
			}
			open[pitch] = NONE;
		}
	}
	if (!changed) return;
	std::vector<Event> out;
	out.reserve(ev.size());
	t.used = 0;
	for (size_t i = 0; i < ev.size(); ++i) {
		if (removed[i]) continue;
		out.push_back(ev[i]);
		if ((ev[i].data[0] & 0xF0) == 0x90 && ev[i].data[2] != 0) ++t.used;
	}
	ev = std::move(out);
}

void putVlq(std::string& s, uint32_t v)
{
	char buf[5];
	int n = 0;
	buf[n++] = char(v & 0x7F);
	while (v >>= 7) buf[n++] = char((v & 0x7F) | 0x80);
	while (n--) s.push_back(buf[n]);
}
void put16(std::string& s, uint32_t v)
{
	s.push_back(char(v >> 8)); s.push_back(char(v));
}
void put32(std::string& s, uint32_t v)
{
	put16(s, v >> 16); put16(s, v & 0xFFFF);
}

void writeTrack(std::string& out, const Track& t, uint32_t endTick)
{
	std::string body;
	putVlq(body, 0);
	body += "\xFF\x03";
	putVlq(body, uint32_t(t.name.size()));
	body += t.name;

	uint32_t last = 0;
	for (const auto& e : t.events) {
		uint32_t tick = std::max(e.tick, last); // never go back in time
		putVlq(body, tick - last);
		last = tick;
		uint8_t status = e.data[0];
		if (t.channel >= 0 && status >= 0x80 && status < 0xF0) {
			status = uint8_t((status & 0xF0) | t.channel);
		}
		body.push_back(char(status));
		for (unsigned i = 1; i < e.size; ++i) body.push_back(char(e.data[i]));
	}
	putVlq(body, std::max(endTick, last) - last);
	body += "\xFF\x2F";
	body.push_back(0);

	out += "MTrk";
	put32(out, uint32_t(body.size()));
	out += body;
}

void writeMidiFile(const std::string& filename,
                   const std::vector<const Track*>& tracks, uint32_t endTick)
{
	std::string out;
	out += "MThd";
	put32(out, 6);
	put16(out, 1);
	put16(out, uint32_t(tracks.size() + 1));
	put16(out, MIDI_DIVISION);
	{
		// Tempo track: 120 bpm, 4/4.
		std::string body;
		putVlq(body, 0);
		body += "\xFF\x03";
		std::string_view name = "openMSX";
		putVlq(body, uint32_t(name.size()));
		body += name;
		putVlq(body, 0);
		body += "\xFF\x51\x03\x07\xA1\x20"; // 500000 us/quarter
		putVlq(body, 0);
		body += "\xFF\x58\x04\x04\x02\x18\x08"; // 4/4
		putVlq(body, endTick);
		body += "\xFF\x2F";
		body.push_back(0);
		out += "MTrk";
		put32(out, uint32_t(body.size()));
		out += body;
	}
	for (const auto* t : tracks) writeTrack(out, *t, endTick);

	std::ofstream file;
	FileOperations::openOfStream(file, filename, std::ios::binary | std::ios::trunc);
	if (!file) throw CommandException("Cannot write to ", filename);
	file.write(out.data(), std::streamsize(out.size()));
	if (!file) throw CommandException("Error writing ", filename);
}

// ---------------------------------------------------------------------------
// Common helpers
// ---------------------------------------------------------------------------

uint32_t toTick(double seconds)
{
	return uint32_t(std::llround(seconds * TICKS_PER_SECOND));
}

// 'level' is a chip volume in 0..15 (15 = loudest).
uint8_t exprForLevel(unsigned level)
{
	if (level == 0) return 0;
	return uint8_t(std::lround(127.0 * std::pow(level / 15.0, 0.75)));
}
uint8_t velocityForLevel(unsigned level)
{
	return uint8_t(std::clamp(long(std::lround(30.0 + 97.0 * level / 15.0)), 1L, 127L));
}

double fineNoteForFrequency(double freq)
{
	return 69.0 + 12.0 * std::log2(freq / 440.0);
}

// State machine that turns a (pitch, volume) stream, sampled on the grid, into
// notes, pitch bends and volume changes.
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
	            bool forceNew, int prog)
	{
		if (!active) {
			if (note >= 0) {
				t.add(tick, 0x80, uint8_t(note), 0);
				note = -1;
			}
			pendNote = -1;
			pendCount = 0;
			lastLevel = -1;
			lastChangeWasDrop = false;
			return;
		}
		++age;
		if (detailed && lastLevel >= 0 && age >= REATTACK_MIN_STEPS) {
			// a big jump up, or a flutter: down by 2+ levels, then up by 2+ again
			if (int(level) >= lastLevel + REATTACK_LEVELS ||
			    (lastChangeWasDrop && int(level) >= lastLevel + FLUTTER_LEVELS)) {
				forceNew = true;
			}
		}
		if (lastLevel >= 0 && int(level) != lastLevel) {
			lastChangeWasDrop = int(level) <= lastLevel - FLUTTER_LEVELS;
		}
		lastLevel = int(level);
		int nearest = std::clamp(int(std::lround(fine)), 0, 127);

		bool newNote = false;
		if (note < 0 || forceNew) {
			newNote = true;
		} else {
			double d = fine - note;
			if (std::abs(d) <= BEND_ONLY_WINDOW) {
				pendNote = -1;
				pendCount = 0;
			} else if (std::abs(d) > JUMP_SEMITONES) {
				newNote = true;
			} else {
				if (pendNote != nearest) {
					pendNote = nearest;
					pendCount = 1;
				} else {
					++pendCount;
				}
				if (pendCount >= STABLE_STEPS) newNote = true;
			}
		}

		if (prog >= 0 && prog != program) {
			t.add2(tick, 0xC0, uint8_t(prog));
			program = prog;
		}
		int expr = exprForLevel(level);
		if (newNote) {
			if (note >= 0) t.add(tick, 0x80, uint8_t(note), 0);
			note = nearest;
			age = 0;
			pendNote = -1;
			pendCount = 0;
		}
		// pitch bend relative to the sounding note
		double dev = std::clamp(fine - note, -BEND_RANGE, BEND_RANGE);
		int wheel = int(std::lround(8192.0 + dev * (8192.0 / BEND_RANGE)));
		wheel = std::clamp((wheel + 16) & ~31, 0, 16383);
		if (!detailed) wheel = 8192;
		if (newNote || wheel != lastWheel) {
			t.add(tick, 0xE0, uint8_t(wheel & 0x7F), uint8_t(wheel >> 7));
			lastWheel = wheel;
		}
		if (newNote || expr != lastExpr) {
			t.add(tick, 0xB0, 11, uint8_t(expr));
			lastExpr = expr;
		}
		if (newNote) {
			t.add(tick, 0x90, uint8_t(note), velocityForLevel(level));
			++t.used;
		}
	}

	void finish(Track& t, uint32_t tick) const
	{
		if (note >= 0) t.add(tick, 0x80, uint8_t(note), 0);
	}
};

// A drum track: notes on the percussion channel.
struct DrumVoice {
	int drum = -1;
	int age = 0;

	void hit(Track& t, uint32_t tick, int note, unsigned level)
	{
		release(t, tick);
		t.add(tick, 0x90, uint8_t(note), velocityForLevel(level));
		drum = note;
		age = 0;
		++t.used;
	}
	void release(Track& t, uint32_t tick)
	{
		if (drum >= 0) {
			t.add(tick, 0x80, uint8_t(drum), 0);
			drum = -1;
		}
	}
	// call once per grid step
	void tick(Track& t, uint32_t tick)
	{
		if (drum >= 0 && ++age >= DRUM_MAX_STEPS) release(t, tick);
	}
};

// ---------------------------------------------------------------------------
// PSG
// ---------------------------------------------------------------------------

// Level (0..15) of the PSG envelope generator after 'step' steps.
unsigned envelopeLevel(unsigned shape, uint64_t step)
{
	bool cont = shape & 8;
	bool att  = shape & 4;
	bool alt  = shape & 2;
	bool hold = shape & 1;
	uint64_t run = step / 16;
	unsigned pos = unsigned(step % 16);
	if (run == 0) return att ? pos : 15 - pos;
	if (!cont) return 0;
	if (hold) {
		bool endUp = att; // direction of the first run
		return (endUp != alt) ? 15 : 0;
	}
	bool up = alt ? (att != bool(run & 1)) : att;
	return up ? pos : 15 - pos;
}

// Pick a General MIDI program that resembles an SCC 32-sample wave form.
struct WaveInfo {
	uint8_t program;
	unsigned peak; // 0..128
};
WaveInfo classifyWave(const uint8_t* raw)
{
	std::array<int, 32> w;
	int peak = 0;
	for (unsigned i = 0; i < 32; ++i) {
		w[i] = int(int8_t(raw[i]));
		peak = std::max(peak, std::abs(w[i]));
	}
	if (peak < 4) return {PROGRAM_SQUARE_LEAD, 0};

	// Fraction of the energy in the fundamental (harmonics 1..15).
	double total = 0.0, fundamental = 0.0;
	for (unsigned k = 1; k <= 15; ++k) {
		double re = 0.0, im = 0.0;
		for (unsigned i = 0; i < 32; ++i) {
			double a = 2.0 * std::numbers::pi * k * i / 32.0;
			re += w[i] * std::cos(a);
			im += w[i] * std::sin(a);
		}
		double e = re * re + im * im;
		total += e;
		if (k == 1) fundamental = e;
	}
	double frac1 = (total > 0.0) ? fundamental / total : 1.0;

	unsigned signChanges = 0, nearPeak = 0, bigJumps = 0;
	for (unsigned i = 0; i < 32; ++i) {
		int a = w[i], b = w[(i + 1) % 32];
		if ((a < 0) != (b < 0)) ++signChanges;
		if (std::abs(a) * 10 >= peak * 7) ++nearPeak;
		if (std::abs(a - b) * 10 >= peak * 8) ++bigJumps;
	}

	uint8_t program;
	if (signChanges <= 2 && nearPeak >= 24) {
		program = 80; // square / pulse -> Square Lead
	} else if (bigJumps <= 2 && signChanges <= 2 && frac1 < 0.9) {
		program = 81; // saw -> Sawtooth Lead
	} else if (frac1 >= 0.9) {
		program = 73; // sine / triangle -> Flute
	} else if (signChanges <= 4) {
		program = 81; // rich, but simple: Sawtooth Lead
	} else {
		program = 90; // complex -> Polysynth
	}
	return {program, unsigned(peak)};
}

uint8_t drumForNoise(unsigned noisePeriod)
{
	// Hi-hat closed/open, snare, mid tom, low tom, bass drum
	if (noisePeriod <=  1) return 42;
	if (noisePeriod <=  4) return 46;
	if (noisePeriod <=  8) return 38;
	if (noisePeriod <= 14) return 47;
	if (noisePeriod <= 20) return 41;
	return 36;
}

} // namespace

// ---------------------------------------------------------------------------
// The Tcl command
// ---------------------------------------------------------------------------

class MidiRecordCmd final : public Command
{
public:
	MidiRecordCmd(MSXMotherBoard& motherBoard_, MidiRecorder& recorder_)
		: Command(motherBoard_.getCommandController(), "midi_record")
		, motherBoard(motherBoard_)
		, recorder(recorder_) {}

	void execute(std::span<const TclObject> tokens, TclObject& result) override
	{
		checkNumArgs(tokens, AtLeast{2}, "subcommand ?filename?");
		auto sub = tokens[1].getString();
		auto startRecording = [&](std::string_view arg) {
			auto file = FileOperations::parseCommandFileArgument(
				arg, MidiRecorder::MIDI_DIR, "openmsx",
				MidiRecorder::MIDI_EXTENSION);
			recorder.start(file, motherBoard.getCurrentTime());
			return file;
		};
		if (sub == "start") {
			checkNumArgs(tokens, Between{2, 3}, "start ?filename?");
			std::string_view arg = (tokens.size() == 3) ? tokens[2].getString()
			                                          : std::string_view{};
			result = strCat("Recording to MIDI file ", startRecording(arg));
		} else if (sub == "toggle") {
			if (recorder.isRecording()) {
				result = stopMessage(recorder.stop(motherBoard.getCurrentTime()));
			} else {
				result = strCat("Recording to MIDI file ", startRecording({}));
			}
		} else if (sub == "stop") {
			result = stopMessage(recorder.stop(motherBoard.getCurrentTime()));
		} else if (sub == "arpeggio") {
			checkNumArgs(tokens, Between{2, 3}, "arpeggio ?on|off?");
			if (tokens.size() == 3) {
				auto v = tokens[2].getString();
				if (v == "on") recorder.setArpeggioChords(true);
				else if (v == "off") recorder.setArpeggioChords(false);
				else throw CommandException("Expected on or off, got: ", v);
			}
			result = recorder.getArpeggioChords() ? "on" : "off";
		} else if (sub == "sweeps") {
			checkNumArgs(tokens, Between{2, 3}, "sweeps ?drums|notes?");
			if (tokens.size() == 3) {
				auto v = tokens[2].getString();
				if (v == "drums") recorder.setSweepDrums(true);
				else if (v == "notes") recorder.setSweepDrums(false);
				else throw CommandException("Expected drums or notes, got: ", v);
			}
			result = recorder.getSweepDrums() ? "drums" : "notes";
		} else if (sub == "detail") {
			checkNumArgs(tokens, Between{2, 3}, "detail ?full|simple?");
			if (tokens.size() == 3) {
				auto v = tokens[2].getString();
				if (v == "full") recorder.setDetail(MidiRecorder::Detail::FULL);
				else if (v == "simple") recorder.setDetail(MidiRecorder::Detail::SIMPLE);
				else throw CommandException("Expected full or simple, got: ", v);
			}
			result = (recorder.getDetail() == MidiRecorder::Detail::FULL) ? "full" : "simple";
		} else if (sub == "status") {
			result = recorder.isRecording()
			       ? strCat("recording to ", recorder.getFilename())
			       : std::string("idle");
		} else {
			throw CommandException("Unknown subcommand: ", sub);
		}
	}

	[[nodiscard]] std::string help(std::span<const TclObject> /*tokens*/) const override
	{
		return "midi_record start ?filename?  start recording PSG and MSX-Music as a MIDI file\n"
		       "                              (without filename: next free openmsx0001.mid)\n"
		       "midi_record stop               stop recording and write the file(s)\n"
		       "midi_record toggle             start (automatic name) or stop\n"
		       "midi_record status             show whether a recording is active\n"
		       "midi_record detail ?full|simple?  full (default): as close to the chips as possible;\n"
		       "                              simple: fewer, longer notes (no repeated notes, no\n"
		       "                              pitch bend, no notes under 60 ms, arpeggios as chords)\n"
		       "midi_record sweeps ?drums|notes?  write fast falling tones (drums made with a tone)\n"
		       "                              as drum hits (always in simple) or as notes (default)\n"
		       "midi_record arpeggio ?on|off?  write arpeggios as chords (on, default) or as\n"
		       "                              fast single notes (off)\n";
	}

	void tabCompletion(std::vector<std::string>& tokens) const override
	{
		if (tokens.size() == 2) {
			completeString(tokens, std::array<std::string_view, 7>{"start", "stop", "toggle", "status", "arpeggio", "detail", "sweeps"});
		} else if (tokens.size() == 3 && tokens[1] == "sweeps") {
			completeString(tokens, std::array<std::string_view, 2>{"drums", "notes"});
		} else if (tokens.size() == 3 && tokens[1] == "detail") {
			completeString(tokens, std::array<std::string_view, 2>{"full", "simple"});
		} else if (tokens.size() == 3 && tokens[1] == "arpeggio") {
			completeString(tokens, std::array<std::string_view, 2>{"on", "off"});
		} else if (tokens.size() == 3 && tokens[1] == "start") {
			completeFileName(tokens, userFileContext());
		}
	}

private:
	static std::string stopMessage(const MidiRecorder::Result& r)
	{
		auto s = strCat("Saved ", r.filename);
		if (!r.filename2.empty()) strAppend(s, " and ", r.filename2);
		strAppend(s, ": ", r.notes, " notes, ", r.drums, " drum hits, ",
		          std::lround(r.seconds), " seconds");
		return s;
	}

	MSXMotherBoard& motherBoard;
	MidiRecorder& recorder;
};

// ---------------------------------------------------------------------------
// MidiRecorder
// ---------------------------------------------------------------------------

MidiRecorder::MidiRecorder(MSXMotherBoard& motherBoard_)
	: motherBoard(motherBoard_)
	, command(std::make_unique<MidiRecordCmd>(motherBoard_, *this))
{
}

MidiRecorder::~MidiRecorder()
{
	if (recording) {
		try {
			stop(motherBoard.getCurrentTime());
		} catch (...) {
			// ignore, nothing sensible to do in a destructor
		}
	}
}

unsigned MidiRecorder::registerChip(Chip kind)
{
	auto& s = chips.emplace_back();
	s.kind = kind;
	s.clock = (kind == Chip::PSG) ? 1789772.0f : float(OPLL_CLOCK); // OPLL and SCC
	return unsigned(chips.size() - 1);
}

void MidiRecorder::opllReset(unsigned id, EmuTime time)
{
	auto& s = chips[id];
	s.regs.fill(0);
	if (!recording) return;
	log(id, 0x0E, 0, s.clock, time);
	for (unsigned r = 0x20; r <= 0x28; ++r) log(id, r, 0, s.clock, time);
}

void MidiRecorder::sccReset(unsigned id, EmuTime time)
{
	auto& s = chips[id];
	s.regs[0xAF] = 0;
	if (recording) log(id, 0xAF, 0, s.clock, time);
}

void MidiRecorder::log(unsigned id, unsigned reg, uint8_t value, float clock,
                       EmuTime time)
{
	double t = (time - startTime).toDouble();
	entries.push_back({t, clock, uint8_t(id), uint8_t(reg), value});
}

void MidiRecorder::start(const std::string& file, EmuTime time)
{
	if (recording) {
		throw CommandException("Already recording to ", filename);
	}
	// Check early that the file can be created.
	{
		std::ofstream test;
		FileOperations::openOfStream(test, file, std::ios::binary | std::ios::app);
		if (!test) throw CommandException("Cannot write to ", file);
	}
	filename = file;
	startTime = time;
	entries.clear();
	// Initial register state
	for (size_t i = 0; i < chips.size(); ++i) {
		const auto& s = chips[i];
		unsigned num = (s.kind == Chip::PSG) ? 14 : (s.kind == Chip::SCC) ? 0xB0 : 0x39;
		for (unsigned r = 0; r < num; ++r) {
			entries.push_back({0.0, s.clock, uint8_t(i), uint8_t(r), s.regs[r]});
		}
	}
	recording = true;
}

MidiRecorder::Result MidiRecorder::stop(EmuTime time)
{
	if (!recording) throw CommandException("Not recording.");
	recording = false;
	double duration = (time - startTime).toDouble();
	auto result = convertAndWrite(duration);
	entries.clear();
	entries.shrink_to_fit();
	return result;
}

MidiRecorder::Result MidiRecorder::convertAndWrite(double duration)
{
	Result result;
	result.filename = filename;
	result.seconds = std::round(duration * 100.0) / 100.0;
	uint32_t endTick = toTick(duration);
	uint64_t steps = uint64_t(duration / GRID_SECONDS) + 1;

	std::vector<Track> tracks;
	tracks.reserve(64); // keep references stable while converting

	using Group = Track::Group;
	size_t numPsg = 0, numScc = 0, numOpll = 0;
	for (const auto& c : chips) {
		(c.kind == Chip::PSG ? numPsg : c.kind == Chip::SCC ? numScc : numOpll)++;
	}
	size_t psgIndex = 0, sccIndex = 0, opllIndex = 0;

	// PSG first, then MSX-Music, so the channel order is always the same.
	for (auto kind : {Chip::PSG, Chip::SCC, Chip::OPLL})
	for (unsigned id = 0; id < chips.size(); ++id) {
		if (chips[id].kind != kind) continue;
		// Gather the log entries of this chip (chronological already).
		std::vector<const LogEntry*> list;
		for (const auto& e : entries) {
			if (e.chip == id) list.push_back(&e);
		}
		if (list.empty()) continue;
		size_t idx = 0;
		float clock = list.front()->clock;

		if (chips[id].kind == Chip::PSG) {
			// ------------------------------------------------ PSG ----
			auto suffix = (numPsg > 1) ? strCat(++psgIndex) : std::string();
			const std::array<std::string, 3> voiceNames = {"A", "B", "C"};
			std::array<Track*, 3> tt = {};
			for (unsigned v = 0; v < 3; ++v) {
				tracks.emplace_back(strCat("PSG", suffix, " ", voiceNames[v]),
				                    Group::PSG_SCC, true);
				tracks.back().setup(PROGRAM_SQUARE_LEAD,
				                    uint8_t(v == 0 ? 40 : v == 1 ? 64 : 88));
				tt[v] = &tracks.back();
			}
			tracks.emplace_back(strCat("PSG", suffix, " noise"), Group::PSG_SCC, false);
			Track* drumTrack = &tracks.back();

			std::array<uint8_t, 16> regs = {};
			double envStart = 0.0;
			std::array<ToneVoice, 3> tone;
			for (auto& v : tone) v.detailed = detail == Detail::FULL;
			std::array<DrumVoice, 3> noise;
			std::array<bool, 3> noiseWasOn = {};
			std::array<int, 3> pendDrum = {-1, -1, -1};
			std::array<int, 3> pendCount = {};

			for (uint64_t n = 0; n <= steps; ++n) {
				double g = double(n) * GRID_SECONDS;
				uint32_t tick = std::min(toTick(g), endTick);

				while (idx < list.size() && list[idx]->time <= g) {
					const auto& e = *list[idx++];
					clock = e.clock;
					if (e.reg < 16) {
						regs[e.reg] = e.value;
						if (e.reg == 13) envStart = e.time;
					}
				}

				unsigned mixer = regs[7];
				unsigned noisePeriod = regs[6] & 0x1F;
				unsigned envShape = regs[13] & 0x0F;
				unsigned envPeriod = std::max(1u, unsigned(regs[11] | (regs[12] << 8)));
				double envStepSeconds = 16.0 * envPeriod / double(clock);
				bool envRepeats = envShape >= 8 && !(envShape & 1);
				double envSteps = (envShape & 2) ? 32.0 : 16.0; // triangle: 2 ramps
				bool buzz = envRepeats &&
				            (16.0 * envStepSeconds < BUZZ_CYCLE_SECONDS);

				for (unsigned v = 0; v < 3; ++v) {
					bool toneOn  = !(mixer & (1u << v));
					bool noiseOn = !(mixer & (8u << v));
					unsigned period = regs[2 * v] | ((regs[2 * v + 1] & 0x0F) << 8);

					// Buzzer bass: repeating envelope, no noise, and either no tone
					// or a tone that is a harmonic of the envelope.
					bool usesEnv = regs[8 + v] & 0x10;
					bool envBass = false;
					double envFreq = 0.0;
					if (usesEnv && envRepeats && !noiseOn &&
					    envSteps * envStepSeconds < BUZZ_BASS_CYCLE_SECONDS) {
						envFreq = 1.0 / (envSteps * envStepSeconds);
						if (!toneOn) {
							envBass = true;
						} else if (period >= MIN_PERIOD) {
							double r = (double(clock) / (16.0 * period)) / envFreq;
							envBass = r >= 1.5 &&
							          std::abs(r - std::round(r)) <= 0.1 + 0.02 * r;
						}
					}

					unsigned level = regs[8 + v] & 0x0F;
					if (usesEnv) {
						if (buzz || envBass) {
							level = 11;
						} else {
							auto stepNo = uint64_t(std::max(0.0, g - envStart) / envStepSeconds);
							level = envelopeLevel(envShape, stepNo);
						}
					}
					bool active = (toneOn && level > 0 && period >= MIN_PERIOD) || envBass;
					double fine = 0.0;
					if (envBass) {
						fine = fineNoteForFrequency(envFreq);
					} else if (active) {
						fine = fineNoteForFrequency(double(clock) / (16.0 * period));
					}
					tone[v].update(*tt[v], tick, active, fine, level, false,
					               envBass ? PROGRAM_SYNTH_BASS : PROGRAM_SQUARE_LEAD);

					// ---- noise (drums) ----
					auto& nv = noise[v];
					bool nActive = noiseOn && level > 0;
					nv.tick(*drumTrack, tick);
					if (!nActive) {
						nv.release(*drumTrack, tick);
					} else {
						int drum = drumForNoise(noisePeriod);
						if (!noiseWasOn[v]) {
							nv.hit(*drumTrack, tick, drum, level);
							pendDrum[v] = -1;
						} else if (nv.drum >= 0 && drum != nv.drum) {
							// Different band: hit again, but only
							// once it is stable for a few steps.
							if (pendDrum[v] != drum) {
								pendDrum[v] = drum;
								pendCount[v] = 1;
							} else if (++pendCount[v] >= 3) {
								nv.hit(*drumTrack, tick, drum, level);
								pendDrum[v] = -1;
							}
						}
					}
					noiseWasOn[v] = nActive;
				}
			}
			for (unsigned v = 0; v < 3; ++v) {
				tone[v].finish(*tt[v], endTick);
				noise[v].release(*drumTrack, endTick);
			}
			if (sweepDrums || detail == Detail::SIMPLE) {
				for (unsigned v = 0; v < 3; ++v) {
					// Arpeggios first: their falling parts are not drums.
					collapseArpeggios(*tt[v]);
					sweepsToDrums(*tt[v], *drumTrack);
				}
			}
		} else if (chips[id].kind == Chip::SCC) {
			// ------------------------------------------------ SCC ----
			auto suffix = (numScc > 1) ? strCat(++sccIndex, " ") : std::string();
			std::array<Track*, 5> tt = {};
			for (unsigned c = 0; c < 5; ++c) {
				tracks.emplace_back(strCat("SCC ", suffix, c + 1), Group::PSG_SCC, true);
				tracks.back().setup(PROGRAM_SQUARE_LEAD, 64);
				tt[c] = &tracks.back();
			}
			std::array<uint8_t, 256> regs = {};
			std::array<bool, 5> dirty;
			dirty.fill(true);
			std::array<WaveInfo, 5> wave = {};
			std::array<ToneVoice, 5> tone;
			for (auto& v : tone) v.detailed = detail == Detail::FULL;

			for (uint64_t n = 0; n <= steps; ++n) {
				double g = double(n) * GRID_SECONDS;
				uint32_t tick = std::min(toTick(g), endTick);

				while (idx < list.size() && list[idx]->time <= g) {
					const auto& e = *list[idx++];
					regs[e.reg] = e.value;
					if (e.reg < 0xA0) dirty[e.reg >> 5] = true;
				}
				for (unsigned c = 0; c < 5; ++c) {
					if (dirty[c]) {
						wave[c] = classifyWave(&regs[c * 32]);
						dirty[c] = false;
					}
					unsigned period = regs[0xA0 + 2 * c] | ((regs[0xA1 + 2 * c] & 0x0F) << 8);
					unsigned vol = regs[0xAA + c] & 0x0F;
					bool enabled = regs[0xAF] & (1u << c);
					// quiet wave forms sound softer
					unsigned level = (vol * wave[c].peak + 64) / 128;
					if (vol > 0 && wave[c].peak > 0) level = std::max(level, 1u);
					bool active = enabled && level > 0 && period >= 8;
					double fine = 0.0;
					if (active) {
						fine = fineNoteForFrequency(double(clock) / (32.0 * (period + 1)));
					}
					tone[c].update(*tt[c], tick, active, fine, level, false, wave[c].program);
				}
			}
			for (unsigned c = 0; c < 5; ++c) tone[c].finish(*tt[c], endTick);
		} else {
			// ---------------------------------------------- OPLL ----
			auto suffix = (numOpll > 1) ? strCat(++opllIndex, " ") : std::string();
			std::array<Track*, 9> tt = {};
			for (unsigned c = 0; c < 9; ++c) {
				tracks.emplace_back(strCat("MSX-Music ", suffix, c + 1),
				                    Group::MSX_MUSIC, true);
				tracks.back().setup(OPLL_PROGRAMS[0], 64);
				tt[c] = &tracks.back();
			}
			tracks.emplace_back(strCat("MSX-Music ", suffix, "rhythm"),
			                    Group::MSX_MUSIC, false);
			Track* drumTrack = &tracks.back();

			std::array<uint8_t, 64> regs = {};
			std::array<unsigned, 9> keyEdges = {};
			std::array<unsigned, 9> lastKeyEdges = {};
			std::array<unsigned, 5> drumEdges = {};
			std::array<unsigned, 5> lastDrumEdges = {};
			std::array<ToneVoice, 9> tone;
			for (auto& v : tone) v.detailed = detail == Detail::FULL;
			std::array<DrumVoice, 5> drums;

			for (uint64_t n = 0; n <= steps; ++n) {
				double g = double(n) * GRID_SECONDS;
				uint32_t tick = std::min(toTick(g), endTick);

				while (idx < list.size() && list[idx]->time <= g) {
					const auto& e = *list[idx++];
					unsigned r = e.reg;
					if (r < 64) {
						if (r >= 0x20 && r <= 0x28) {
							// count key-on edges, so a quick
							// off/on between two grid points
							// still retriggers the note
							bool was = regs[r] & 0x10;
							if (!was && (e.value & 0x10)) ++keyEdges[r - 0x20];
						} else if (r == 0x0E) {
							for (unsigned b = 0; b < 5; ++b) {
								bool was = regs[r] & (1u << b);
								if (!was && (e.value & (1u << b))) ++drumEdges[b];
							}
						}
						regs[r] = e.value;
					}
				}

				bool rhythm = regs[0x0E] & 0x20;
				for (unsigned c = 0; c < 9; ++c) {
					bool keyOn = regs[0x20 + c] & 0x10;
					unsigned fnum = regs[0x10 + c] | ((regs[0x20 + c] & 1) << 8);
					unsigned block = (regs[0x20 + c] >> 1) & 7;
					unsigned inst = regs[0x30 + c] >> 4;
					unsigned att = regs[0x30 + c] & 15;
					unsigned level = 15 - att;
					bool melodic = !(rhythm && c >= 6);
					bool active = melodic && keyOn && fnum > 0 && level > 0;
					double fine = 0.0;
					if (active) {
						double freq = double(fnum) * double(1u << block)
						            * (OPLL_CLOCK / 72.0) / 524288.0;
						fine = fineNoteForFrequency(freq);
					}
					bool force = keyEdges[c] != lastKeyEdges[c];
					lastKeyEdges[c] = keyEdges[c];
					tone[c].update(*tt[c], tick, active, fine, level,
					               force && keyOn, OPLL_PROGRAMS[inst]);
				}

				// rhythm section
				std::array<unsigned, 5> att = {
					unsigned(regs[0x37] >> 4),   // hi-hat
					unsigned(regs[0x38] & 15),   // top cymbal
					unsigned(regs[0x38] >> 4),   // tom-tom
					unsigned(regs[0x37] & 15),   // snare drum
					unsigned(regs[0x36] & 15)};  // bass drum
				for (unsigned b = 0; b < 5; ++b) {
					bool on = rhythm && (regs[0x0E] & (1u << b));
					drums[b].tick(*drumTrack, tick);
					if (!on) {
						drums[b].release(*drumTrack, tick);
					} else if (drumEdges[b] != lastDrumEdges[b] || n == 0) {
						drums[b].hit(*drumTrack, tick, OPLL_DRUMS[b], 15 - att[b]);
					}
					lastDrumEdges[b] = drumEdges[b];
				}
			}
			for (unsigned c = 0; c < 9; ++c) tone[c].finish(*tt[c], endTick);
			for (auto& d : drums) d.release(*drumTrack, endTick);
		}
	}

	if (arpeggioChords || detail == Detail::SIMPLE) {
		for (auto& t : tracks) {
			if (t.melodic) collapseArpeggios(t);
		}
	}
	for (auto& t : tracks) {
		if (t.melodic) {
			dropShortNotes(t, detail == Detail::SIMPLE ? SIMPLE_MIN_NOTE_TICKS
			                                           : GLITCH_NOTE_TICKS);
		}
	}

	// ---- keep only the tracks that were used, assign MIDI channels ----
	size_t melodic[2] = {0, 0};
	for (const auto& t : tracks) {
		if (t.used == 0) continue;
		if (t.melodic) ++melodic[size_t(t.group)];
		(t.melodic ? result.notes : result.drums) += t.used;
	}
	bool split = melodic[0] > 0 && melodic[1] > 0 &&
	             melodic[0] + melodic[1] > MELODIC_CHANNELS;

	auto writeGroup = [&](std::optional<Group> only, const std::string& file) {
		std::vector<const Track*> out;
		size_t next = 0;
		for (auto& t : tracks) {
			if (t.used == 0) continue;
			if (only && t.group != *only) continue;
			if (t.melodic) {
				if (next >= MELODIC_CHANNELS) continue; // too many voices
				t.channel = int(next >= DRUM_CHANNEL ? next + 1 : next);
				++next;
			} else {
				t.channel = DRUM_CHANNEL;
			}
			out.push_back(&t);
		}
		writeMidiFile(file, out, endTick);
	};

	if (!split) {
		writeGroup(std::nullopt, filename);
	} else {
		auto pos = filename.rfind(MidiRecorder::MIDI_EXTENSION);
		std::string second = (pos != std::string::npos && pos + MidiRecorder::MIDI_EXTENSION.size() == filename.size())
			? filename.substr(0, pos) + "-msxmusic" + std::string(MidiRecorder::MIDI_EXTENSION)
			: filename + "-msxmusic";
		writeGroup(Group::PSG_SCC, filename);
		writeGroup(Group::MSX_MUSIC, second);
		result.filename2 = second;
	}
	return result;
}

} // namespace openmsx
