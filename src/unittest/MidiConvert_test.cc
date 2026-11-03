#include "catch.hpp"

#include "MidiConvert.hh"

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace openmsx;
using namespace openmsx::midi;

namespace {

Track makeTrack(bool melodic = true)
{
	return Track("test", Track::Group::PSG_SCC, melodic);
}

// Add a note (on at 'start', off at 'end').
void addNote(Track& t, uint32_t start, uint32_t end, uint8_t pitch)
{
	t.add(start, 0x90, pitch, 100);
	t.add(end,   0x80, pitch, 0);
	++t.used;
}

std::vector<Event> notesOn(const Track& t)
{
	std::vector<Event> result;
	for (const auto& e : t.events) {
		if (e.data[0] == 0x90) result.push_back(e);
	}
	return result;
}

} // namespace

TEST_CASE("MidiConvert: time and level conversions")
{
	SECTION("toTick") {
		CHECK(toTick(0.0) == 0);
		CHECK(toTick(1.0) == TICKS_PER_SECOND);
		CHECK(toTick(0.0204) == 20);
	}
	SECTION("exprForLevel") {
		CHECK(exprForLevel(0) == 0);
		CHECK(exprForLevel(15) == 127);
		for (unsigned i = 1; i <= 15; ++i) {
			CHECK(exprForLevel(i) > exprForLevel(i - 1));
		}
	}
	SECTION("velocityForLevel") {
		CHECK(velocityForLevel(0) >= 1); // never 0: that would be a note off
		CHECK(velocityForLevel(15) == 127);
		CHECK(velocityForLevel(8) > velocityForLevel(7));
	}
	SECTION("fineNoteForFrequency") {
		CHECK(fineNoteForFrequency(440.0) == Approx(69.0));
		CHECK(fineNoteForFrequency(880.0) == Approx(81.0));
		CHECK(fineNoteForFrequency(261.6256) == Approx(60.0).margin(0.001));
	}
}

TEST_CASE("MidiConvert: PSG envelope generator")
{
	SECTION("shape 0: one decay, then silent") {
		CHECK(envelopeLevel(0, 0) == 15);
		CHECK(envelopeLevel(0, 15) == 0);
		CHECK(envelopeLevel(0, 16) == 0);
		CHECK(envelopeLevel(0, 1000) == 0);
	}
	SECTION("shape 4: one attack, then silent") {
		CHECK(envelopeLevel(4, 0) == 0);
		CHECK(envelopeLevel(4, 15) == 15);
		CHECK(envelopeLevel(4, 16) == 0);
	}
	SECTION("shape 8: sawtooth down, repeated") {
		CHECK(envelopeLevel(8, 0) == 15);
		CHECK(envelopeLevel(8, 15) == 0);
		CHECK(envelopeLevel(8, 16) == 15);
		CHECK(envelopeLevel(8, 31) == 0);
	}
	SECTION("shape 10: triangle starting down") {
		CHECK(envelopeLevel(10, 0) == 15);
		CHECK(envelopeLevel(10, 15) == 0);
		CHECK(envelopeLevel(10, 16) == 0);
		CHECK(envelopeLevel(10, 31) == 15);
		CHECK(envelopeLevel(10, 32) == 15);
	}
	SECTION("shape 12: sawtooth up, repeated") {
		CHECK(envelopeLevel(12, 0) == 0);
		CHECK(envelopeLevel(12, 15) == 15);
		CHECK(envelopeLevel(12, 16) == 0);
	}
	SECTION("shape 14: triangle starting up") {
		CHECK(envelopeLevel(14, 0) == 0);
		CHECK(envelopeLevel(14, 15) == 15);
		CHECK(envelopeLevel(14, 16) == 15);
		CHECK(envelopeLevel(14, 31) == 0);
	}
	SECTION("shapes 9, 11, 13, 15: hold the final level") {
		CHECK(envelopeLevel(9,  100) == 0);
		CHECK(envelopeLevel(11, 100) == 15);
		CHECK(envelopeLevel(13, 100) == 15);
		CHECK(envelopeLevel(15, 100) == 0);
	}
}

