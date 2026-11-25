#include "voice_lines.hh"

#include "madnight.hh"

#include <EASTL/string_view.h>
#include <psyqo-paths/archive-manager.hh>
#include <psyqo/spu.hh>
#include <psyqo/xprintf.h>

#ifdef PCDRV
#include <psyqo/cdrom-pcdrv.hh>
#endif

static constexpr uint16_t LINE_VOLUME = 0x2c00;
// instant attack, hold, a short release so a cut line does not click
static constexpr uint32_t LINE_ADSR = 0x1f00000f | (0x0aU << 16);
static constexpr uint32_t ARCHIVE_LBA = 23; // where the authoring tool puts the archive
static constexpr uint32_t SECTOR = 2048;

#ifndef DIALTONE_STREAM_DELAY
#define DIALTONE_STREAM_DELAY 0
#endif

// One sector-aligned read buffer: reads run one at a time, each take fits a slot part.
alignas(4) static uint8_t s_read[VoiceLines::PART_BYTES];

// The CD device. A PCDRV build reads the disc image through its own PCdrv handle; a disc build
// shares the engine's CDRomDevice, which MadnightEngine keeps private. The engine stays as it is, so
// the member is reached through an explicit template instantiation, which C++ exempts from access
// checks ([temp.spec.general]).
#ifdef PCDRV
static psyqo::CDRomPCDrv s_pcdrv(g_gameIsoName);
static psyqo::CDRom& Device(void) { return s_pcdrv; }
#else
namespace {
struct EngineCDRom {
	using type = CDRomHelper MadnightEngine::*;
	friend type Get(EngineCDRom);
};
template <typename Tag, typename Tag::type M> struct Reach {
	friend typename Tag::type Get(Tag) { return M; }
};
template struct Reach<EngineCDRom, &MadnightEngine::m_cdromHelper>;
} // namespace
static psyqo::CDRom& Device(void) { return (g_madnightEngine.*Get(EngineCDRom{})).CDRomDevice(); }
#endif

static inline uint16_t Read16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static inline uint32_t Read32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

// VINDEX.BIN rows (tools/pack_voices.py)
static constexpr uint32_t HEADER = 16, VOICE_ROW = 4, GROUP_ROW = 4, LINE_ROW = 4, TAKE_ROW = 6, RES_ROW = 8;

