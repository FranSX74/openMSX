#include "MidiConvert.hh"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace openmsx::midi {

// ---------------------------------------------------------------------------
// Helpers
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

// ---------------------------------------------------------------------------
// Voices
// ---------------------------------------------------------------------------

void ToneVoice::update(Track& t, uint32_t tick, bool active, double fine,
                       unsigned level, bool forceNew, int prog)
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

void ToneVoice::finish(Track& t, uint32_t tick) const
{
	if (note >= 0) t.add(tick, 0x80, uint8_t(note), 0);
}

void DrumVoice::hit(Track& t, uint32_t tick, int note, unsigned level)
{
	release(t, tick);
	t.add(tick, 0x90, uint8_t(note), velocityForLevel(level));
	drum = note;
	age = 0;
	++t.used;
}

void DrumVoice::release(Track& t, uint32_t tick)
{
	if (drum >= 0) {
		t.add(tick, 0x80, uint8_t(drum), 0);
		drum = -1;
	}
}

void DrumVoice::tick(Track& t, uint32_t tick)
{
	if (drum >= 0 && ++age >= DRUM_MAX_STEPS) release(t, tick);
}

PsgConverter::PsgConverter(bool detailed)
{
	for (auto& v : tone) v.detailed = detailed;
}

void PsgConverter::write(unsigned reg, uint8_t value, double time)
{
	if (reg < 16) {
		regs[reg] = value;
		if (reg == 13) envStart = time;
	}
}

void PsgConverter::step(double g, uint32_t tick, float clock,
                        const std::array<Track*, 3>& tones, Track& drums)
{
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
		tone[v].update(*tones[v], tick, active, fine, level, false,
		               envBass ? PROGRAM_SYNTH_BASS : PROGRAM_SQUARE_LEAD);

		// ---- noise (drums) ----
		auto& nv = noise[v];
		bool nActive = noiseOn && level > 0;
		nv.tick(drums, tick);
		if (!nActive) {
			nv.release(drums, tick);
		} else {
			int drum = drumForNoise(noisePeriod);
			if (!noiseWasOn[v]) {
				nv.hit(drums, tick, drum, level);
				pendDrum[v] = -1;
			} else if (nv.drum >= 0 && drum != nv.drum) {
				// Different band: hit again, but only
				// once it is stable for a few steps.
				if (pendDrum[v] != drum) {
					pendDrum[v] = drum;
					pendCount[v] = 1;
				} else if (++pendCount[v] >= 3) {
					nv.hit(drums, tick, drum, level);
					pendDrum[v] = -1;
				}
			}
		}
		noiseWasOn[v] = nActive;
	}
}

void PsgConverter::finish(uint32_t tick, const std::array<Track*, 3>& tones, Track& drums)
{
	for (unsigned v = 0; v < 3; ++v) {
		tone[v].finish(*tones[v], tick);
		noise[v].release(drums, tick);
	}
}

// ---------------------------------------------------------------------------
// Post-processing of a track
// ---------------------------------------------------------------------------

