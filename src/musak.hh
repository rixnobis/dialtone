#pragma once

#include <stdint.h>

struct MusakInstrument {
	const uint8_t* data;
	uint16_t size;
	uint32_t adsr;
	uint16_t sampleRate; // one-shots; 0 = cycle-locked, the event carries the pitch
};

struct MusakEvent {
	uint16_t at; // 16th note in the loop
	uint8_t track, length, velocity;
	uint16_t pitch;
};

// The office musak in phone-books mode: a 16-bar bossa loop sequenced on SPU voices 8-15 from five
// instruments of a few hundred bytes each (tools/make_musak.py), quiet under the tones and ducked
// while a caller or the supervisor is talking.
class Musak final {
  public:
	static constexpr uint32_t SPU_ADDR = 0x7e000; // below the tones at 0x7f000
	static constexpr uint8_t FIRST_VOICE = 8, TRACKS = 8;

	void Init(void);
	void Start(void);
	void Stop(void);
	void SetEnabled(bool on);
	bool Enabled(void) const { return m_enabled; }
	void Duck(bool duck) { m_duck = duck; }
	void Tick(uint32_t frames);

  private:
	void Trigger(const MusakEvent& e);
	void ApplyVolumes(void);

	uint32_t m_addr[8] = {};
	uint16_t m_base[TRACKS][2] = {};   // the volume each voice was keyed at, left/right
	uint8_t m_release[TRACKS] = {};    // 16ths until key off
	uint16_t m_step = 0;			   // 16th within the loop
	uint8_t m_frame = 0;			   // frames into the 16th
	uint16_t m_event = 0;			   // next event
	uint16_t m_gain = 256;			   // ducking gain, 256 = full
	bool m_duck = false;
	bool m_enabled = true;
	bool m_running = false;
	bool m_ready = false;
};
