#include "MidiOutSynth.hh"

#include "DeviceConfig.hh"
#include "File.hh"
#include "FileOperations.hh"
#include "HardwareConfig.hh"
#include "MSXMotherBoard.hh"
#include "PlugException.hh"
#include "SoundDevice.hh"
#include "XMLElement.hh"
#include "serialize.hh"

#include "narrow.hh"

// TinySoundFont (MIT license), see src/3rdparty/TinySoundFont/tsf.h
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wuseless-cast"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wfloat-equal"
#pragma GCC diagnostic ignored "-Wundef"
#endif
#define TSF_IMPLEMENTATION
#include "tsf.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include <array>
#include <cassert>
#include <span>

namespace openmsx {

static constexpr unsigned MAX_VOICES = 128;
// TinySoundFont renders at full scale, which would clip with many notes.
static constexpr float GAIN_DB = -12.0f;
static constexpr unsigned DRUM_CHANNEL = 9; // MIDI channel 10

// The sound device in the mixer of the machine: it renders the SoundFont.
class MidiSynthSound final : public SoundDevice
{
public:
	MidiSynthSound(MSXMotherBoard& motherBoard, tsf* font_);
	~MidiSynthSound();

	/** Play a complete MIDI message. */
	void message(std::span<const uint8_t> msg, EmuTime time);

