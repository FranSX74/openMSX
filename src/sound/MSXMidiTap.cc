#include "MSXMidiTap.hh"

#include "MSXMotherBoard.hh"

#include "serialize.hh"

#include <algorithm>

using namespace openmsx::midi;

namespace openmsx {

static constexpr double SCC_CLOCK = OPLL_CLOCK; // as logged by MidiRecorder

// -- the chips --

struct MSXMidiTap::PsgState final : ChipState {
	PsgState(unsigned id_, const std::array<int, 3>& ch, uint8_t volume)
		: ChipState(id_, MidiRecorder::Chip::PSG), converter(true), noise("noise", Track::Group::PSG_SCC, false) {
		static constexpr std::array<uint8_t, 3> pan = {40, 64, 88};
		static constexpr std::array<const char*, 3> name = {"A", "B", "C"};
		for (unsigned v = 0; v < 3; ++v) {
			tracks.emplace_back(name[v], Track::Group::PSG_SCC, true);
			tracks.back().setup(PROGRAM_SQUARE_LEAD, pan[v], volume);
			channels.push_back(ch[v]);
		}
		drums = &noise;
	}
	void step(double g, uint32_t tick) override {
		converter.step(g, tick, clock, {&tracks[0], &tracks[1], &tracks[2]}, *drums);
	}
	void finish(uint32_t tick) override {
		converter.finish(tick, {&tracks[0], &tracks[1], &tracks[2]}, *drums);
	}
	PsgConverter converter;
	float clock = 1789772.0f;
	Track noise;
};

struct MSXMidiTap::SccState final : ChipState {
	SccState(unsigned id_, const std::array<int, 5>& ch, uint8_t volume)
		: ChipState(id_, MidiRecorder::Chip::SCC), converter(true) {
		for (unsigned c = 0; c < 5; ++c) {
			tracks.emplace_back("SCC", Track::Group::PSG_SCC, true);
			tracks.back().setup(PROGRAM_SQUARE_LEAD, 64, volume);
			channels.push_back(ch[c]);
		}
	}
	std::array<Track*, 5> ptrs() {
		return {&tracks[0], &tracks[1], &tracks[2], &tracks[3], &tracks[4]};
	}
	void step(double /*g*/, uint32_t tick) override { converter.step(tick, float(SCC_CLOCK), ptrs()); }
	void finish(uint32_t tick) override { converter.finish(tick, ptrs()); }
	SccConverter converter;
};

struct MSXMidiTap::OpllState final : ChipState {
	OpllState(unsigned id_, const std::array<int, 9>& ch, uint8_t volume)
		: ChipState(id_, MidiRecorder::Chip::OPLL), converter(true), noise("rhythm", Track::Group::MSX_MUSIC, false) {
		for (unsigned c = 0; c < 9; ++c) {
			tracks.emplace_back("FM", Track::Group::MSX_MUSIC, true);
			tracks.back().setup(OPLL_PROGRAMS[0], 64, volume);
			channels.push_back(ch[c]);
		}
		drums = &noise;
	}
	std::array<Track*, 9> ptrs() {
		std::array<Track*, 9> r;
		for (unsigned c = 0; c < 9; ++c) r[c] = &tracks[c];
		return r;
	}
	void step(double /*g*/, uint32_t tick) override { converter.step(tick, ptrs(), *drums); }
	void finish(uint32_t tick) override { converter.finish(tick, ptrs(), *drums); }
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
	// do not leave the synthesizer with sounding notes
	allNotesOff(getCurrentTime());
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
		chips.push_back(std::make_unique<PsgState>(id, ch, getMotherBoard().getMidiRecorder().getVolume(MidiRecorder::Chip::PSG)));
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
		chips.push_back(std::make_unique<SccState>(id, ch, getMotherBoard().getMidiRecorder().getVolume(MidiRecorder::Chip::SCC)));
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
		chips.push_back(std::make_unique<OpllState>(id, ch, getMotherBoard().getMidiRecorder().getVolume(MidiRecorder::Chip::OPLL)));
		opll = static_cast<OpllState*>(chips.back().get());
	}
	opll->converter.write(reg, value);
}