psyqo::Coroutine<> VoiceLines::Load(void) {
	m_delay = DIALTONE_STREAM_DELAY;
	auto& archive = g_madnightEngine.m_archiveHelper;
	m_index = co_await archive.LoadFile("VOICE/VINDEX.BIN");
	const uint8_t* p = m_index.data();
	if (m_index.size() < HEADER || Read32(p) != 0x32565444) { // "DTV2"
		printf("DIALTONE: no voice pack, callers stay quiet\n");
		m_index.clear();
		co_return;
	}
	uint16_t voices = Read16(p + 4), lines = Read16(p + 6), takes = Read16(p + 8), residents = Read16(p + 12);
	uint8_t streams = p[10], kinds = p[11];
	m_rate = Read16(p + 14);
	uint32_t tables = HEADER + voices * VOICE_ROW + voices * kinds * GROUP_ROW + lines * LINE_ROW + takes * TAKE_ROW +
					  residents * RES_ROW;
	if (kinds != KINDS || streams > 100 || m_index.size() < tables + 1 ||
		m_index.size() < tables + 1 + p[tables] * 4u) {
		printf("DIALTONE: voice pack index is damaged\n");
		m_index.clear();
		co_return;
	}
	m_voices = p + HEADER;
	m_groups = m_voices + voices * VOICE_ROW;
	m_lines = m_groups + voices * kinds * GROUP_ROW;
	m_takes = m_lines + lines * LINE_ROW;
	m_residents = m_takes + takes * TAKE_ROW;
	m_voiceCount = voices;
	m_kindCount = kinds;
	m_pitch = uint32_t(m_rate) * 4096 / psyqo::SPU::BASE_SAMPLE_RATE;
	for (uint16_t i = 0; i < voices && m_supVoice < 0; i++) {
		if (Read16(m_groups + (i * kinds + SUP_WELCOME) * GROUP_ROW + 2)) {
			m_supVoice = i;
		}
	}

	// the stream files: where each one starts on the disc, from the archive's own index
	psyqo::paths::ArchiveManager manager;
	if (co_await manager.initialize(ARCHIVE_LBA, Device())) {
		for (uint8_t f = 0; f < streams; f++) {
			char name[24];
			snprintf(name, sizeof(name), "VOICE/STRM%02d.BIN", f);
			auto* entry = manager.getIndexEntry(eastl::string_view(name));
			if (!entry || entry->getCompressionMethod() != psyqo::paths::ArchiveManager::IndexEntry::Method::NONE) {
				// a compressed stream file cannot be read a sector at a time; the toc must store it
				printf("DIALTONE: voice stream %s %s\n", name, entry ? "is compressed" : "missing");
				break;
			}
			m_streamLba[f] = manager.getIndexEntrySectorStart(entry);
			m_streamCount = f + 1;
		}
	}
	if (m_streamCount == streams) {
		m_lineCount = lines;
		m_takeCount = takes;
	}

	// the resident lines, to sound RAM for good
	const uint8_t* files = m_index.data() + tables;
	uint32_t addr = RESIDENT_ADDR;
	bool resident = true;
	for (uint8_t f = 0; f < files[0]; f++) {
		char name[24];
		snprintf(name, sizeof(name), "VOICE/RES%d.BIN", f);
		auto bank = co_await archive.LoadFile(name);
		uint32_t size = Read32(files + 1 + f * 4);
		if (bank.size() != size || addr + size > SPU_END) {
			printf("DIALTONE: resident voice bank %d unusable (%d bytes, want %d)\n", f, bank.size(), size);
			resident = false;
			break;
		}
		psyqo::SPU::dmaWrite(addr, bank.data(), size);
		addr += size;
	}
	m_residentCount = resident ? residents : 0;
	printf("DIALTONE: voice pack %d voices %d lines %d takes in %d stream files, %d resident lines %d bytes\n",
		   m_voiceCount, m_lineCount, m_takeCount, m_streamCount, m_residentCount, addr - RESIDENT_ADDR);
}

uint32_t VoiceLines::Rand(uint32_t n) {
	// xorshift, its own: the game's rolls must not move because a line was played
	m_rng ^= m_rng << 13;
	m_rng ^= m_rng >> 17;
	m_rng ^= m_rng << 5;
	return n ? m_rng % n : 0;
}

int16_t VoiceLines::PickLine(uint8_t voice, Kind kind) {
	if (!m_lineCount) {
		return -1;
	}
	const uint8_t* g = m_groups + (voice * m_kindCount + kind) * GROUP_ROW;
	uint16_t first = Read16(g), count = Read16(g + 2);
	if (!count) {
		return -1;
	}
	uint16_t pick = first + Rand(count);
	// never the same line twice running
	if (pick == m_lastLine && count > 1) {
		pick = first + (pick - first + 1) % count;
	}
	m_lastLine = pick;
	return pick;
}

int16_t VoiceLines::FindResident(uint8_t voice, Kind kind) const {
	for (uint16_t i = 0; i < m_residentCount; i++) {
		const uint8_t* r = m_residents + i * RES_ROW;
		if (r[0] == voice && r[1] == kind) {
			return i;
		}
	}
	return -1;
}