// Cut the notes into the runs and chords the arpeggio detection works on.
// 'emitChord(a, b)' is called for every segment notes[a..b] that may be a
// chord, 'newSegment(k)' for the first note of every segment (a segment also
// ends where a run of short notes ends).
template<typename Notes, typename EmitChord, typename NewSegment>
static void arpSegments(const Notes& notes, EmitChord emitChord, NewSegment newSegment)
{
	constexpr size_t NONE = size_t(-1);
	size_t i = 0;
	while (i < notes.size()) {
		// find a run of short notes that follow each other closely
		size_t j = i;
		auto fits = [&](size_t k) {
			return notes[k].off != NONE && notes[k].t1 - notes[k].t0 <= ARP_NOTE_MAX_TICKS;
		};
		if (!fits(i)) { newSegment(i); ++i; continue; }
		while (j + 1 < notes.size() && fits(j + 1) &&
		       notes[j + 1].t0 - notes[j].t1 <= ARP_GAP_MAX_TICKS) ++j;
		// cut the run into chords: a pitch that is not part of the current
		// cycle starts a new chord once the cycle has been repeated
		size_t segStart = i;
		newSegment(i);
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
				newSegment(k);
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
}

void collapseArpeggios(Track& t, const ArpContinuation* cont)
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
		// the rest of an arpeggio that was cut in pieces: it continues the
		// chord of the previous piece, however few notes are left
		bool continues = cont && a == 0 && b > a &&
		                 notes[0].t0 - cont->tick <= ARP_GAP_MAX_TICKS;
		for (size_t k = a; continues && k <= b; ++k) {
			continues = std::ranges::find(cont->pitches, notes[k].pitch) != cont->pitches.end();
		}
		if (!continues &&
		    (distinct < 2 || b - a + 1 < std::max(ARP_MIN_NOTES, 2 * distinct))) return;
		if (continues && distinct < 1) return;
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

	arpSegments(notes, emitChord, [](size_t) {});
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

// ---------------------------------------------------------------------------
// Real-time cleaning
// ---------------------------------------------------------------------------

namespace {
struct LiveNote {
	size_t on;
	size_t off; // SIZE_MAX: still sounding
	uint32_t t0, t1;
	uint8_t pitch;
};
constexpr size_t NO_EVENT = size_t(-1);
}

static std::vector<LiveNote> pairNotes(const std::vector<Event>& ev)
{
	std::vector<LiveNote> notes;
	std::array<size_t, 128> open;
	open.fill(NO_EVENT);
	for (size_t i = 0; i < ev.size(); ++i) {
		uint8_t status = ev[i].data[0] & 0xF0;
		if (status != 0x80 && status != 0x90) continue;
		uint8_t pitch = ev[i].data[1] & 0x7F;
		if (status == 0x90 && ev[i].data[2] != 0) {
			if (open[pitch] != NO_EVENT) continue;
			open[pitch] = notes.size();
			notes.push_back({i, NO_EVENT, ev[i].tick, ev[i].tick, pitch});
		} else if (open[pitch] != NO_EVENT) {
			notes[open[pitch]].off = i;
			notes[open[pitch]].t1 = ev[i].tick;
			open[pitch] = NO_EVENT;
		}
	}
	return notes;
}

size_t LiveCleaner::findSplit(uint32_t now, bool requireSafe) const
{
	auto notes = pairNotes(buf);
	// a note that is still sounding and is already too long for an arpeggio
	auto isLong = [&](const LiveNote& n) {
		return (n.off != NO_EVENT) ? n.t1 - n.t0 > ARP_NOTE_MAX_TICKS
		                           : now - n.t0 > ARP_NOTE_MAX_TICKS;
	};
	// the notes that start a segment of the arpeggio detection: a cut there
	// does not change what the detection finds
	std::vector<bool> segStart(notes.size() + 1, false);
	if (requireSafe) {
		std::vector<LiveNote> closed;
		for (const auto& n : notes) {
			if (n.off == NO_EVENT) break;
			closed.push_back(n);
		}
		arpSegments(closed, [](size_t, size_t) {}, [&](size_t k) { segStart[k] = true; });
	}
	auto safe = [&](size_t k, const LiveNote* prev, const LiveNote* next) {
		if (!requireSafe || !prev || isLong(*prev)) return true;
		if (next && k < notes.size() && notes[k].off != NO_EVENT && segStart[k]) return true;
		uint32_t gap = (next ? next->t0 : now) - prev->t1;
		return !(gap <= ARP_GAP_MAX_TICKS && (!next || !isLong(*next)));
	};
	size_t best = 0;
	const LiveNote* prev = nullptr;
	for (size_t k = 0; k < notes.size(); ++k) {
		const auto& n = notes[k];
		if (n.on > 0 && safe(k, prev, &n)) best = n.on;
		if (n.off == NO_EVENT) {
			// sounding: the events up to its note-on can go too
			if (isLong(n) && safe(k + 1, &n, nullptr)) best = n.on + 1;
			return best;
		}
		prev = &n;
	}
	if (safe(notes.size(), prev, nullptr)) best = buf.size();
	return best;
}

void LiveCleaner::finalize(size_t split, bool chopped, std::deque<Event>& out)
{
	Track piece("", Track::Group::PSG_SCC, true);
	piece.events.assign(buf.begin(), buf.begin() + ptrdiff_t(split));
	buf.erase(buf.begin(), buf.begin() + ptrdiff_t(split));
	ArpContinuation cont{chopPitches, chopTick};
	if (arpeggio) collapseArpeggios(piece, chopPitches.empty() ? nullptr : &cont);
	dropShortNotes(piece, GLITCH_NOTE_TICKS);

	for (const auto& e : piece.events) {
		uint8_t status = e.data[0] & 0xF0;
		if (status == 0x90 && e.data[2] != 0 && !chopPitches.empty() &&
		    e.tick - chopTick <= ARP_GAP_MAX_TICKS &&
		    std::ranges::find(chopPitches, e.data[1]) != chopPitches.end()) {
			// continuation of a chord: take back its note-off
			auto it = std::find_if(out.rbegin(), out.rend(), [&](const Event& o) {
				return (o.data[0] & 0xF0) == 0x80 && o.data[1] == e.data[1];
			});
			if (it != out.rend()) {
				out.erase(std::next(it).base());
				continue;
			}
		}
		out.push_back(e);
	}
	chopPitches.clear();
	if (chopped) {
		uint32_t last = 0;
		for (const auto& e : piece.events) {
			if ((e.data[0] & 0xF0) == 0x80) last = std::max(last, e.tick);
		}
		chopTick = last;
		for (const auto& e : piece.events) {
			if ((e.data[0] & 0xF0) == 0x80 && e.tick == last) chopPitches.push_back(e.data[1]);
		}
	}
}

void LiveCleaner::process(uint32_t now, std::deque<Event>& out)
{
	if (buf.empty()) return;
	if (!melodic) {
		drain(out);
		return;
	}
	if (size_t split = findSplit(now, true)) finalize(split, false, out);
	if (!buf.empty() && now - buf.front().tick > LIVE_MAX_HOLD_TICKS) {
		if (size_t split = findSplit(now, false)) finalize(split, true, out);
	}
}

void LiveCleaner::drain(std::deque<Event>& out)
{
	if (melodic) {
		finalize(buf.size(), false, out);
	} else {
		out.insert(out.end(), buf.begin(), buf.end());
		buf.clear();
	}
	chopPitches.clear();
}

// ---------------------------------------------------------------------------
// Standard MIDI File
// ---------------------------------------------------------------------------

void putVlq(std::string& s, uint32_t v)
{
	char buf[5];
	int n = 0;
	buf[n++] = char(v & 0x7F);
	while (v >>= 7) buf[n++] = char((v & 0x7F) | 0x80);
	while (n--) s.push_back(buf[n]);
}
static void put16(std::string& s, uint32_t v)
{
	s.push_back(char(v >> 8)); s.push_back(char(v));
}
static void put32(std::string& s, uint32_t v)
{
	put16(s, v >> 16); put16(s, v & 0xFFFF);
}

static void writeTrack(std::string& out, const Track& t, uint32_t endTick)
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

std::string buildMidiFile(const std::vector<const Track*>& tracks, uint32_t endTick)
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
	return out;
}

