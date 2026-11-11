#ifndef MSXMIDITAP_HH
#define MSXMIDITAP_HH

#include "MSXDevice.hh"
#include "MidiConvert.hh"
#include "MidiOutConnector.hh"
#include "MidiRecorder.hh"
#include "Schedulable.hh"
#include "outer.hh"

#include <array>
#include <deque>
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
  *
  * In the 'clean' mode (midi_record live clean) the output is delayed by
  * LIVE_LATENCY_TICKS, which lets it drop glitches and write arpeggios as
  * chords, as the export does.
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
		virtual void step(double g, uint32_t tick) = 0;
		virtual void finish(uint32_t tick) = 0;
		/** The register state (see PsgConverter::registers()). */
		[[nodiscard]] virtual std::vector<uint8_t> registers() const = 0;
		/** Chip dependent: the PSG envelope start, relative to 'g'. */
		[[nodiscard]] virtual double extra(double /*g*/) const { return 0.0; }
		virtual float getClock() const { return 0.0f; }
		/** Write the saved registers back. */
		virtual void restore(const std::vector<uint8_t>& regs, double extra, float clock) = 0;

		/** The chip has no MIDI channels yet: none of its voices played. */
		[[nodiscard]] bool pending() const {
			return !channels.empty() && channels[0] == PENDING;
		}

		unsigned id;
		MidiRecorder::Chip kind;
		std::vector<midi::Track> tracks;
		std::vector<int> channels; // MIDI channel of every track, -1: none, PENDING: not yet
		/** While pending: the last program, controllers and pitch bend of every
		  * track, sent before its first note. */
		std::vector<std::vector<midi::Event>> held;
		midi::Track* drums = nullptr; // not in 'tracks' for the PSG and OPLL

		/** Clean mode: the events of a track (the drums last) wait here to be
		  * cleaned, and then for their time to be sent. */
		struct Lane {
			Lane(bool melodic) : cleaner(melodic, true) {}
			midi::LiveCleaner cleaner;
			std::deque<midi::Event> ready;
		};
		std::vector<Lane> lanes;
	};
	/** What is saved of a chip. */
	struct SavedChip {
		template<typename Archive> void serialize(Archive& ar, unsigned version);
		unsigned id = 0;
		uint8_t kind = 0;
		float clock = 0.0f;
		double extra = 0.0;
		std::vector<int> channels;
		std::vector<uint8_t> regs;
	};
	void restoreChip(const SavedChip& saved);
	void findPredecessor();
	void restoreIfLoaded(EmuTime time);
	void applyRestore();
	/** Forget the conversion state and restart the output clock. */
	void restartOutput(EmuTime time);
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
	/** A chip gets its channels when one of its voices plays the first note,
	  * not at its first register write: a chip that is only initialized (at
	  * reset, or by a game that does not use it) takes no channels. */
	static constexpr int PENDING = -2;

	void step(EmuTime time);
	void flush(EmuTime time);
	/** An event of melodic track 'i' of 'chip'. */
	void deliver(ChipState& chip, size_t i, const midi::Event& e, EmuTime time);
	void sendEvent(const midi::Event& e, int channel, EmuTime time);
	/** Clean mode. 'all': send everything that is waiting. */
	void flushClean(EmuTime time, uint32_t now, bool all);
	[[nodiscard]] uint32_t tickOf(EmuTime time) const;
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
	std::vector<SavedChip> toRestore; // loaded, not applied yet
	/** All the MIDI Taps (of all the machines). */
	static inline std::vector<MSXMidiTap*> liveTaps;
	/** For a MIDI Tap that was loaded from a state: the one of the machine
	  * it replaces (see ~MSXMidiTap()), if it still exists. */
	MSXMidiTap* predecessor = nullptr;
	int nextChannel = 0;
	bool cleanActive = false;
	EmuTime start;
	EmuTime nextStep;
};

} // namespace openmsx

#endif