int8_t VoiceLines::PickVoice(Tier tier, uint16_t number, bool resident) {
	// the number decides who answers it, so a number called again is the same person
	uint8_t options[255];
	uint8_t n = 0;
	for (uint8_t pass = 0; pass < 2 && !n; pass++) {
		for (uint16_t v = 0; v < m_voiceCount; v++) {
			Tier t = Tier(m_voices[v * VOICE_ROW]);
			if (t == CORPORATE || (pass == 0 && t != tier)) {
				continue;
			}
			bool has = resident ? FindResident(v, GREET) >= 0
								: m_lineCount && Read16(m_groups + (v * m_kindCount + GREET) * GROUP_ROW + 2);
			if (has) {
				options[n++] = v;
			}
		}
	}
	return n ? options[uint32_t(number) * 40503u % 65521u % n] : -1;
}

VoiceLines::Plan* VoiceLines::PlanFor(uint8_t simLine) {
	for (auto& p : m_plans) {
		if (p.simLine == int8_t(simLine)) {
			return &p;
		}
	}
	return nullptr;
}

void VoiceLines::Ring(uint8_t slot, uint8_t simLine, uint16_t number, Tier tier, Kind closing, bool again) {
	if (!Ready() || slot >= SLOTS) {
		return;
	}
	// a new call on the slot: whatever it was saying is over, and its reads are void
	if (m_playing[slot]) {
		psyqo::SPU::silenceChannels(1u << (SPU_VOICE + slot));
		m_playing[slot] = 0;
	}
	m_pendingClose[slot] = -1;
	m_gen[slot]++;
	for (auto& p : m_plans) {
		if (p.simLine == int8_t(simLine)) {
			p = Plan();
		}
	}
	Plan& plan = m_plans[slot];
	plan = Plan();
	plan.simLine = simLine;
	plan.closing = closing;
	plan.tier = tier;
	plan.number = number;

	int8_t voice = PickVoice(tier, number, false);
	if (voice < 0) {
		return; // nothing to stream: the pick-up takes a resident line
	}
	plan.voice = voice;
	auto has = [&](Kind k) { return Read16(m_groups + (voice * m_kindCount + k) * GROUP_ROW + 2) != 0; };
	if (again && has(CALLBACK) && Rand(2) == 0) {
		plan.opening = CALLBACK;
	} else if (closing == HANGUP) {
		plan.opening = HANGUP;
	} else if (has(ODD) && Rand(6) == 0) {
		plan.opening = ODD;
	} else {
		plan.opening = GREET;
	}
	int16_t open = PickLine(voice, plan.opening);
	int16_t close = closing == HANGUP ? -1 : PickLine(voice, closing);
	if (open >= 0) {
		const uint8_t* l = m_lines + open * LINE_ROW;
		plan.take[0] = Read16(l) + Rand(l[2]);
		Queue(slot, 0);
	}
	if (close >= 0) {
		const uint8_t* l = m_lines + close * LINE_ROW;
		plan.take[1] = Read16(l) + Rand(l[2]);
		Queue(slot, 1);
	}
	printf("DIALTONE: voice plan slot=%d line=%d number=%d voice=%d open=%d line %d take %d close=%d line %d take %d\n",
		   slot, simLine, number, voice, plan.opening, open, plan.take[0], closing, close, plan.take[1]);
}

void VoiceLines::Queue(uint8_t slot, uint8_t part) {
	if (m_jobCount >= sizeof(m_jobs) / sizeof(m_jobs[0])) {
		return; // stays None: the pick-up falls back to a resident line
	}
	m_plans[slot].part[part] = Part::Queued;
	m_jobs[m_jobCount++] = {slot, part, m_gen[slot], m_plans[slot].take[part]};
	NextRead();
}

void VoiceLines::NextRead(void) {
	while (!m_reading && m_jobCount) {
		Job job = m_jobs[0];
		for (uint8_t i = 1; i < m_jobCount; i++) {
			m_jobs[i - 1] = m_jobs[i];
		}
		m_jobCount--;
		if (job.gen != m_gen[job.slot]) {
			continue; // its call is gone
		}
		m_current = job;
		m_reading = true;
		m_wait = m_delay;
		if (!m_wait) {
			Issue();
		}
	}
}

