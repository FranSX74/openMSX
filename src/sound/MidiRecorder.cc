#include "MidiRecorder.hh"

#include "MidiConvert.hh"

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
#include <optional>

namespace openmsx {

namespace {

void writeMidiFile(const std::string& filename,
                   const std::vector<const midi::Track*>& tracks, uint32_t endTick)
{
	auto out = midi::buildMidiFile(tracks, endTick);
	std::ofstream file;
	FileOperations::openOfStream(file, filename, std::ios::binary | std::ios::trunc);
	if (!file) throw CommandException("Cannot write to ", filename);
	file.write(out.data(), std::streamsize(out.size()));
	if (!file) throw CommandException("Error writing ", filename);
}

} // namespace

using namespace midi;

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
		} else if (sub == "percussion") {
			checkNumArgs(tokens, Between{2, 3}, "percussion ?on|off?");
			if (tokens.size() == 3) {
				auto v = tokens[2].getString();
				if (v == "on") recorder.setDetectPercussion(true);
				else if (v == "off") recorder.setDetectPercussion(false);
				else throw CommandException("Expected on or off, got: ", v);
			}
			result = recorder.getDetectPercussion() ? "on" : "off";
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
		       "midi_record percussion ?on|off?  detect drums made with a fast falling tone and\n"
		       "                              write them as drum hits (default: off, plain notes)\n"
		       "midi_record arpeggio ?on|off?  write arpeggios as chords (on, default) or as\n"
		       "                              fast single notes (off)\n";
	}

	void tabCompletion(std::vector<std::string>& tokens) const override
	{
		if (tokens.size() == 2) {
			completeString(tokens, std::array<std::string_view, 7>{"start", "stop", "toggle", "status", "arpeggio", "detail", "percussion"});
		} else if (tokens.size() == 3 && tokens[1] == "percussion") {
			completeString(tokens, std::array<std::string_view, 2>{"on", "off"});
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
		unsigned num = (s.kind == Chip::PSG) ? 14 : (s.kind == Chip::SCC) ? 0xB1 : 0x39;
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
			if (detectPercussion) {
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
			Track* sccDrums = nullptr;
			if (detectPercussion) {
				tracks.emplace_back(strCat("SCC ", suffix, "drums"), Group::PSG_SCC, false);
				sccDrums = &tracks.back();
			}
			std::array<uint8_t, 256> regs = {};
			std::array<bool, 5> dirty;
			dirty.fill(true);
			std::array<unsigned, 5> effPeriod = {}; // period after the deformation register
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
					if (e.reg >= 0xA0 && e.reg < 0xAA) {
						// as in the SCC: the deformation register that is active at the time of
						// the frequency write decides how many bits of the period are used
						unsigned c = (e.reg - 0xA0) / 2;
						unsigned per = regs[0xA0 + 2 * c] | ((regs[0xA1 + 2 * c] & 0x0F) << 8);
						if (regs[0xB0] & 2) per &= 0xFF;
						else if (regs[0xB0] & 1) per >>= 8;
						effPeriod[c] = per;
					}
				}
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
					tone[c].update(*tt[c], tick, active, fine, level, false, wave[c].program);
				}
			}
			for (unsigned c = 0; c < 5; ++c) tone[c].finish(*tt[c], endTick);
			if (sccDrums) {
				for (unsigned c = 0; c < 5; ++c) {
					collapseArpeggios(*tt[c]);
					sweepsToDrums(*tt[c], *sccDrums);
				}
			}
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
						fine += opllPitchOffset(inst, regs);
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
