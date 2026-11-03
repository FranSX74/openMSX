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
		} else if (sub == "volume") {
			checkNumArgs(tokens, Between{3, 4}, "volume psg|scc|opll ?0-127?");
			auto chipName = tokens[2].getString();
			MidiRecorder::Chip chip;
			if (chipName == "psg") chip = MidiRecorder::Chip::PSG;
			else if (chipName == "scc") chip = MidiRecorder::Chip::SCC;
			else if (chipName == "opll" || chipName == "msx-music") chip = MidiRecorder::Chip::OPLL;
			else throw CommandException("Expected psg, scc or opll, got: ", chipName);
			if (tokens.size() == 4) {
				auto v = tokens[3].getInt(getInterpreter());
				if (v < 0 || v > 127) throw CommandException("Volume must be 0..127");
				recorder.setVolume(chip, uint8_t(v), motherBoard.getCurrentTime());
			}
			result = int(recorder.getVolume(chip));
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
		       "midi_record volume psg|scc|opll ?0-127?  channel volume (CC#7) of the voices of a\n"
		       "                              chip, to balance them (defaults 85, 120, 100)\n"
		       "midi_record arpeggio ?on|off?  write arpeggios as chords (on, default) or as\n"
		       "                              fast single notes (off)\n";
	}

	void tabCompletion(std::vector<std::string>& tokens) const override
	{
		if (tokens.size() == 2) {
			completeString(tokens, std::array<std::string_view, 8>{"start", "stop", "toggle", "status", "arpeggio", "detail", "percussion", "volume"});
		} else if (tokens.size() == 3 && tokens[1] == "percussion") {
			completeString(tokens, std::array<std::string_view, 2>{"on", "off"});
		} else if (tokens.size() == 3 && tokens[1] == "volume") {
			completeString(tokens, std::array<std::string_view, 3>{"psg", "scc", "opll"});
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
	for (auto* l : listeners) {
		l->opllWrite(id, 0x0E, 0, time);
		for (unsigned r = 0x20; r <= 0x28; ++r) l->opllWrite(id, r, 0, time);
	}
	if (!recording) return;
	log(id, 0x0E, 0, s.clock, time);
	for (unsigned r = 0x20; r <= 0x28; ++r) log(id, r, 0, s.clock, time);
}

void MidiRecorder::sccReset(unsigned id, EmuTime time)
{
	auto& s = chips[id];
	s.regs[0xAF] = 0;
	for (auto* l : listeners) l->sccWrite(id, 0xAF, 0, time);
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
				                    uint8_t(v == 0 ? 40 : v == 1 ? 64 : 88),
				                    volumes[unsigned(Chip::PSG)]);
				tt[v] = &tracks.back();
			}
			tracks.emplace_back(strCat("PSG", suffix, " noise"), Group::PSG_SCC, false);
			Track* drumTrack = &tracks.back();

			PsgConverter psg(detail == Detail::FULL);
			for (uint64_t n = 0; n <= steps; ++n) {
				double g = double(n) * GRID_SECONDS;
				uint32_t tick = std::min(toTick(g), endTick);

				while (idx < list.size() && list[idx]->time <= g) {
					const auto& e = *list[idx++];
					clock = e.clock;
					psg.write(e.reg, e.value, e.time);
				}
				psg.step(g, tick, clock, tt, *drumTrack);
			}
			psg.finish(endTick, tt, *drumTrack);
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
				tracks.back().setup(PROGRAM_SQUARE_LEAD, 64, volumes[unsigned(Chip::SCC)]);
				tt[c] = &tracks.back();
			}
			Track* sccDrums = nullptr;
			if (detectPercussion) {
				tracks.emplace_back(strCat("SCC ", suffix, "drums"), Group::PSG_SCC, false);
				sccDrums = &tracks.back();
			}
			SccConverter scc(detail == Detail::FULL);
			for (uint64_t n = 0; n <= steps; ++n) {
				double g = double(n) * GRID_SECONDS;
				uint32_t tick = std::min(toTick(g), endTick);

				while (idx < list.size() && list[idx]->time <= g) {
					const auto& e = *list[idx++];
					scc.write(e.reg, e.value);
				}
				scc.step(tick, clock, tt);
			}
			scc.finish(endTick, tt);
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
				tracks.back().setup(OPLL_PROGRAMS[0], 64, volumes[unsigned(Chip::OPLL)]);
				tt[c] = &tracks.back();
			}
			tracks.emplace_back(strCat("MSX-Music ", suffix, "rhythm"),
			                    Group::MSX_MUSIC, false);
			Track* drumTrack = &tracks.back();

			OpllConverter opll(detail == Detail::FULL);
			for (uint64_t n = 0; n <= steps; ++n) {
				double g = double(n) * GRID_SECONDS;
				uint32_t tick = std::min(toTick(g), endTick);

				while (idx < list.size() && list[idx]->time <= g) {
					const auto& e = *list[idx++];
					opll.write(e.reg, e.value);
				}
				opll.step(tick, tt, *drumTrack);
			}
			opll.finish(endTick, tt, *drumTrack);
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