void VoiceLines::Issue(void) {
	const Job& job = m_current;
	if (job.gen != m_gen[job.slot]) {
		m_reading = false;
		NextRead();
		return;
	}
	const uint8_t* t = m_takes + job.take * TAKE_ROW;
	uint32_t lba = m_streamLba[t[0]] + Read16(t + 2);
	uint32_t sectors = (Read16(t + 4) * 16u + SECTOR - 1) / SECTOR;
	m_plans[job.slot].part[job.part] = Part::Reading;
	m_readStart = m_frame;
	Device().readSectors(lba, sectors, s_read, [this](bool ok) { ReadDone(ok); });
}

void VoiceLines::ReadDone(bool ok) {
	Job job = m_current;
	m_reading = false;
	if (job.gen == m_gen[job.slot]) {
		Plan& plan = m_plans[job.slot];
		if (ok) {
			uint32_t bytes = Read16(m_takes + job.take * TAKE_ROW + 4) * 16u;
			psyqo::SPU::dmaWrite(SPU_ADDR + (job.slot * 2 + job.part) * PART_BYTES, s_read, bytes);
			plan.part[job.part] = Part::Ready;
		} else {
			plan.part[job.part] = Part::None;
		}
		printf("DIALTONE: voice read slot=%d part=%d take=%d ok=%d frames=%d\n", job.slot, job.part, job.take, ok,
			   m_frame - m_readStart);
	}
	NextRead();
}

void VoiceLines::Fallback(uint8_t slot) {
	Plan& plan = m_plans[slot];
	m_gen[slot]++; // its reads, done or not, are no longer wanted: the voice changes
	plan.part[0] = plan.part[1] = Part::None;
	int8_t voice = PickVoice(plan.tier, plan.number, true);
	if (voice >= 0) {
		plan.resident[0] = FindResident(voice, plan.closing == HANGUP ? HANGUP : GREET);
		plan.resident[1] = plan.closing == HANGUP ? -1 : FindResident(voice, plan.closing);
	}
	plan.fellBack = true;
	printf("DIALTONE: voice fallback slot=%d held=%d resident voice=%d lines %d %d\n", slot, plan.held, voice,
		   plan.resident[0], plan.resident[1]);
}

bool VoiceLines::HoldPickUp(uint8_t simLine) {
	Plan* plan = PlanFor(simLine);
	if (!plan || plan->fellBack || plan->part[0] == Part::Ready) {
		if (plan && plan->held) {
			printf("DIALTONE: voice hold over slot=%d held=%d\n", int(plan - m_plans), plan->held);
		}
		return false;
	}
	uint8_t slot = plan - m_plans;
	if (plan->part[0] == Part::None || ++plan->held >= MAX_HOLD) {
		Fallback(slot);
		return false;
	}
	if (plan->held == 1) {
		printf("DIALTONE: voice hold slot=%d line=%d\n", slot, simLine);
	}
	return true;
}

void VoiceLines::PlayAt(uint8_t slot, uint32_t addr, uint32_t bytes, uint16_t volume) {
	psyqo::SPU::ChannelPlaybackConfig config;
	config.sampleRate.value = m_pitch;
	config.volumeLeft = volume;
	config.volumeRight = volume;
	config.adsr = LINE_ADSR;
	psyqo::SPU::playADPCM(SPU_VOICE + slot, addr, config, true);
	m_playing[slot] = (bytes / 16 * 28 * 60 + m_rate - 1) / m_rate + 2;
}

static uint16_t SlotVolume(uint8_t slot) { return slot == 0 ? LINE_VOLUME : LINE_VOLUME / 2; }

void VoiceLines::PlayPart(uint8_t slot, uint8_t part) {
	const Plan& plan = m_plans[slot];
	uint32_t bytes = Read16(m_takes + plan.take[part] * TAKE_ROW + 4) * 16u;
	PlayAt(slot, SPU_ADDR + (slot * 2 + part) * PART_BYTES, bytes, SlotVolume(slot));
	printf("DIALTONE: voice line slot=%d part=%d take=%d frames=%d\n", slot, part, plan.take[part], m_playing[slot]);
}

