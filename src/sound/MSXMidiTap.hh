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
  * Currently only the PSG is supported.
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
	struct PsgState {
		PsgState(unsigned id_, unsigned index_)
			: id(id_), index(index_), converter(true)
			, tracks{midi::Track("A", midi::Track::Group::PSG_SCC, true),
			         midi::Track("B", midi::Track::Group::PSG_SCC, true),
			         midi::Track("C", midi::Track::Group::PSG_SCC, true)}
			, drums("noise", midi::Track::Group::PSG_SCC, false) {
			static constexpr std::array<uint8_t, 3> pan = {40, 64, 88};
			for (unsigned v = 0; v < 3; ++v) {
				tracks[v].setup(midi::PROGRAM_SQUARE_LEAD, pan[v]);
			}
		}
		unsigned id;
		unsigned index; // 0, 1: selects the MIDI channels
		float clock = 1789772.0f;
		midi::PsgConverter converter;
		std::array<midi::Track, 3> tracks;
		midi::Track drums;
	};

	// MidiRecorder::Listener
	void psgWrite(unsigned id, unsigned reg, uint8_t value,
	              float clock, EmuTime time) override;

	void step(EmuTime time);
	void flush(EmuTime time);
	void sendEvents(midi::Track& track, uint8_t channel, EmuTime time);
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
	std::vector<std::unique_ptr<PsgState>> psgs;
	EmuTime start;
	EmuTime nextStep;
};

} // namespace openmsx

#endif
