#pragma once

#include <stdint.h>

#include <psyqo/coroutine.hh>

// Recorded caller and supervisor lines, from the optional asset pack (VOICE/INDEX.BIN plus
// VOICE/BANKn.BIN, built by tools/pack_voices.py from a dialtone-assets checkout). Without the pack
// the game plays as before. Every line sits in sound RAM from boot and plays on SPU voice 16, one
// at a time; not every pick-up speaks, and the same line never plays twice running.
class VoiceLines final {
  public:
	enum Kind : uint8_t { GREET, HANGUP, NOSALE, SALE, SUP_WELCOME, SUP_MET, SUP_MISSED, KINDS };
	static constexpr uint8_t MAX_LINES = 96, MAX_VOICES = 16;
	static constexpr uint32_t SPU_ADDR = 0x2000, SPU_END = 0x7e000; // up to the musak samples
	static constexpr uint8_t SPU_VOICE = 16;

	psyqo::Coroutine<> Load(void);
	bool Ready(void) const { return m_count != 0; }
	uint32_t BytesLoaded(void) const { return m_bytes; }

	void Seed(uint32_t seed) { m_rng = seed | 1; }
	// a call is picked up and they listen (SALE/NOSALE to come) or hang up at once (HANGUP)
	void PickUp(uint8_t simLine, Kind closing);
	// the call on simLine is over; a caller who spoke gets their closing line if it was a listen
	void CallEnded(uint8_t simLine, bool sold);
	void Supervisor(Kind kind);
	void Stop(void);
	void Tick(uint32_t frames);
	bool Live(void) const { return m_playing || m_queued >= 0; }

  private:
	struct Line {
		uint8_t kind, voice;
		uint16_t pitch;	  // SPU pitch register for its sample rate
		uint32_t addr;	  // in sound RAM
		uint16_t frames;  // its length at 60 Hz, rounded up
	};

	uint32_t Rand(uint32_t n);
	int16_t Find(uint8_t voice, Kind kind) const;
	int16_t PickOf(Kind kind); // any voice, never the last line played
	void Play(int16_t index);

	Line m_lines[MAX_LINES];
	uint8_t m_count = 0;
	uint32_t m_bytes = 0;
	uint32_t m_rng = 0x2433;
	int16_t m_last = -1;
	int16_t m_queued = -1;	   // plays as soon as the current one is done
	uint16_t m_playing = 0;	   // frames left of the line now playing
	int8_t m_callVoice[8];	   // per sim line: the voice that answered, -1 if they did not speak
	Kind m_callClosing[8];
};

extern VoiceLines g_voiceLines;
