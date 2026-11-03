#ifndef MIDIRECORDER_HH
#define MIDIRECORDER_HH

#include "EmuTime.hh"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace openmsx {

class MSXMotherBoard;
class MidiRecordCmd;

/** Records the sound chip activity of the emulated MSX into Standard MIDI
  * File(s) (format 1, one track per chip channel).
  *
  * Supported chips: PSG (AY-3-8910/YM2149) and MSX-Music (YM2413).
  *
  * While recording, only the writes to the chip registers (together with their
  * exact emulation time) are logged. The conversion to MIDI events is done
  * offline when the recording is stopped, so it can look ahead and filter
  * (vibrato -> pitch bend, short glitches, ...).
  *
  * MIDI channels are assigned at the end to the voices that were actually
  * used. If more than 15 melodic voices were used, the result is split in two
  * files: PSG (+SCC) in the first and MSX-Music in the second.
  */
class MidiRecorder
{
public:
	static constexpr std::string_view MIDI_DIR = "midi";
	static constexpr std::string_view MIDI_EXTENSION = ".mid";

	enum class Chip : uint8_t { PSG, SCC, OPLL };
	/** FULL: as close to the chips as possible (repeated notes, flutter,
	  * vibrato as pitch bend). SIMPLE: fewer, longer notes, easier to edit. */
	enum class Detail : uint8_t { FULL, SIMPLE };

	struct Result {
		size_t notes = 0;
		size_t drums = 0;
		double seconds = 0.0;
		std::string filename;
		std::string filename2; // only filled when the result was split
	};

public:
	explicit MidiRecorder(MSXMotherBoard& motherBoard);
	~MidiRecorder();

	MidiRecorder(const MidiRecorder&) = delete;
	MidiRecorder& operator=(const MidiRecorder&) = delete;

	/** Every sound chip registers itself and gets an id to use in the
	  * write functions below. */
	[[nodiscard]] unsigned registerChip(Chip kind);

	/** Must be called for every register write to a PSG. This is cheap
	  * when not recording.
	  * @param clock PSG clock frequency in Hz (typically 1789772). */
	void psgWrite(unsigned id, unsigned reg, uint8_t value,
	              float clock, EmuTime time) {
		auto& s = chips[id];
		s.regs[reg & 15] = value;
		s.clock = clock;
		if (recording) log(id, reg & 15, value, clock, time);
	}

	/** Write to the YM2413 address (port=false) or data (port=true) port. */
	void opllWritePort(unsigned id, bool port, uint8_t value, EmuTime time) {
		auto& s = chips[id];
		if (!port) {
			s.latch = value & 0x3F;
			return;
		}
		opllWriteReg(id, s.latch, value, time);
	}
	/** Direct write to a YM2413 register. */
	void opllWriteReg(unsigned id, unsigned reg, uint8_t value, EmuTime time) {
		auto& s = chips[id];
		reg &= 0x3F;
		s.regs[reg] = value;
		if (recording) log(id, reg, value, s.clock, time);
	}
	void opllReset(unsigned id, EmuTime time);

	/** Write to an SCC register, in the normalized (SCC+) layout:
	  * 0x00..0x9F wave forms 1-5, 0xA0..0xA9 frequencies, 0xAA..0xAE
	  * volumes, 0xAF channel enable. */
	void sccWriteReg(unsigned id, unsigned reg, uint8_t value, EmuTime time) {
		auto& s = chips[id];
		s.regs[reg & 0xFF] = value;
		if (recording) log(id, reg & 0xFF, value, s.clock, time);
	}
	void sccReset(unsigned id, EmuTime time);

	/** Write arpeggios (fast cycles through a few pitches) as chords. */
	void setArpeggioChords(bool value) { arpeggioChords = value; }
	[[nodiscard]] bool getArpeggioChords() const { return arpeggioChords; }

	/** Write fast descending runs of short notes (a drum made with a falling
	  * tone) as drum hits. Always on in the simple detail. */
	void setSweepDrums(bool value) { sweepDrums = value; }
	[[nodiscard]] bool getSweepDrums() const { return sweepDrums; }

	void setDetail(Detail value) { detail = value; }
	[[nodiscard]] Detail getDetail() const { return detail; }

	[[nodiscard]] bool isRecording() const { return recording; }
	[[nodiscard]] const std::string& getFilename() const { return filename; }

	/** Start recording to the given file. Throws on error. */
	void start(const std::string& file, EmuTime time);
	/** Stop recording and write the MIDI file(s). Throws on error. */
	Result stop(EmuTime time);

private:
	struct LogEntry {
		double time; // seconds since start of recording
		float clock;
		uint8_t chip;
		uint8_t reg;
		uint8_t value;
	};
	struct ChipState {
		Chip kind = Chip::PSG;
		std::array<uint8_t, 256> regs = {};
		float clock = 1789772.0f;
		uint8_t latch = 0; // YM2413 address latch
	};

	void log(unsigned id, unsigned reg, uint8_t value, float clock,
	         EmuTime time);
	[[nodiscard]] Result convertAndWrite(double duration);

private:
	MSXMotherBoard& motherBoard;
	std::vector<ChipState> chips;
	std::vector<LogEntry> entries;
	std::string filename;
	EmuTime startTime = EmuTime::zero();
	bool recording = false;
	bool arpeggioChords = true;
	bool sweepDrums = false;
	Detail detail = Detail::FULL;
	std::unique_ptr<MidiRecordCmd> command;
};

} // namespace openmsx

#endif
