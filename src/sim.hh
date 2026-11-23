#pragma once

#include "bignum.hh"

#include <stdint.h>

#include <EASTL/array.h>

class Rand;

static constexpr uint8_t NUMBER_LENGTH = 7;
static constexpr uint8_t MAX_LINES = 2;
static constexpr uint8_t STATION_COUNT = 4;

// all times are in vsync ticks
static constexpr uint16_t TICKS_PER_SECOND = 60;

// each station is a wall: prizes jump an order of magnitude or two, and so do the crowds
struct Station {
	const char* name;
	const char* reach;	// "local", "regional", ...
	const char* number; // NUMBER_LENGTH digits, no dash
	uint32_t prizeMin, prizeMax;
	uint8_t callerMin, callerMax; // "caller N wins", N picked in [min, max]
	uint16_t rivalEvery;		  // average ticks between rival callers getting through
	uint8_t busyPercent;		  // chance a call hits a busy signal
	uint16_t roundSeconds;
	uint32_t unlockAt; // earned in this area code before you can tune in
};

enum class LineState : uint8_t { Locked, Idle, Dialing, Ringing, Busy, OnAir };

enum class Outcome : uint8_t { None, Busy, NoAnswer, WrongNumber, Missed, Won };

struct Line {
	LineState state = LineState::Locked;
	char digits[NUMBER_LENGTH + 1] = {0};
	uint8_t digitCount = 0;
	bool autoDialing = false;  // being dialed by redial or the autodialer, not by hand
	bool byMachine = false;	   // the autodialer, which dials at its own (slow, then faster) speed
	uint16_t timer = 0;		   // ticks left in the current state, or ticks spent idle
	uint8_t callerNumber = 0;  // which caller we were when we got through
	uint8_t callerTarget = 0;  // which caller the DJ wanted at that moment
	Outcome outcome = Outcome::None;
	BigNum prizeWon;
};

enum class Upgrade : uint8_t { Redial, SecondLine, Count };

struct UpgradeInfo {
	const char* name;
	uint32_t price; // flat, desk items are one-offs
};

// levelled upgrades: not desk objects, they change the numbers. Price of the next level is
// base * g^level, g from Sim::GrowthMilli (1.15, Haggler brings it down)
// the autodialer is a stat too: level 1 is a slow, dumb machine and each level makes it less so
enum class Stat : uint8_t { Autodialer, QuickFingers, Luck, Charm, Count };

struct StatInfo {
	const char* name;
	uint32_t basePrice;
	uint16_t maxLevel;
};

// bought with Reputation, kept through a new area code
enum class Perk : uint8_t { BigName, HeadStart, SpeedDial, Haggler, Count };

struct PerkInfo {
	const char* name;
	uint32_t baseRep; // price of the next level is baseRep * repGrowth^level
	uint8_t repGrowth;
	uint16_t maxLevel;
};

// everything that survives a power cycle, written raw into the memory card save
struct SimSave {
	BigNum cash, runEarned, allTimeEarned, reputation, repGranted;
	uint32_t wins;
	uint16_t statLevel[uint8_t(Stat::Count)];
	uint16_t perkLevel[uint8_t(Perk::Count)];
	uint16_t areaCodes; // prestiges so far
	uint8_t upgrades;
	uint8_t station;
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
	static const eastl::array<StatInfo, size_t(Stat::Count)> c_stats;
	static const eastl::array<PerkInfo, size_t(Perk::Count)> c_perks;
	// all-time earnings for the first point of Reputation; total Reputation = sqrt(all-time / this)
	static constexpr uint32_t REP_THRESHOLD = 20000;

	void Init(Rand* rand, SimListener* listener);
	void Tick(uint32_t ticks);

	// player input
	void PressDigit(char digit);
	void HangUp(void);
	void Redial(void);
	bool BuyNextUpgrade(void);
	bool BuyStat(Stat stat);
	bool BuyPerk(Perk perk);
	void TuneStation(int8_t direction);
	// prestige, in two steps so perks bought in between apply to the new run: NewAreaCode pays the
	// Reputation and hangs up, StartAreaCode then resets cash, desk, stats and station
	bool NewAreaCode(void);
	void StartAreaCode(void);

	void Save(SimSave& out) const;
	void Load(const SimSave& in);