TEST_CASE("MidiConvert: classifyWave")
{
	SECTION("silent wave") {
		std::array<uint8_t, 32> raw = {};
		CHECK(classifyWave(raw.data()).program == PROGRAM_SQUARE_LEAD);
		CHECK(classifyWave(raw.data()).peak == 0);
	}
	SECTION("square wave") {
		std::array<uint8_t, 32> raw;
		for (unsigned i = 0; i < 32; ++i) raw[i] = uint8_t(i < 16 ? 100 : -100);
		CHECK(classifyWave(raw.data()).program == 80);
	}
	SECTION("sawtooth wave") {
		std::array<uint8_t, 32> raw;
		for (unsigned i = 0; i < 32; ++i) raw[i] = uint8_t(int8_t(-128 + 8 * i));
		CHECK(classifyWave(raw.data()).program == 81);
	}
	SECTION("sine wave") {
		std::array<uint8_t, 32> raw;
		for (unsigned i = 0; i < 32; ++i) {
			raw[i] = uint8_t(int8_t(std::lround(100.0 * std::sin(2.0 * M_PI * i / 32.0))));
		}
		CHECK(classifyWave(raw.data()).program == 73);
		CHECK(classifyWave(raw.data()).peak > 0);
	}
}

TEST_CASE("MidiConvert: drumForNoise")
{
	CHECK(drumForNoise(0) == 42);
	CHECK(drumForNoise(1) == 42);
	CHECK(drumForNoise(3) == 46);
	CHECK(drumForNoise(8) == 38);
	CHECK(drumForNoise(31) == 36);
	// a longer noise period is never a higher pitched drum than before
	// (hi-hats, snare, toms, bass drum)
	CHECK(drumForNoise(5) == drumForNoise(8));
}

TEST_CASE("MidiConvert: opllPitchOffset")
{
	std::array<uint8_t, 64> regs = {};
	SECTION("ROM instruments") {
		CHECK(opllPitchOffset(1, regs) == Approx(0.0)); // violin 1/1
		CHECK(opllPitchOffset(2, regs) == Approx(0.0)); // guitar 3/1
		CHECK(opllPitchOffset(13, regs) == Approx(-12.0)); // synth bass: carrier MULT 1/2
	}
	SECTION("user patch") {
		// MULT 1/1
		regs[0x00] = 1; regs[0x01] = 1;
		CHECK(opllPitchOffset(0, regs) == Approx(0.0));
		// carrier 1/2 gives an octave lower
		regs[0x01] = 0;
		CHECK(opllPitchOffset(0, regs) == Approx(-12.0));
		// carrier 2: an octave higher, but a modulator with MULT 1 keeps the fundamental
		regs[0x01] = 2; regs[0x00] = 1;
		CHECK(opllPitchOffset(0, regs) == Approx(0.0));
		regs[0x00] = 2;
		CHECK(opllPitchOffset(0, regs) == Approx(12.0));
		// a weak modulator (high total level) is ignored
		regs[0x00] = 1; regs[0x02] = 0x3F;
		CHECK(opllPitchOffset(0, regs) == Approx(12.0));
	}
}

TEST_CASE("MidiConvert: variable length quantity")
{
	auto vlq = [](uint32_t v) {
		std::string s;
		putVlq(s, v);
		return s;
	};
	CHECK(vlq(0)          == std::string("\x00", 1));
	CHECK(vlq(0x7F)       == "\x7F");
	CHECK(vlq(0x80)       == std::string("\x81\x00", 2));
	CHECK(vlq(0x2000)     == std::string("\xC0\x00", 2));
	CHECK(vlq(0x3FFF)     == "\xFF\x7F");
	CHECK(vlq(0x4000)     == std::string("\x81\x80\x00", 3));
	CHECK(vlq(0x0FFFFFFF) == "\xFF\xFF\xFF\x7F");
}

TEST_CASE("MidiConvert: Standard MIDI File")
{
	auto t1 = makeTrack();
	t1.setup(80, 64);
	addNote(t1, 0, 500, 60);
	auto t2 = makeTrack();
	t2.setup(38, 64);
	addNote(t2, 100, 200, 40);

	auto file = buildMidiFile({&t1, &t2}, 1000);

	REQUIRE(file.size() > 14);
	CHECK(file.substr(0, 4) == "MThd");
	// header: length 6, format 1, 3 tracks (tempo + 2), division
	CHECK(uint8_t(file[7]) == 6);
	CHECK(uint8_t(file[9]) == 1);
	CHECK(uint8_t(file[11]) == 3);
	CHECK((uint8_t(file[12]) << 8 | uint8_t(file[13])) == int(MIDI_DIVISION));

	// every track chunk has a correct length and ends with an end-of-track
	size_t pos = 14;
	unsigned chunks = 0;
	while (pos < file.size()) {
		REQUIRE(pos + 8 <= file.size());
		CHECK(file.substr(pos, 4) == "MTrk");
		size_t len = (size_t(uint8_t(file[pos + 4])) << 24) |
		             (size_t(uint8_t(file[pos + 5])) << 16) |
		             (size_t(uint8_t(file[pos + 6])) <<  8) |
		              size_t(uint8_t(file[pos + 7]));
		REQUIRE(pos + 8 + len <= file.size());
		CHECK(file.substr(pos + 8 + len - 3, 3) == std::string("\xFF\x2F\x00", 3));
		pos += 8 + len;
		++chunks;
	}
	CHECK(pos == file.size());
	CHECK(chunks == 3);
}