// ---------------------------------------------------------------------------
// Chip models
// ---------------------------------------------------------------------------

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

double opllPitchOffset(unsigned inst, const std::array<uint8_t, 64>& regs)
{
	unsigned mod, car;
	bool weakModulator = false;
	if (inst == 0) {
		mod = regs[0x00] & 15;
		car = regs[0x01] & 15;
		weakModulator = (regs[0x02] & 0x3F) >= OPLL_WEAK_MODULATOR_TL;
	} else {
		mod = OPLL_ROM_MULT[inst][0];
		car = OPLL_ROM_MULT[inst][1];
	}
	unsigned c = OPLL_MULT_HALVES[car];
	unsigned m = OPLL_MULT_HALVES[mod];
	unsigned g = weakModulator ? c : std::gcd(c, m);
	return 12.0 * std::log2(double(g) / 2.0);
}

SccConverter::SccConverter(bool detailed)
{
	dirty.fill(true);
	for (auto& v : tone) v.detailed = detailed;
}

void SccConverter::write(unsigned reg, uint8_t value)
{
	regs[reg] = value;
	if (reg < 0xA0) dirty[reg >> 5] = true;
	if (reg >= 0xA0 && reg < 0xAA) {
		// as in the SCC: the deformation register that is active at the time of
		// the frequency write decides how many bits of the period are used
		unsigned c = (reg - 0xA0) / 2;
		unsigned per = regs[0xA0 + 2 * c] | ((regs[0xA1 + 2 * c] & 0x0F) << 8);
		if (regs[0xB0] & 2) per &= 0xFF;
		else if (regs[0xB0] & 1) per >>= 8;
		effPeriod[c] = per;
	}
}