	// SoundDevice
	void setOutputRate(unsigned hostSampleRate, double speed) override;
	bool updateBuffer(size_t length, float* buffer, EmuTime time) override;

protected:
	[[nodiscard]] float getAmplificationFactorImpl() const override {
		return 1.0f; // already in [-1, 1]
	}
	void generateChannels(std::span<float*> buffers, unsigned num) override;

private:
	void reset();

private:
	tsf* font;
	unsigned sampleRate = 44100;
};

static DeviceConfig makeConfig(MSXMotherBoard& motherBoard)
{
	static XMLElement* xml = [] {
		auto& doc = XMLDocument::getStaticDocument();
		XMLElement* result = doc.allocateElement("midisynth");
		// 32767: no extra scaling (the volume setting of the device does that)
		result->setFirstChild(doc.allocateElement("sound"))
		      ->setFirstChild(doc.allocateElement("volume", "32767"));
		return result;
	}();
	assert(motherBoard.getMachineConfig());
	return DeviceConfig(*motherBoard.getMachineConfig(), *xml);
}

MidiSynthSound::MidiSynthSound(MSXMotherBoard& motherBoard, tsf* font_)
	: SoundDevice(motherBoard.getMSXMixer(), "MIDI-synth",
	              "General MIDI synthesizer (SoundFont)",
	              1, 44100, true)
	, font(font_)
{
	tsf_set_max_voices(font, MAX_VOICES);
	reset();
	registerSound(makeConfig(motherBoard));
}

MidiSynthSound::~MidiSynthSound()
{
	unregisterSound();
	tsf_close(font);
}

void MidiSynthSound::reset()
{
	tsf_reset(font);
	// channel 10 plays the drum kits
	tsf_channel_set_bank_preset(font, DRUM_CHANNEL, 128, 0);
}

void MidiSynthSound::setOutputRate(unsigned hostSampleRate, double /*speed*/)
{
	sampleRate = hostSampleRate;
	setInputRate(hostSampleRate);
	tsf_set_output(font, TSF_STEREO_INTERLEAVED, narrow<int>(hostSampleRate), GAIN_DB);
}

void MidiSynthSound::message(std::span<const uint8_t> msg, EmuTime time)
{
	if (msg.empty()) return;
	// Produce the sound until now, so the message acts at the right moment.
	updateStream(time);

	uint8_t status = msg[0];
	if (status >= 0xF0) {
		// System messages. Only 'General MIDI System On' is handled.
		static constexpr std::array<uint8_t, 6> gmOn = {0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7};
		if (msg.size() >= gmOn.size() &&
		    std::equal(gmOn.begin(), gmOn.end(), msg.begin())) {
			reset();
		}
		return;
	}
	int channel = status & 0x0F;
	auto data = [&](size_t i) { return (i < msg.size()) ? int(msg[i] & 0x7F) : 0; };
	switch (status & 0xF0) {
	case 0x80:
		tsf_channel_note_off(font, channel, data(1));
		break;
	case 0x90:
		if (int vel = data(2); vel == 0) {
			tsf_channel_note_off(font, channel, data(1));
		} else {
			tsf_channel_note_on(font, channel, data(1), narrow<float>(vel) * (1.0f / 127.0f));
		}
		break;
	case 0xB0:
		// bank select, volume, pan, expression, sustain, RPN/data entry,
		// all sound off, all notes off, reset all controllers
		tsf_channel_midi_control(font, channel, data(1), data(2));
		break;
	case 0xC0:
		tsf_channel_set_presetnumber(font, channel, data(1), channel == int(DRUM_CHANNEL));
		break;
	case 0xE0:
		tsf_channel_set_pitchwheel(font, channel, data(1) | (data(2) << 7));
		break;
	default:
		// polyphonic and channel aftertouch: not supported by the synth
		break;
	}
}

void MidiSynthSound::generateChannels(std::span<float*> buffers, unsigned num)
{
	assert(buffers.size() == 1);
	if (tsf_active_voice_count(font) == 0) {
		buffers[0] = nullptr; // silence
		return;
	}
	// stereo interleaved: 'num' frames
	tsf_render_float(font, buffers[0], narrow<int>(num), 0);
}

bool MidiSynthSound::updateBuffer(size_t length, float* buffer, EmuTime /*time*/)
{
	return mixChannels(buffer, length);
}


// MidiOutSynth ==============================================================

MidiOutSynth::MidiOutSynth(MSXMotherBoard& motherBoard_)
	: motherBoard(motherBoard_)
	, soundFontSetting(
		motherBoard_.getCommandController(), "midi-out-synth-soundfont",
		"SoundFont (.sf2) file played by the 'midi-out-synth' pluggable", "")
{
}

MidiOutSynth::~MidiOutSynth() = default;

void MidiOutSynth::start()
{
	std::string filename = FileOperations::expandTilde(std::string(soundFontSetting.getString()));
	if (filename.empty()) {
		throw PlugException("No SoundFont: set the 'midi-out-synth-soundfont' setting "
		                    "to a .sf2 file first");
	}
	tsf* font = nullptr;
	try {
		File file(filename);
		auto mapped = file.mmap<const uint8_t>();
		font = tsf_load_memory(mapped.data(), narrow<int>(mapped.size()));
	} catch (MSXException& e) {
		throw PlugException("Cannot read the SoundFont '", filename, "': ", e.getMessage());
	}
	if (!font) {
		throw PlugException("'", filename, "' is not a valid SoundFont (.sf2) file");
	}
	sound = std::make_unique<MidiSynthSound>(motherBoard, font);
}

void MidiOutSynth::stop()
{
	sound.reset();
}

void MidiOutSynth::plugHelper(Connector& /*connector*/, EmuTime /*time*/)
{
	start();
}

void MidiOutSynth::unplugHelper(EmuTime /*time*/)
{
	clearBuffer();
	stop();
}

zstring_view MidiOutSynth::getName() const
{
	return "midi-out-synth";
}

zstring_view MidiOutSynth::getDescription() const
{
	return "General MIDI synthesizer that plays a SoundFont. Plug it into a "
	       "MIDI out connector to hear the MIDI inside openMSX. The SoundFont "
	       "(.sf2 file) is set with the 'midi-out-synth-soundfont' setting.";
}

void MidiOutSynth::recvMessage(const std::vector<uint8_t>& message, EmuTime time)
{
	if (sound) sound->message(message, time);
}

template<typename Archive>
void MidiOutSynth::serialize(Archive& /*ar*/, unsigned /*version*/)
{
	// The notes that sound are not saved. After a loadstate or a reverse the
	// connector is plugged again without plugHelper(): load the SoundFont
	// again (the synthesizer starts silent).
	if constexpr (Archive::IS_LOADER) {
		start();
	}
}
INSTANTIATE_SERIALIZE_METHODS(MidiOutSynth);
REGISTER_POLYMORPHIC_INITIALIZER(Pluggable, MidiOutSynth, "MidiOutSynth");

} // namespace openmsx