TEST_CASE("MidiConvert: ToneVoice")
{
	const double A4 = 69.0;

	SECTION("a steady tone is one note") {
		auto t = makeTrack();
		ToneVoice v;
		for (unsigned i = 0; i < 100; ++i) {
			v.update(t, toTick(i * GRID_SECONDS), true, A4, 12, false, -1);
		}
		v.update(t, 500, false, 0.0, 0, false, -1);
		auto on = notesOn(t);
		REQUIRE(on.size() == 1);
		CHECK(on[0].data[1] == 69);
		CHECK(t.used == 1);
	}
	SECTION("a small deviation is a pitch bend, not a new note") {
		auto t = makeTrack();
		ToneVoice v;
		for (unsigned i = 0; i < 20; ++i) {
			v.update(t, i * 5, true, A4 + 0.5, 12, false, -1);
		}
		CHECK(notesOn(t).size() == 1);
		bool bend = false;
		for (const auto& e : t.events) bend |= (e.data[0] == 0xE0);
		CHECK(bend);
	}
	SECTION("a big jump is a new note") {
		auto t = makeTrack();
		ToneVoice v;
		v.update(t, 0, true, A4, 12, false, -1);
		v.update(t, 5, true, A4 + 7.0, 12, false, -1);
		CHECK(notesOn(t).size() == 2);
	}
	SECTION("a volume jump re-attacks the note (detailed only)") {
		for (bool detailed : {true, false}) {
			auto t = makeTrack();
			ToneVoice v;
			v.detailed = detailed;
			for (unsigned i = 0; i < 5; ++i) v.update(t, i * 5, true, A4, 4, false, -1);
			for (unsigned i = 5; i < 10; ++i) v.update(t, i * 5, true, A4, 12, false, -1);
			CHECK(notesOn(t).size() == (detailed ? 2u : 1u));
		}
	}
	SECTION("flutter of the volume re-attacks the note") {
		auto t = makeTrack();
		ToneVoice v;
		unsigned n = 0;
		for (unsigned i = 0; i < 20; ++i) {
			v.update(t, n++ * 5, true, A4, (i & 1) ? 13 : 15, false, -1);
		}
		CHECK(notesOn(t).size() >= 4);
	}
	SECTION("silence ends the note") {
		auto t = makeTrack();
		ToneVoice v;
		v.update(t, 0, true, A4, 12, false, -1);
		v.update(t, 5, false, 0.0, 0, false, -1);
		REQUIRE(t.events.size() >= 2);
		CHECK(t.events.back().data[0] == 0x80);
		// finish() has nothing left to do
		auto before = t.events.size();
		v.finish(t, 10);
		CHECK(t.events.size() == before);
	}
	SECTION("finish releases a sounding note") {
		auto t = makeTrack();
		ToneVoice v;
		v.update(t, 0, true, A4, 12, false, -1);
		v.finish(t, 50);
		CHECK(t.events.back().data[0] == 0x80);
		CHECK(t.events.back().tick == 50);
	}
	SECTION("program change only when it changes") {
		auto t = makeTrack();
		ToneVoice v;
		v.update(t, 0, true, A4, 12, false, 38);
		v.update(t, 5, true, A4, 12, false, 38);
		unsigned changes = 0;
		for (const auto& e : t.events) changes += (e.data[0] == 0xC0);
		CHECK(changes == 1);
	}
}

TEST_CASE("MidiConvert: DrumVoice")
{
	auto t = makeTrack(false);
	DrumVoice d;
	d.hit(t, 0, 38, 15);
	for (int i = 0; i < DRUM_MAX_STEPS + 2; ++i) d.tick(t, uint32_t(i * 5));
	// the hit is released by itself after a while
	REQUIRE(t.events.size() == 2);
	CHECK(t.events[0].data[0] == 0x90);
	CHECK(t.events[1].data[0] == 0x80);
	CHECK(t.used == 1);
	// a new hit replaces the previous one
	d.hit(t, 100, 36, 15);
	d.hit(t, 110, 38, 15);
	CHECK(t.events.size() == 2 + 3); // on, then off+on
}

