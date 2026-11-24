#include "musak.hh"

#include <common/hardware/spu.h>
#include <psyqo/spu.hh>

#include "musak_data.inc"

// 100 bpm: a 16th note is 9 frames at 60 Hz
static constexpr uint8_t FRAMES_PER_STEP = 9;
// overall level, scaled per track: the tones sit at 0x0e00-0x1400 per voice, this stays well under
static constexpr uint16_t c_trackLevel[Musak::TRACKS] = {0x0700, 0x0280, 0x0280, 0x0280, 0x0280, 0x0500, 0x0180, 0x0200};
// stereo placement, left share out of 16
static constexpr uint8_t c_trackPan[Musak::TRACKS] = {8, 10, 11, 5, 6, 5, 11, 9};
static constexpr uint8_t c_trackInstrument[Musak::TRACKS] = {2, 0, 0, 0, 0, 1, 3, 4};
// ducked under a voice line: about -10 dB, down in ~8 frames and back up over ~half a second
static constexpr uint16_t DUCK_GAIN = 80, DUCK_DOWN = 22, DUCK_UP = 6;

void Musak::Init(void) {
	uint32_t addr = SPU_ADDR;
	for (uint8_t i = 0; i < sizeof(c_instruments) / sizeof(c_instruments[0]); i++) {
		psyqo::SPU::dmaWrite(addr, c_instruments[i].data, c_instruments[i].size);
		m_addr[i] = addr;
		addr += (c_instruments[i].size + 15) & ~15;
	}
	static_assert(SPU_ADDR + MUSAK_SAMPLE_BYTES <= 0x7f000, "musak samples run into the tones");
	m_ready = true;
}

void Musak::Start(void) {
	m_step = 0;
	m_frame = 0;
	m_event = 0;
	m_gain = m_duck ? DUCK_GAIN : 256;
	m_running = m_ready && m_enabled;
}

void Musak::Stop(void) {
	m_running = false;
	psyqo::SPU::silenceChannels(0xffu << FIRST_VOICE);
	for (auto& r : m_release) {
		r = 0;
	}
}

void Musak::SetEnabled(bool on) {
	if (on == m_enabled) {
		return;
	}
	m_enabled = on;
	if (on) {
		Start();
	} else {
		Stop();
	}
}

void Musak::Trigger(const MusakEvent& e) {
	const auto& inst = c_instruments[c_trackInstrument[e.track]];
	uint32_t level = uint32_t(c_trackLevel[e.track]) * e.velocity / 127;
	uint8_t pan = c_trackPan[e.track];
	m_base[e.track][0] = level * pan / 8 > 0x3fff ? 0x3fff : level * pan / 8;
	m_base[e.track][1] = level * (16 - pan) / 8 > 0x3fff ? 0x3fff : level * (16 - pan) / 8;
	uint16_t pitch = inst.sampleRate ? uint32_t(inst.sampleRate) * 4096 / psyqo::SPU::BASE_SAMPLE_RATE : e.pitch;
	psyqo::SPU::ChannelPlaybackConfig config;
	config.sampleRate.value = pitch;
	config.volumeLeft = uint32_t(m_base[e.track][0]) * m_gain / 256;
	config.volumeRight = uint32_t(m_base[e.track][1]) * m_gain / 256;
	config.adsr = inst.adsr;
	psyqo::SPU::playADPCM(FIRST_VOICE + e.track, m_addr[c_trackInstrument[e.track]], config, true);
	m_release[e.track] = e.length;
}

void Musak::ApplyVolumes(void) {
	for (uint8_t t = 0; t < TRACKS; t++) {
		SPU_VOICES[FIRST_VOICE + t].volumeLeft = uint32_t(m_base[t][0]) * m_gain / 256;
		SPU_VOICES[FIRST_VOICE + t].volumeRight = uint32_t(m_base[t][1]) * m_gain / 256;
	}
}

void Musak::Tick(uint32_t frames) {
	if (!m_running) {
		return;
	}
	while (frames--) {
		uint16_t target = m_duck ? DUCK_GAIN : 256;
		if (m_gain != target) {
			if (m_gain > target) {
				m_gain = m_gain - target > DUCK_DOWN ? m_gain - DUCK_DOWN : target;
			} else {
				m_gain = target - m_gain > DUCK_UP ? m_gain + DUCK_UP : target;
			}
			ApplyVolumes();
		}
		if (m_frame++ != 0) {
			if (m_frame == FRAMES_PER_STEP) {
				m_frame = 0;
			}
			continue;
		}
		// a new 16th: key off what has run its length, then key on what starts here
		uint32_t off = 0;
		for (uint8_t t = 0; t < TRACKS; t++) {
			if (m_release[t] && --m_release[t] == 0) {
				off |= 1u << (FIRST_VOICE + t);
			}
		}
		if (off) {
			SPU_KEY_OFF_LOW = off & 0xffff;
			SPU_KEY_OFF_HIGH = off >> 16;
		}
		constexpr uint16_t count = sizeof(c_events) / sizeof(c_events[0]);
		while (m_event < count && c_events[m_event].at == m_step) {
			Trigger(c_events[m_event++]);
		}
		if (++m_step == MUSAK_LOOP_STEPS) {
			m_step = 0;
			m_event = 0;
		}
	}
}
