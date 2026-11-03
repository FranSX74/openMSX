#include "MSXMidiTap.hh"

#include "MSXMotherBoard.hh"

#include "serialize.hh"

#include <algorithm>

using namespace openmsx::midi;

namespace openmsx {

static constexpr unsigned MAX_PSG = 2; // the first two PSGs get MIDI channels

MSXMidiTap::MSXMidiTap(const DeviceConfig& config)
	: MSXDevice(config)
	, syncStep(getScheduler())
	, outConnector(MSXDevice::getPluggingController(), "MIDI-Tap-out")
	, start(getCurrentTime())
	, nextStep(start)
{
	getMotherBoard().getMidiRecorder().addListener(this);
	syncStep.setSyncPoint(nextStep);
}

MSXMidiTap::~MSXMidiTap()
{
	getMotherBoard().getMidiRecorder().removeListener(this);
}

void MSXMidiTap::psgWrite(unsigned id, unsigned reg, uint8_t value,
                          float clock, EmuTime time)
{
	auto it = std::ranges::find(psgs, id, [](const auto& p) { return p->id; });
	PsgState* psg;
	if (it != psgs.end()) {
		psg = it->get();
	} else {
		if (psgs.size() >= MAX_PSG) return;
		psg = psgs.emplace_back(std::make_unique<PsgState>(
			id, unsigned(psgs.size()))).get();
	}
	psg->clock = clock;
	psg->converter.write(reg, value, (time - start).toDouble());
}

void MSXMidiTap::step(EmuTime time)
{
	double g = (time - start).toDouble();
	for (auto& psg : psgs) {
		psg->converter.step(g, 0, psg->clock,
		                    {&psg->tracks[0], &psg->tracks[1], &psg->tracks[2]},
		                    psg->drums);
	}
	flush(time);
	nextStep = nextStep + EmuDuration::sec(GRID_SECONDS);
	syncStep.setSyncPoint(nextStep);
}

void MSXMidiTap::sendEvents(Track& track, uint8_t channel, EmuTime time)
{
	for (const auto& e : track.events) {
		outConnector.recvByte(uint8_t((e.data[0] & 0xF0) | channel), time);
		for (unsigned i = 1; i < e.size; ++i) {
			outConnector.recvByte(e.data[i], time);
		}
	}
	track.events.clear();
}

void MSXMidiTap::flush(EmuTime time)
{
	for (auto& psg : psgs) {
		for (unsigned v = 0; v < 3; ++v) {
			sendEvents(psg->tracks[v], uint8_t(3 * psg->index + v), time);
		}
		sendEvents(psg->drums, DRUM_CHANNEL, time);
	}
}

void MSXMidiTap::allNotesOff(EmuTime time)
{
	for (auto& psg : psgs) {
		psg->converter.finish(0, {&psg->tracks[0], &psg->tracks[1], &psg->tracks[2]},
		                      psg->drums);
	}
	flush(time);
}

void MSXMidiTap::powerDown(EmuTime time)
{
	allNotesOff(time);
}

void MSXMidiTap::reset(EmuTime time)
{
	allNotesOff(time);
}

template<typename Archive>
void MSXMidiTap::serialize(Archive& ar, unsigned /*version*/)
{
	ar.template serializeBase<MSXDevice>(*this);
	// The conversion state is not saved: after loading a state the notes
	// start again with the next register write.
	if constexpr (Archive::IS_LOADER) {
		psgs.clear();
		start = getCurrentTime();
		nextStep = start;
		syncStep.removeSyncPoints();
		syncStep.setSyncPoint(nextStep);
	}
}
INSTANTIATE_SERIALIZE_METHODS(MSXMidiTap);
REGISTER_MSXDEVICE(MSXMidiTap, "MidiTap");

} // namespace openmsx
