#pragma once

#include <stdint.h>

#include <EASTL/array.h>

class Rand;

static constexpr uint8_t NUMBER_LENGTH = 7;
static constexpr uint8_t MAX_LINES = 2;
static constexpr uint8_t STATION_COUNT = 2;

// all times are in vsync ticks
static constexpr uint16_t TICKS_PER_SECOND = 60;

struct Station {
	const char* name;
	const char* number; // NUMBER_LENGTH digits, no dash
	uint32_t prizeMin, prizeMax;
	uint8_t callerMin, callerMax; // "caller N wins", N picked in [min, max]
	uint16_t rivalEvery;		  // average ticks between rival callers getting through
	uint8_t busyPercent;		  // chance a call hits a busy signal
	uint16_t roundSeconds;
	uint32_t unlockAt; // lifetime earnings needed before you can tune in
};

enum class LineState : uint8_t { Locked, Idle, Dialing, Ringing, Busy, OnAir };

enum class Outcome : uint8_t { None, Busy, NoAnswer, WrongNumber, Missed, Won };

struct Line {
	LineState state = LineState::Locked;
	char digits[NUMBER_LENGTH + 1] = {0};
	uint8_t digitCount = 0;
	bool autoDialing = false;  // being dialed by redial or the autodialer, not by hand
	uint16_t timer = 0;		   // ticks left in the current state, or ticks spent idle
	uint8_t callerNumber = 0;  // what the DJ said when we got through
	Outcome outcome = Outcome::None;
	uint32_t prizeWon = 0;
};

enum class Upgrade : uint8_t { Redial, SecondLine, Autodialer, Count };

struct UpgradeInfo {
	const char* name;
	uint32_t price;
};

// sound cues the sim asks for, the scene maps them to the SPU
class SimListener {
  public:
	virtual void OnDigit(uint8_t line, char digit) = 0;
	virtual void OnRing(uint8_t line) = 0;
	virtual void OnBusy(uint8_t line) = 0;
	virtual void OnHangUp(uint8_t line) = 0;
	virtual void OnWin(uint8_t line) = 0;
};

class Sim final {
  public:
	static const eastl::array<Station, STATION_COUNT> c_stations;
	static const eastl::array<UpgradeInfo, size_t(Upgrade::Count)> c_upgrades;

	void Init(Rand* rand, SimListener* listener);
	void Tick(uint32_t ticks);

	// player input
	void PressDigit(char digit);
	void HangUp(void);
	void Redial(void);
	bool BuyNextUpgrade(void);
	void TuneStation(int8_t direction);

	// state for the ui
	uint32_t Cash(void) const { return m_cash; }
	uint32_t Lifetime(void) const { return m_lifetime; }
	uint32_t Wins(void) const { return m_wins; }
	const Station& CurrentStation(void) const { return c_stations[m_station]; }
	uint8_t StationIndex(void) const { return m_station; }
	bool IsStationUnlocked(uint8_t index) const { return m_lifetime >= c_stations[index].unlockAt; }
	bool HasUpgrade(Upgrade upgrade) const { return m_upgrades & (1 << uint8_t(upgrade)); }
	int8_t NextUpgrade(void) const;
	const Line& GetLine(uint8_t index) const { return m_lines[index]; }
	int8_t HandLine(void) const { return m_handLine; }

	bool IsRoundOpen(void) const { return m_roundOpen; }
	uint8_t CallerTarget(void) const { return m_callerTarget; }
	uint8_t CallersSoFar(void) const { return m_callersSoFar; }
	uint32_t Prize(void) const { return m_prize; }
	uint16_t SecondsLeft(void) const { return (m_roundTimer + TICKS_PER_SECOND - 1) / TICKS_PER_SECOND; }
	const char* DJLine(void) const { return m_djLine; }

  private:
	void StartRound(void);
	void EndRound(void);
	void SetDJ(const char* fmt, ...);
	void StartDialing(uint8_t index, bool automatic);
	void FinishDialing(uint8_t index);
	void TickLine(uint8_t index);
	void ConnectToDJ(uint8_t index);
	int8_t FindIdleLine(void) const;
	uint32_t RandRange(uint32_t min, uint32_t max); // inclusive

	Rand* m_rand = nullptr;
	SimListener* m_listener = nullptr;

	uint32_t m_cash = 0;
	uint32_t m_lifetime = 0;
	uint32_t m_wins = 0;
	uint8_t m_upgrades = 0;
	uint8_t m_station = 0;

	eastl::array<Line, MAX_LINES> m_lines;
	int8_t m_handLine = -1; // the line the keypad is typing into
	uint8_t m_autoDigitTimer = 0;

	bool m_roundOpen = false;
	uint16_t m_roundTimer = 0;
	uint8_t m_callerTarget = 0;
	uint8_t m_callersSoFar = 0;
	uint32_t m_prize = 0;
	uint32_t m_rollover = 0;
	char m_djLine[48] = {0};
};
