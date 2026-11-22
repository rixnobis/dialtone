#include "tones.hh"

#include "madnight.hh"

#include <psyqo/spu.hh>

static constexpr uint32_t TONE_SPU_ADDR = 0x7f000;
static constexpr uint8_t SAMPLES_PER_PERIOD = 28; // one ADPCM block
static constexpr uint16_t VOLUME_KEYPAD = 0x1400;
static constexpr uint16_t VOLUME_LINE = 0x0e00;

// one sine period, 4-bit, as nibbles in [-7, 7]
static constexpr int8_t c_sine[SAMPLES_PER_PERIOD] = {0, 2, 3, 4, 5, 6, 7, 7, 7, 6, 5, 4, 3, 2,
													 0, -2, -3, -4, -5, -6, -7, -7, -7, -6, -5, -4, -3, -2};

static VagEntry s_tone;

static uint16_t PitchFor(uint16_t frequency) {
	return uint32_t(frequency) * SAMPLES_PER_PERIOD * SPU_NOMINAL_PITCH / psyqo::SPU::BASE_SAMPLE_RATE;
}

void Tones::Init(void) {
	// filter 0, shift 1: each nibble lands as nibble << 11
	alignas(4) uint8_t block[psyqo::SPU::ADPCM_BLOCK_SIZE];
	block[0] = 0x01;
	block[1] = 0x07; // loop start + loop end + repeat: the block loops onto itself
	for (uint8_t i = 0; i < SAMPLES_PER_PERIOD; i += 2) {
		block[2 + i / 2] = (c_sine[i] & 0xf) | ((c_sine[i + 1] & 0xf) << 4);
	}
	psyqo::SPU::dmaWrite(TONE_SPU_ADDR, block, sizeof(block));

	s_tone.spuAddr = TONE_SPU_ADDR;
	s_tone.size = sizeof(block);
	m_ready = true;
}

void Tones::Play(uint8_t voice, uint16_t frequency, uint16_t volume) {
	if (!m_ready) {
		return;
	}
	auto& sound = g_madnightEngine.m_soundManager;
	s_tone.pitch = PitchFor(frequency);
	sound.PlayVAGFile(&s_tone, voice, sound.CreatePlaybackConfig(&s_tone, volume), true);
}

void Tones::Stop(uint8_t voiceMask) { g_madnightEngine.m_soundManager.SilenceChannels(voiceMask); }

void Tones::Digit(char digit) {
	static constexpr uint16_t rows[] = {697, 770, 852, 941};
	static constexpr uint16_t cols[] = {1209, 1336, 1477};
	uint8_t row, col;
	if (digit == '*' || digit == '0' || digit == '#') {
		row = 3, col = digit == '*' ? 0 : (digit == '0' ? 1 : 2);
	} else {
		uint8_t n = digit - '1';
		row = n / 3, col = n % 3;
	}
	Play(0, rows[row], VOLUME_KEYPAD);
	Play(1, cols[col], VOLUME_KEYPAD);
	m_digitTimer = 6;
}

void Tones::Line(uint8_t line, Cue cue) {
	m_lines[line].cue = cue;
	m_lines[line].t = 0;
	Stop(0b11 << (2 + line * 2));
	TickLine(line);
}

void Tones::Win(void) {
	m_winStep = 0;
	m_winTimer = 1;
}

void Tones::TickLine(uint8_t line) {
	auto& v = m_lines[line];
	uint8_t a = 2 + line * 2;
	uint16_t lo = 0, hi = 0, on = 0, period = 0;
	switch (v.cue) {
		case Cue::Ringback:
			lo = 440, hi = 480, on = 70, period = 120;
			break;
		case Cue::Busy:
			lo = 480, hi = 620, on = 25, period = 50;
			break;
		default:
			return;
	}

	uint16_t phase = v.t % period;
	if (phase == 0) {
		uint16_t volume = line == 0 ? VOLUME_LINE : VOLUME_LINE / 2;
		Play(a, lo, volume);
		Play(a + 1, hi, volume);
	} else if (phase == on) {
		Stop(0b11 << a);
	}
	v.t++;
}

void Tones::Tick(uint32_t ticks) {
	while (ticks--) {
		if (m_digitTimer && --m_digitTimer == 0) {
			Stop(0b11);
		}

		for (uint8_t i = 0; i < 2; i++) {
			TickLine(i);
		}

		if (m_winTimer && --m_winTimer == 0) {
			static constexpr uint16_t notes[] = {523, 659, 784, 1047, 0};
			uint16_t note = notes[m_winStep++];
			if (note) {
				Play(6, note, VOLUME_KEYPAD);
				Play(7, note / 2, VOLUME_KEYPAD / 2);
				m_winTimer = m_winStep == 4 ? 30 : 9;
			} else {
				Stop(0b11 << 6);
			}
		}
	}
}
