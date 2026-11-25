#pragma once

#include <stdint.h>

// Phone tones without any sample files: one looping ADPCM block holding a single sine period
// is uploaded to the top of sound RAM, and each tone is two SPU voices playing it at two pitches.
// It sits above everything the engine's SoundManager allocates from the bottom.
class Tones final {
  public:
	// RingbackUS is the North American cadence, 2 s on and 4 s off; Ringback the short radio one
	enum class Cue : uint8_t { None, Digit, Ringback, Busy, Win, Ring, RingbackUS };

	void Init(void);
	void Tick(uint32_t ticks);

	// voices 0/1 belong to the keypad, voices 2-5 to the two lines, voices 6/7 to the jingle
	void Digit(char digit);
	void Line(uint8_t line, Cue cue);
	void Win(void);
	bool Ringing(void) const; // a ringback or the corp bell is going on either line

  private:
	struct Voice {
		Cue cue = Cue::None;
		uint16_t t = 0;
	};

	void Play(uint8_t voice, uint16_t frequency, uint16_t volume);
	void Stop(uint8_t voiceMask);
	void TickLine(uint8_t line);

	uint16_t m_digitTimer = 0;
	Voice m_lines[2];
	uint8_t m_winStep = 0;
	uint16_t m_winTimer = 0;
	bool m_ready = false;
};