	// state for the ui
	const BigNum& Cash(void) const { return m_cash; }
	const BigNum& RunEarned(void) const { return m_runEarned; }
	const BigNum& AllTimeEarned(void) const { return m_allTime; }
	const BigNum& Reputation(void) const { return m_reputation; }
	BigNum PendingRep(void) const; // what a new area code pays right now
	BigNum NextRepAt(void) const;  // all-time earnings for one more point
	uint16_t AreaCodes(void) const { return m_areaCodes; }
	uint32_t Wins(void) const { return m_wins; }
	const Station& CurrentStation(void) const { return c_stations[m_station]; }
	uint8_t StationIndex(void) const { return m_station; }
	bool IsStationUnlocked(uint8_t index) const { return m_runEarned >= BigNum::FromUint(c_stations[index].unlockAt); }
	int8_t NextLockedStation(void) const;
	bool HasUpgrade(Upgrade upgrade) const { return m_upgrades & (1 << uint8_t(upgrade)); }
	int8_t NextUpgrade(void) const;
	uint16_t StatLevel(Stat stat) const { return m_statLevel[uint8_t(stat)]; }
	BigNum StatPrice(Stat stat) const; // zero once maxed
	uint16_t PerkLevel(Perk perk) const { return m_perkLevel[uint8_t(perk)]; }
	BigNum PerkPrice(Perk perk) const; // in Reputation, zero once maxed
	uint16_t GrowthMilli(void) const;  // stat price growth per level, 1150 = x1.15
	BigNum StartCash(void) const;
	// what the stats currently do, for the shop text
	uint8_t AutoDigitTicks(void) const;
	uint8_t AutodialDigitTicks(void) const;
	uint16_t AutodialIdleTicks(void) const;
	uint8_t AutodialLines(void) const; // how many lines it works, from the last one down
	uint8_t AutodialWindow(void) const; // 0: dials blind; else holds until the count is this close
	bool AutodialReady(void) const;
	bool HasAutodialer(void) const { return StatLevel(Stat::Autodialer) > 0; }
	uint8_t ConnectTicks(void) const;
	uint8_t BusyPercent(void) const; // before the jam near the end of a round
	BigNum Consolation(void) const;
	BigNum PrizeMultiplier(void) const; // Charm and Big name together
	const Line& GetLine(uint8_t index) const { return m_lines[index]; }
	int8_t HandLine(void) const { return m_handLine; }

	bool IsRoundOpen(void) const { return m_roundOpen; }
	uint8_t CallerTarget(void) const { return m_callerTarget; }
	uint8_t CallersSoFar(void) const { return m_callersSoFar; }
	const BigNum& Prize(void) const { return m_prize; }
	uint16_t SecondsLeft(void) const { return (m_roundTimer + TICKS_PER_SECOND - 1) / TICKS_PER_SECOND; }
	const char* DJLine(void) const { return m_djLine; }

  private:
	void StartRound(void);
	void ResetRun(void);
	void Earn(const BigNum& amount);
	void EndRound(void);
	void SetDJ(const char* fmt, ...);
	void StartDialing(uint8_t index, bool automatic, bool machine = false);
	void FinishDialing(uint8_t index);
	void TickLine(uint8_t index);
	void ConnectToDJ(uint8_t index);
	int8_t FindIdleLine(void) const;
	uint32_t RandRange(uint32_t min, uint32_t max); // inclusive

	Rand* m_rand = nullptr;
	SimListener* m_listener = nullptr;

	BigNum m_cash, m_runEarned, m_allTime, m_reputation, m_repGranted;
	uint32_t m_wins = 0;
	uint16_t m_areaCodes = 0;
	uint8_t m_upgrades = 0;
	uint16_t m_statLevel[uint8_t(Stat::Count)] = {0};
	uint16_t m_perkLevel[uint8_t(Perk::Count)] = {0};
	uint8_t m_station = 0;

	eastl::array<Line, MAX_LINES> m_lines;
	int8_t m_handLine = -1; // the line the keypad is typing into
	uint8_t m_autoDigitTimer = 0;

	bool m_roundOpen = false;
	uint16_t m_roundTimer = 0;
	uint8_t m_callerTarget = 0;
	uint8_t m_callersSoFar = 0;
	BigNum m_prize, m_rollover;
	char m_djLine[48] = {0};
};