void SccConverter::step(uint32_t tick, float clock, const std::array<Track*, 5>& tones)
{
	for (unsigned c = 0; c < 5; ++c) {
		if (dirty[c]) {
			wave[c] = classifyWave(&regs[c * 32]);
			dirty[c] = false;
		}
		unsigned period = effPeriod[c];
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
		tone[c].update(*tones[c], tick, active, fine, level, false, wave[c].program);
	}
}

void SccConverter::finish(uint32_t tick, const std::array<Track*, 5>& tones)
{
	for (unsigned c = 0; c < 5; ++c) tone[c].finish(*tones[c], tick);
}

OpllConverter::OpllConverter(bool detailed)
{
	for (auto& v : tone) v.detailed = detailed;
}

void OpllConverter::write(unsigned r, uint8_t value)
{
	if (r >= 64) return;
	if (r >= 0x20 && r <= 0x28) {
		// count key-on edges, so a quick off/on between two grid points
		// still retriggers the note
		bool was = regs[r] & 0x10;
		if (!was && (value & 0x10)) ++keyEdges[r - 0x20];
	} else if (r == 0x0E) {
		for (unsigned b = 0; b < 5; ++b) {
			bool was = regs[r] & (1u << b);
			if (!was && (value & (1u << b))) ++drumEdges[b];
		}
	}
	regs[r] = value;
}

void OpllConverter::step(uint32_t tick, const std::array<Track*, 9>& tones, Track& drumTrack)
{
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
			fine += opllPitchOffset(inst, regs);
		}
		bool force = keyEdges[c] != lastKeyEdges[c];
		lastKeyEdges[c] = keyEdges[c];
		tone[c].update(*tones[c], tick, active, fine, level,
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
		drums[b].tick(drumTrack, tick);
		if (!on) {
			drums[b].release(drumTrack, tick);
		} else if (drumEdges[b] != lastDrumEdges[b] || first) {
			drums[b].hit(drumTrack, tick, OPLL_DRUMS[b], 15 - att[b]);
		}
		lastDrumEdges[b] = drumEdges[b];
	}
	first = false;
}

void OpllConverter::finish(uint32_t tick, const std::array<Track*, 9>& tones, Track& drumTrack)
{
	for (unsigned c = 0; c < 9; ++c) tone[c].finish(*tones[c], tick);
	for (auto& d : drums) d.release(drumTrack, tick);
}

} // namespace openmsx::midi
