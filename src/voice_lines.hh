#pragma once

#include <stddef.h>
#include <stdint.h>

#include <psyqo/buffer.hh>
#include <psyqo/coroutine.hh>

// Recorded caller, supervisor and insider lines from the optional voice pack (tools/pack_voices.py,
// built from a dialtone-assets checkout). Without the pack the game plays with silent callers.
//
// Most of the bank stays on the CD. A call's lines are picked as soon as it starts ringing (the sim
// rolls the outcome then), read into one of two streaming slots in sound RAM while it rings, and the
// pick-up waits for the read. A few short stock lines per tier and the supervisor's live in sound
// RAM from boot; they cover a call whose read is late, or a CD that is busy with the other slot.
//
// Slot 0 follows your own line, slot 1 the autodialer call on its readout, each on its own SPU voice.
class VoiceLines final {
  public:
	enum Kind : uint8_t {
		GREET,
		HANGUP,
		NOSALE,
		SALE,
		ODD,
		CALLBACK,
		SUP_WELCOME,
		SUP_MET,
		SUP_MISSED,
		SUP_WARN,
		SUP_RING,
		SUP_MISSEDCALL,
		SUP_PROMOTE,
		SUP_DONE,
		NOTE_FIRST,
		NOTE_HINT,
		NOTE_WARN,
		NOTE_PLEA,
		NOTE_CUT,
		NOTE_LAST,
		KINDS
	};
	enum Tier : uint8_t { RESIDENTIAL, SMALL_BUSINESS, CORPORATE };
	static constexpr uint8_t SLOTS = 2;
	// sound RAM: two slots of two parts (opening line, closing line), then the resident lines up to
	// the musak samples at 0x7e000
	static constexpr uint32_t SPU_ADDR = 0x2000, PART_BYTES = 0xa000;
	static constexpr uint32_t RESIDENT_ADDR = SPU_ADDR + SLOTS * 2 * PART_BYTES, SPU_END = 0x7e000;
	static constexpr uint8_t SPU_VOICE = 16; // slot n plays on SPU_VOICE + n
	// the longest a pick-up waits for its read before a resident line covers for it, in frames
	static constexpr uint16_t MAX_HOLD = 240;

	psyqo::Coroutine<> Load(void);
	bool Ready(void) const { return m_lineCount != 0 || m_residentCount != 0; }

	void Seed(uint32_t seed) { m_rng = seed | 1; }
	// a call on simLine starts ringing on slot; number is a stable id for who picks up (the same
	// number is always the same voice), tier picks the cast, closing is SALE / NOSALE for a listen
	// and HANGUP for one who hangs up, again says the number was called before
	void Ring(uint8_t slot, uint8_t simLine, uint16_t number, Tier tier, Kind closing, bool again);
	// the sim asks whether a pick-up on simLine should keep ringing: true while its opening line is
	// still on its way and it has waited less than MAX_HOLD
	bool HoldPickUp(uint8_t simLine);
	// they picked up / the call is over; a listener who spoke gets their closing line
	void PickUp(uint8_t simLine);
	// wait: frames to hold the closing line back (a sale on your line waits for the win jingle)
	void CallEnded(uint8_t simLine, uint16_t wait = 0);
	void Supervisor(Kind kind);
	void Stop(void);
	void Tick(uint32_t frames);
	bool Live(void) const;
	bool Reading(void) const { return m_reading; }

  private:
	enum class Part : uint8_t { None, Queued, Reading, Ready };
	struct Plan {
		int8_t simLine = -1;
		uint8_t voice = 0;
		Kind opening = GREET, closing = HANGUP;
		uint16_t take[2] = {0, 0};		   // the takes picked for the opening and closing lines
		Part part[2] = {Part::None, Part::None};
		Tier tier = RESIDENTIAL;
		uint16_t number = 0;
		uint16_t held = 0;				   // frames the pick-up has waited
		int16_t resident[2] = {-1, -1};	   // resident lines standing in, if the read was late
		bool fellBack = false;
		bool spoke = false;				   // the opening line played
	};
	struct Job {
		uint8_t slot, part, gen;
		uint16_t take;
	};

	uint32_t Rand(uint32_t n);
	int16_t PickLine(uint8_t voice, Kind kind);
	int8_t PickVoice(Tier tier, uint16_t number, bool resident);
	int16_t FindResident(uint8_t voice, Kind kind) const;
	void Queue(uint8_t slot, uint8_t part);
	void NextRead(void);
	void Issue(void);
	void ReadDone(bool ok);
	void Fallback(uint8_t slot);
	void PlayPart(uint8_t slot, uint8_t part);
	void PlayResident(uint8_t slot, int16_t index, uint16_t volume);
	void PlayClose(uint8_t slot, int8_t which);
	void PlayAt(uint8_t slot, uint32_t addr, uint32_t bytes, uint16_t volume);
	Plan* PlanFor(uint8_t simLine);

	// index tables, straight out of VINDEX.BIN
	psyqo::Buffer<uint8_t> m_index;
	const uint8_t *m_voices = nullptr, *m_groups = nullptr, *m_lines = nullptr, *m_takes = nullptr,
				  *m_residents = nullptr;
	uint16_t m_voiceCount = 0, m_lineCount = 0, m_takeCount = 0, m_residentCount = 0, m_rate = 8000;
	uint8_t m_kindCount = 0, m_streamCount = 0;
	uint16_t m_pitch = 0;
	uint32_t m_streamLba[100] = {};
	int16_t m_supVoice = -1;

	Plan m_plans[SLOTS];
	uint16_t m_playing[SLOTS] = {0, 0}; // frames left of the line each slot is playing
	int8_t m_pendingClose[SLOTS] = {-1, -1};
	uint16_t m_closeWait[SLOTS] = {0, 0};
	uint8_t m_gen[SLOTS] = {0, 0};
	Job m_jobs[4];
	uint8_t m_jobCount = 0;
	Job m_current;
	bool m_reading = false;
	uint16_t m_delay = 0; // DIALTONE_STREAM_DELAY: frames to wait before starting each read
	uint16_t m_wait = 0;
	uint32_t m_frame = 0, m_readStart = 0;
	int16_t m_lastLine = -1;
	uint32_t m_rng = 0x2437;
};

extern VoiceLines g_voiceLines;
