#ifndef MIDIOUTSYNTH_HH
#define MIDIOUTSYNTH_HH

#include "MidiOutDevice.hh"

#include "FilenameSetting.hh"

#include <memory>
#include <vector>

namespace openmsx {

class MidiSynthSound;
class MSXMotherBoard;

/** A General MIDI synthesizer that plays a SoundFont (.sf2).
  *
  * It is a MIDI out device like the ones that send the MIDI to the host
  * (ALSA, CoreMIDI, Windows), so it can be plugged into any MIDI out
  * connector, for instance the 'MIDI-Tap-out' of the MIDI Tap extension or
  * the one of an MSX-MIDI cartridge. The audio is produced in the mixer of
  * the emulated machine, so it is in sync with the rest of the sound.
  *
  * The SoundFont is the file of the setting 'midi-out-synth-soundfont'.
  */
class MidiOutSynth final : public MidiOutDevice
{
public:
	explicit MidiOutSynth(MSXMotherBoard& motherBoard);
	~MidiOutSynth() override;

	// Pluggable
	void plugHelper(Connector& connector, EmuTime time) override;
	void unplugHelper(EmuTime time) override;
	[[nodiscard]] zstring_view getName() const override;
	[[nodiscard]] zstring_view getDescription() const override;

	template<typename Archive>
	void serialize(Archive& ar, unsigned version);

protected:
	// MidiOutDevice
	void recvMessage(const std::vector<uint8_t>& message, EmuTime time) override;

private:
	/** Load the SoundFont and start producing sound. Throws PlugException. */
	void start();
	void stop();

private:
	MSXMotherBoard& motherBoard;
	FilenameSetting soundFontSetting;
	std::unique_ptr<MidiSynthSound> sound;
};

} // namespace openmsx

#endif
