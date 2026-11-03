#include "MSXMidiTap.hh"

#include "MSXMotherBoard.hh"

#include "serialize.hh"

#include <algorithm>

using namespace openmsx::midi;

namespace openmsx {

static constexpr double SCC_CLOCK = OPLL_CLOCK; // as logged by MidiRecorder

// -- the chips --

struct MSXMidiTap::PsgState final : ChipState {
	PsgState(unsigned id_, const std::array<int, 3>& ch)
		: ChipState(id_), converter(true), noise("noise", Track::Group::PSG_SCC, false) {
		static constexpr std::array<uint8_t, 3> pan = {40, 64, 88};
		static constexpr std::array<const char*, 3> name = {"A", "B", "C"};
		for (unsigned v = 0; v < 3; ++v) {
			tracks.emplace_back(name[v], Track::Group::PSG_SCC, true);
			tracks.back().setup(PROGRAM_SQUARE_LEAD, pan[v]);
			channels.push_back(ch[v]);
		}
		drums = &noise;
	}
	void step(double g) override {
		converter.step(g, 0, clock, {&tracks[0], &tracks[1], &tracks[2]}, *drums);
	}
	void finish() override {
		converter.finish(0, {&tracks[0], &tracks[1], &tracks[2]}, *drums);
	}
	PsgConverter converter;
	float clock = 1789772.0f;
	Track noise;
};

struct MSXMidiTap::SccState final : ChipState {
	SccState(unsigned id_, const std::array<int, 5>& ch)
		: ChipState(id_), converter(true) {
		for (unsigned c = 0; c < 5; ++c) {
			tracks.emplace_back("SCC", Track::Group::PSG_SCC, true);
			tracks.back().setup(PROGRAM_SQUARE_LEAD, 64);
			channels.push_back(ch[c]);
		}
	}
	std::array<Track*, 5> ptrs() {
		return {&tracks[0], &tracks[1], &tracks[2], &tracks[3], &tracks[4]};
	}
	void step(double /*g*/) override { converter.step(0, float(SCC_CLOCK), ptrs()); }
	void finish() override { converter.finish(0, ptrs()); }
	SccConverter converter;
};

struct MSXMidiTap::OpllState final : ChipState {
	OpllState(unsigned id_, const std::array<int, 9>& ch)
		: ChipState(id_), converter(true), noise("rhythm", Track::Group::MSX_MUSIC, false) {
		for (unsigned c = 0; c < 9; ++c) {
			tracks.emplace_back("FM", Track::Group::MSX_MUSIC, true);
			tracks.back().setup(OPLL_PROGRAMS[0], 64);
			channels.push_back(ch[c]);
		}
		drums = &noise;
	}
	std::array<Track*, 9> ptrs() {
		std::array<Track*, 9> r;
		for (unsigned c = 0; c < 9; ++c) r[c] = &tracks[c];
		return r;
	}
	void step(double /*g*/) override { converter.step(0, ptrs(), *drums); }
	void finish() override { converter.finish(0, ptrs(), *drums); }
	OpllConverter converter;
	Track noise;
};

// -- the device --

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

int MSXMidiTap::allocateChannel()
{
	if (nextChannel == int(DRUM_CHANNEL)) ++nextChannel;
	return (nextChannel < 16) ? nextChannel++ : -1;
}

template<typename T>
T* MSXMidiTap::find(unsigned id)
{
	for (auto& c : chips) {
		if (c->id == id) {
			if (auto* t = dynamic_cast<T*>(c.get())) return t;
		}
	}
	return nullptr;
}

void MSXMidiTap::psgWrite(unsigned id, unsigned reg, uint8_t value,
                          float clock, EmuTime time)
{
	auto* psg = find<PsgState>(id);
	if (!psg) {
		std::array<int, 3> ch;
		for (auto& c : ch) c = allocateChannel();
		chips.push_back(std::make_unique<PsgState>(id, ch));
		psg = static_cast<PsgState*>(chips.back().get());
	}
	psg->clock = clock;
	psg->converter.write(reg, value, (time - start).toDouble());
}

void MSXMidiTap::sccWrite(unsigned id, unsigned reg, uint8_t value, EmuTime /*time*/)
{
	auto* scc = find<SccState>(id);
	if (!scc) {
		std::array<int, 5> ch;
		for (auto& c : ch) c = allocateChannel();
		chips.push_back(std::make_unique<SccState>(id, ch));
		scc = static_cast<SccState*>(chips.back().get());
	}
	scc->converter.write(reg, value);
}

void MSXMidiTap::opllWrite(unsigned id, unsigned reg, uint8_t value, EmuTime /*time*/)
{
	auto* opll = find<OpllState>(id);
	if (!opll) {
		std::array<int, 9> ch;
		for (auto& c : ch) c = allocateChannel();
		chips.push_back(std::make_unique<OpllState>(id, ch));
		opll = static_cast<OpllState*>(chips.back().get());
	}
	opll->converter.write(reg, value);
}

void MSXMidiTap::step(EmuTime time)
{
	double g = (time - start).toDouble();
	for (auto& chip : chips) chip->step(g);
	flush(time);
	nextStep = nextStep + EmuDuration::sec(GRID_SECONDS);
	syncStep.setSyncPoint(nextStep);
}

void MSXMidiTap::sendEvents(Track& track, int channel, EmuTime time)
{
	if (channel >= 0) {
		for (const auto& e : track.events) {
			outConnector.recvByte(uint8_t((e.data[0] & 0xF0) | channel), time);
			for (unsigned i = 1; i < e.size; ++i) {
				outConnector.recvByte(e.data[i], time);
			}
		}
	}
	track.events.clear();
}

void MSXMidiTap::flush(EmuTime time)
{
	for (auto& chip : chips) {
		for (size_t i = 0; i < chip->tracks.size(); ++i) {
			sendEvents(chip->tracks[i], chip->channels[i], time);
		}
		if (chip->drums) sendEvents(*chip->drums, int(DRUM_CHANNEL), time);
	}
}

void MSXMidiTap::allNotesOff(EmuTime time)
{
	for (auto& chip : chips) chip->finish();
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
		chips.clear();
		nextChannel = 0;
		start = getCurrentTime();
		nextStep = start;
		syncStep.removeSyncPoints();
		syncStep.setSyncPoint(nextStep);
	}
}
INSTANTIATE_SERIALIZE_METHODS(MSXMidiTap);
REGISTER_MSXDEVICE(MSXMidiTap, "MidiTap");

} // namespace openmsx