TEST_CASE("MidiConvert: dropShortNotes")
{
	auto t = makeTrack();
	addNote(t, 100, 105, 60);  // 5 ticks: glitch
	addNote(t, 200, 400, 62);  // 200 ticks: kept
	addNote(t, 500, 540, 64);  // 40 ticks
	SECTION("glitch threshold") {
		dropShortNotes(t, GLITCH_NOTE_TICKS);
		auto on = notesOn(t);
		REQUIRE(on.size() == 2);
		CHECK(on[0].data[1] == 62);
		CHECK(on[1].data[1] == 64);
		CHECK(t.used == 2);
	}
	SECTION("simple threshold") {
		dropShortNotes(t, SIMPLE_MIN_NOTE_TICKS);
		auto on = notesOn(t);
		REQUIRE(on.size() == 1);
		CHECK(on[0].data[1] == 62);
		CHECK(t.used == 1);
		// note offs of removed notes are gone too
		unsigned offs = 0;
		for (const auto& e : t.events) offs += (e.data[0] == 0x80);
		CHECK(offs == 1);
	}
}

TEST_CASE("MidiConvert: collapseArpeggios")
{
	SECTION("a fast triad becomes one chord") {
		auto t = makeTrack();
		uint32_t tick = 1000;
		const uint8_t pitches[3] = {60, 64, 67};
		for (unsigned i = 0; i < 12; ++i) {
			addNote(t, tick, tick + 20, pitches[i % 3]);
			tick += 20;
		}
		collapseArpeggios(t);
		auto on = notesOn(t);
		// three simultaneous notes spanning the whole arpeggio
		REQUIRE(on.size() == 3);
		for (const auto& e : on) CHECK(e.tick == 1000);
		CHECK(t.used == 3);
	}
	SECTION("a slow melody is not touched") {
		auto t = makeTrack();
		for (unsigned i = 0; i < 8; ++i) {
			addNote(t, i * 300, i * 300 + 250, uint8_t(60 + (i % 3)));
		}
		auto before = t.events.size();
		collapseArpeggios(t);
		CHECK(t.events.size() == before);
		CHECK(notesOn(t).size() == 8);
	}
	SECTION("a fast scale (no repeated pitch) is not an arpeggio") {
		auto t = makeTrack();
		for (unsigned i = 0; i < 8; ++i) {
			addNote(t, i * 20, i * 20 + 20, uint8_t(60 + i));
		}
		collapseArpeggios(t);
		CHECK(notesOn(t).size() == 8);
	}
}

TEST_CASE("MidiConvert: sweepsToDrums")
{
	SECTION("a falling run of short notes becomes a drum hit") {
		auto voice = makeTrack();
		auto drums = makeTrack(false);
		// 6 notes of 20 ticks falling from 72 by 3 semitones each
		for (unsigned i = 0; i < 6; ++i) {
			addNote(voice, 1000 + i * 20, 1000 + i * 20 + 20, uint8_t(72 - 3 * i));
		}
		addNote(voice, 3000, 3500, 60); // a real note
		auto hits = sweepsToDrums(voice, drums);
		CHECK(hits == 1);
		auto on = notesOn(drums);
		REQUIRE(on.size() == 1);
		CHECK(on[0].tick == 1000);
		CHECK(on[0].data[1] == DRUM_SNARE); // starts at or above SWEEP_SNARE_MIN_PITCH
		auto rest = notesOn(voice);
		REQUIRE(rest.size() == 1);
		CHECK(rest[0].tick == 3000);
	}
	SECTION("a low starting pitch gives a bass drum") {
		auto voice = makeTrack();
		auto drums = makeTrack(false);
		for (unsigned i = 0; i < 6; ++i) {
			addNote(voice, i * 20, i * 20 + 20, uint8_t(50 - 3 * i));
		}
		CHECK(sweepsToDrums(voice, drums) == 1);
		auto on = notesOn(drums);
		REQUIRE(on.size() == 1);
		CHECK(on[0].data[1] == DRUM_BASS);
	}
	SECTION("a rising run is not a drum") {
		auto voice = makeTrack();
		auto drums = makeTrack(false);
		for (unsigned i = 0; i < 6; ++i) {
			addNote(voice, i * 20, i * 20 + 20, uint8_t(50 + 3 * i));
		}
		CHECK(sweepsToDrums(voice, drums) == 0);
		CHECK(notesOn(voice).size() == 6);
	}
	SECTION("long notes are not a drum") {
		auto voice = makeTrack();
		auto drums = makeTrack(false);
		for (unsigned i = 0; i < 6; ++i) {
			addNote(voice, i * 200, i * 200 + 200, uint8_t(72 - 3 * i));
		}
		CHECK(sweepsToDrums(voice, drums) == 0);
	}
}
