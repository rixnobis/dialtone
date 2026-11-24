#include "voice_lines.hh"

#include "madnight.hh"

#include <psyqo/spu.hh>
#include <psyqo/xprintf.h>

// 1 in this many pick-ups speaks (forceh wanted to see how fast the voices grate: start at half)
static constexpr uint8_t SPEAK_ONE_IN = 2;
static constexpr uint16_t LINE_VOLUME = 0x2c00;
// instant attack, hold, a short release so a cut line does not click
static constexpr uint32_t LINE_ADSR = 0x1f00000f | (0x0aU << 16);

static inline uint16_t Read16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static inline uint32_t Read32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

psyqo::Coroutine<> VoiceLines::Load(void) {
	for (auto& v : m_callVoice) {
		v = -1;
	}
	auto& archive = g_madnightEngine.m_archiveHelper;
	auto index = co_await archive.LoadFile("VOICE/INDEX.BIN");
	const uint8_t* p = index.data();
	if (index.size() < 8 || Read32(p) != 0x58565444) { // "DTVX"
		printf("DIALTONE: no voice pack, callers stay quiet\n");
		co_return;
	}
	uint16_t count = Read16(p + 4), banks = Read16(p + 6);
	if (count > MAX_LINES || index.size() < 8u + count * 12u + banks * 4u) {
		printf("DIALTONE: voice pack index is damaged\n");
		co_return;
	}
	// INDEX.BIN is small; copy what we need before it goes
	Line lines[MAX_LINES];
	uint32_t bankSize[16];
	for (uint16_t i = 0; i < count; i++) {
		const uint8_t* e = p + 8 + i * 12;
		uint16_t rate = Read16(e + 2);
		uint32_t size = Read32(e + 8);
		lines[i] = {e[0], e[1], uint16_t(uint32_t(rate) * 4096 / psyqo::SPU::BASE_SAMPLE_RATE), SPU_ADDR + Read32(e + 4),
					uint16_t((size / 16 * 28 * 60 + rate - 1) / rate)};
	}
	for (uint16_t b = 0; b < banks && b < 16; b++) {
		bankSize[b] = Read32(p + 8 + count * 12 + b * 4);
	}
	index.clear();

	uint32_t addr = SPU_ADDR;
	for (uint16_t b = 0; b < banks && b < 16; b++) {
		char name[24];
		snprintf(name, sizeof(name), "VOICE/BANK%d.BIN", b);
		auto bank = co_await archive.LoadFile(name);
		uint32_t size = bank.size() & ~15u;
		if (size != bankSize[b] || addr + size > SPU_END) {
			printf("DIALTONE: voice bank %d unusable (%d bytes, want %d)\n", b, bank.size(), bankSize[b]);
			break;
		}
		psyqo::SPU::dmaWrite(addr, bank.data(), size);
		addr += size;
		bank.clear();
	}
	// keep only the lines whose bytes made it into sound RAM
	for (uint16_t i = 0; i < count; i++) {
		if (lines[i].addr + 16 <= addr && lines[i].voice < MAX_VOICES && lines[i].kind < KINDS) {
			m_lines[m_count++] = lines[i];
		}
	}
	m_bytes = addr - SPU_ADDR;
	printf("DIALTONE: voice pack %d lines, %d bytes of sound RAM\n", m_count, m_bytes);
}

uint32_t VoiceLines::Rand(uint32_t n) {
	// xorshift, its own: the game's rolls must not move because a line was played
	m_rng ^= m_rng << 13;
	m_rng ^= m_rng >> 17;
	m_rng ^= m_rng << 5;
	return n ? m_rng % n : 0;
}

int16_t VoiceLines::Find(uint8_t voice, Kind kind) const {
	for (uint8_t i = 0; i < m_count; i++) {
		if (m_lines[i].voice == voice && m_lines[i].kind == kind) {
			return i;
		}
	}
	return -1;
}

int16_t VoiceLines::PickOf(Kind kind) {
	int16_t options[MAX_LINES];
	uint8_t n = 0;
	for (uint8_t i = 0; i < m_count; i++) {
		if (m_lines[i].kind == kind && i != m_last) {
			options[n++] = i;
		}
	}
	return n ? options[Rand(n)] : -1;
}

void VoiceLines::Play(int16_t index) {
	if (index < 0) {
		return;
	}
	const auto& l = m_lines[index];
	psyqo::SPU::ChannelPlaybackConfig config;
	config.sampleRate.value = l.pitch;
	config.volumeLeft = LINE_VOLUME;
	config.volumeRight = LINE_VOLUME;
	config.adsr = LINE_ADSR;
	psyqo::SPU::playADPCM(SPU_VOICE, l.addr, config, true);
	m_playing = l.frames + 2;
	m_last = index;
	printf("DIALTONE: voice line %d kind=%d voice=%d frames=%d\n", index, l.kind, l.voice, l.frames);
}

void VoiceLines::PickUp(uint8_t simLine, Kind closing) {
	if (!Ready() || simLine >= sizeof(m_callVoice)) {
		return;
	}
	m_callVoice[simLine] = -1;
	// never two at once, and only some pick-ups speak
	if (Live() || Rand(SPEAK_ONE_IN) != 0) {
		return;
	}
	Kind first = closing == HANGUP ? HANGUP : GREET;
	// a caller who has both the opening line and the closing one this call will need
	uint8_t voices[MAX_VOICES];
	uint8_t n = 0;
	for (uint8_t v = 0; v < MAX_VOICES; v++) {
		int16_t a = Find(v, first);
		if (a >= 0 && a != m_last && (closing == HANGUP || Find(v, closing) >= 0)) {
			voices[n++] = v;
		}
	}
	if (!n) {
		return;
	}
	uint8_t v = voices[Rand(n)];
	m_callVoice[simLine] = closing == HANGUP ? -1 : int8_t(v);
	m_callClosing[simLine] = closing;
	Play(Find(v, first));
}

void VoiceLines::CallEnded(uint8_t simLine, bool sold) {
	if (simLine >= sizeof(m_callVoice) || m_callVoice[simLine] < 0) {
		return;
	}
	int16_t line = Find(m_callVoice[simLine], sold ? SALE : NOSALE);
	m_callVoice[simLine] = -1;
	if (line < 0 || line == m_last && m_playing) {
		return;
	}
	if (m_playing) {
		m_queued = line;
	} else {
		Play(line);
	}
}

void VoiceLines::Supervisor(Kind kind) {
	if (!Ready()) {
		return;
	}
	// the corp outranks a caller: cut whoever is talking
	m_queued = -1;
	Play(PickOf(kind));
}

void VoiceLines::Stop(void) {
	psyqo::SPU::silenceChannels(1u << SPU_VOICE);
	m_playing = 0;
	m_queued = -1;
	for (auto& v : m_callVoice) {
		v = -1;
	}
}

void VoiceLines::Tick(uint32_t frames) {
	if (!m_playing) {
		return;
	}
	m_playing = m_playing > frames ? m_playing - frames : 0;
	if (!m_playing && m_queued >= 0) {
		int16_t q = m_queued;
		m_queued = -1;
		Play(q);
	}
}