uint32_t MSXMidiTap::tickOf(EmuTime time) const
{
	return toTick((time - start).toDouble());
}

void MSXMidiTap::volumesChanged(EmuTime time)
{
	const auto& recorder = getMotherBoard().getMidiRecorder();
	uint32_t tick = tickOf(time);
	for (auto& chip : chips) {
		auto volume = recorder.getVolume(chip->kind);
		for (size_t i = 0; i < chip->tracks.size(); ++i) {
			chip->tracks[i].add(tick, 0xB0, 7, volume);
		}
	}
	flush(time);
}

void MSXMidiTap::step(EmuTime time)
{
	double g = (time - start).toDouble();
	uint32_t tick = tickOf(time);
	for (auto& chip : chips) chip->step(g, tick);
	flush(time);
	nextStep = nextStep + EmuDuration::sec(GRID_SECONDS);
	syncStep.setSyncPoint(nextStep);
}

void MSXMidiTap::sendEvent(const Event& e, int channel, EmuTime time)
{
	if (channel < 0) return;
	outConnector.recvByte(uint8_t((e.data[0] & 0xF0) | channel), time);
	for (unsigned i = 1; i < e.size; ++i) {
		outConnector.recvByte(e.data[i], time);
	}
}

void MSXMidiTap::sendEvents(Track& track, int channel, EmuTime time)
{
	for (const auto& e : track.events) sendEvent(e, channel, time);
	track.events.clear();
}

void MSXMidiTap::flushClean(EmuTime time, uint32_t now, bool all)
{
	for (auto& chip : chips) {
		size_t n = chip->tracks.size() + (chip->drums ? 1 : 0);
		while (chip->lanes.size() < n) {
			// the drum lane is the last one
			chip->lanes.emplace_back(chip->lanes.size() < chip->tracks.size());
		}
		for (size_t i = 0; i < n; ++i) {
			bool isDrums = i >= chip->tracks.size();
			auto& track = isDrums ? *chip->drums : chip->tracks[i];
			auto& lane = chip->lanes[i];
			lane.cleaner.setArpeggio(getMotherBoard().getMidiRecorder().getArpeggioChords());
			for (const auto& e : track.events) lane.cleaner.push(e);
			track.events.clear();
			if (all) lane.cleaner.drain(lane.ready);
			else lane.cleaner.process(now, lane.ready);
			int channel = isDrums ? int(DRUM_CHANNEL) : chip->channels[i];
			while (!lane.ready.empty() &&
			       (all || lane.ready.front().tick + LIVE_LATENCY_TICKS <= now)) {
				sendEvent(lane.ready.front(), channel, time);
				lane.ready.pop_front();
			}
		}
	}
}

void MSXMidiTap::flush(EmuTime time)
{
	using LiveMode = MidiRecorder::LiveMode;
	if (getMotherBoard().getMidiRecorder().getLiveMode() == LiveMode::CLEAN) {
		flushClean(time, tickOf(time), false);
		cleanActive = true;
		return;
	}
	if (cleanActive) {
		// back to raw: whatever waits goes out now
		flushClean(time, tickOf(time), true);
		cleanActive = false;
	}
	for (auto& chip : chips) {
		for (size_t i = 0; i < chip->tracks.size(); ++i) {
			sendEvents(chip->tracks[i], chip->channels[i], time);
		}
		if (chip->drums) sendEvents(*chip->drums, int(DRUM_CHANNEL), time);
	}
}

void MSXMidiTap::allNotesOff(EmuTime time)
{
	uint32_t tick = tickOf(time);
	for (auto& chip : chips) chip->finish(tick);
	flush(time);
	if (cleanActive) flushClean(time, tick, true);
	// and for what the converters do not know about (notes cut short, a
	// connector plugged in while playing, ...): all sound off + all notes off
	for (uint8_t channel = 0; channel < 16; ++channel) {
		for (uint8_t controller : {uint8_t(120), uint8_t(123)}) {
			outConnector.recvByte(uint8_t(0xB0 | channel), time);
			outConnector.recvByte(controller, time);
			outConnector.recvByte(0, time);
		}
	}
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