void VoiceLines::PlayResident(uint8_t slot, int16_t index, uint16_t volume) {
	if (index < 0) {
		return;
	}
	const uint8_t* r = m_residents + index * RES_ROW;
	PlayAt(slot, RESIDENT_ADDR + Read32(r + 4), Read16(r + 2) * 16u, volume);
	printf("DIALTONE: voice resident slot=%d index=%d voice=%d kind=%d frames=%d\n", slot, index, r[0], r[1],
		   m_playing[slot]);
}

void VoiceLines::PickUp(uint8_t simLine) {
	Plan* plan = PlanFor(simLine);
	if (!plan) {
		return;
	}
	uint8_t slot = plan - m_plans;
	if (plan->part[0] != Part::Ready && !plan->fellBack) {
		Fallback(slot); // a pick-up the sim did not hold for
	}
	if (plan->part[0] == Part::Ready) {
		PlayPart(slot, 0);
		plan->spoke = true;
	} else if (plan->resident[0] >= 0) {
		PlayResident(slot, plan->resident[0], SlotVolume(slot));
		plan->spoke = true;
	}
}

void VoiceLines::CallEnded(uint8_t simLine) {
	Plan* plan = PlanFor(simLine);
	if (!plan) {
		return;
	}
	uint8_t slot = plan - m_plans;
	int8_t close = -1;
	if (plan->spoke && plan->closing != HANGUP) {
		if (plan->part[1] == Part::Ready) {
			close = 0;
		} else if (plan->fellBack && plan->resident[1] >= 0) {
			close = 1;
		}
	}
	if (close >= 0) {
		if (m_playing[slot]) {
			m_pendingClose[slot] = close;
		} else {
			PlayClose(slot, close);
		}
	}
	// the call is done; the slot's audio stays until its next call rings
	plan->simLine = -1;
}

void VoiceLines::PlayClose(uint8_t slot, int8_t which) {
	if (which == 0) {
		PlayPart(slot, 1);
	} else {
		PlayResident(slot, m_plans[slot].resident[1], SlotVolume(slot));
	}
}

void VoiceLines::Supervisor(Kind kind) {
	if (!m_residentCount) {
		return;
	}
	// the corp outranks a caller: cut whoever is on your line
	m_pendingClose[0] = -1;
	for (uint16_t i = 0; i < m_residentCount; i++) {
		if (m_residents[i * RES_ROW + 1] == kind) {
			PlayResident(0, i, LINE_VOLUME);
			return;
		}
	}
}

void VoiceLines::Stop(void) {
	psyqo::SPU::silenceChannels(((1u << SLOTS) - 1) << SPU_VOICE);
	m_jobCount = 0;
	for (uint8_t s = 0; s < SLOTS; s++) {
		m_playing[s] = 0;
		m_pendingClose[s] = -1;
		m_gen[s]++;
		m_plans[s] = Plan();
	}
}

bool VoiceLines::Live(void) const {
	for (uint8_t s = 0; s < SLOTS; s++) {
		if (m_playing[s] || m_pendingClose[s] >= 0) {
			return true;
		}
	}
	return false;
}

void VoiceLines::Tick(uint32_t frames) {
	m_frame += frames;
	if (m_reading && m_wait) {
		m_wait = m_wait > frames ? m_wait - frames : 0;
		if (!m_wait) {
			Issue();
		}
	}
	for (uint8_t s = 0; s < SLOTS; s++) {
		if (!m_playing[s]) {
			continue;
		}
		m_playing[s] = m_playing[s] > frames ? m_playing[s] - frames : 0;
		if (!m_playing[s] && m_pendingClose[s] >= 0) {
			int8_t which = m_pendingClose[s];
			m_pendingClose[s] = -1;
			PlayClose(s, which);
		}
	}
}
