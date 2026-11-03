#ifndef MSXMIDITAP_HH
#define MSXMIDITAP_HH

#include "MSXDevice.hh"
#include "MidiConvert.hh"
#include "MidiOutConnector.hh"
#include "MidiRecorder.hh"
#include "Schedulable.hh"
#include "outer.hh"

#include <array>
#include <memory>
#include <vector>

namespace openmsx {

/** Cartridge without any I/O or memory that sends the activity of the sound
  * chips of the MSX to a MIDI out connector, in real time (like the MIDI-PAC
  * hardware does). The conversion is the same as the one of the MIDI export
  * (see MidiRecorder), but without the processing that needs to look ahead.
  *
  * Supports PSG, SCC and MSX-Music. The melodic voices get the MIDI channels
  * in the order the chips are first used (channel 10 is for the drums); the
  * voices that do not fit in the 15 channels are not sent.
  */
class MSXMidiTap final : public MSXDevice, private MidiRecorder::Listener
{
public:
	explicit MSXMidiTap(const DeviceConfig& config);
	~MSXMidiTap() override;

	void powerDown(EmuTime time) override;
	void reset(EmuTime time) override;

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

private:
	/** The converter and the MIDI tracks (used as event buffers) of one chip. */
	struct ChipState {
		ChipState(unsigned id_, MidiRecorder::Chip kind_) : id(id_), kind(kind_) {}
		virtual ~ChipState() = default;
		virtual void step(double g) = 0;
		virtual void finish() = 0;

		unsigned id;
		MidiRecorder::Chip kind;
		std::vector<midi::Track> tracks;
		std::vector<int> channels; // MIDI channel of every track, -1: none
		midi::Track* drums = nullptr; // not in 'tracks' for the PSG and OPLL
	};
	struct PsgState;
	struct SccState;
	struct OpllState;

	// MidiRecorder::Listener
	void psgWrite(unsigned id, unsigned reg, uint8_t value,
	              float clock, EmuTime time) override;
	void sccWrite(unsigned id, unsigned reg, uint8_t value, EmuTime time) override;
	void opllWrite(unsigned id, unsigned reg, uint8_t value, EmuTime time) override;
	void volumesChanged(EmuTime time) override;

	template<typename T> [[nodiscard]] T* find(unsigned id);
	template<typename T> T* create(unsigned id);
	/** The next free MIDI channel for a melodic voice (the drums have channel
	  * 10), or -1 when all are used. */
	[[nodiscard]] int allocateChannel();

	void step(EmuTime time);
	void flush(EmuTime time);
	void sendEvents(midi::Track& track, int channel, EmuTime time);
	void allNotesOff(EmuTime time);

private:
	struct SyncStep final : Schedulable {
		friend class MSXMidiTap;
		explicit SyncStep(Scheduler& s) : Schedulable(s) {}
		void executeUntil(EmuTime time) override {
			auto& tap = OUTER(MSXMidiTap, syncStep);
			tap.step(time);
		}
	} syncStep;

	MidiOutConnector outConnector;
	std::vector<std::unique_ptr<ChipState>> chips;
	int nextChannel = 0;
	EmuTime start;
	EmuTime nextStep;
};

} // namespace openmsx

#endif
