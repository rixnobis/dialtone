#include "sim.hh"

#include "rand.hh"

#include <EASTL/algorithm.h>
#include <psyqo/xprintf.h>
#include <stdarg.h>

const eastl::array<Station, STATION_COUNT> Sim::c_stations = {{
	{.name = "KDLT 88.5",
	 .reach = "local",
	 .number = "5550188",
	 .prizeMin = 12,
	 .prizeMax = 30,
	 .callerMin = 3,
	 .callerMax = 6,
	 .rivalEvery = 180,
	 .busyPercent = 35,
	 .roundSeconds = 25,
	 .unlockAt = 0},
	{.name = "WHOT 104.7",
	 .reach = "regional",
	 .number = "5551047",
	 .prizeMin = 150,
	 .prizeMax = 400,
	 .callerMin = 6,
	 .callerMax = 11,
	 .rivalEvery = 110,
	 .busyPercent = 50,
	 .roundSeconds = 25,
	 .unlockAt = 300},
	{.name = "KNAT 96.1",
	 .reach = "national",
	 .number = "5550961",
	 .prizeMin = 5000,
	 .prizeMax = 12000,
	 .callerMin = 9,
	 .callerMax = 16,
	 .rivalEvery = 75,
	 .busyPercent = 62,
	 .roundSeconds = 28,
	 .unlockAt = 15000},
	{.name = "SKY 1",
	 .reach = "satellite",
	 .number = "5550001",
	 .prizeMin = 300000,
	 .prizeMax = 750000,
	 .callerMin = 14,
	 .callerMax = 24,
	 .rivalEvery = 55,
	 .busyPercent = 72,
	 .roundSeconds = 30,
	 .unlockAt = 1000000},
}};

const eastl::array<UpgradeInfo, size_t(Upgrade::Count)> Sim::c_upgrades = {{
	{.name = "Redial button", .price = 25},
	{.name = "Second line", .price = 60},
}};

const eastl::array<StatInfo, size_t(Stat::Count)> Sim::c_stats = {{
	{.name = "Autodialer", .basePrice = 120, .maxLevel = 10},
	{.name = "Quick fingers", .basePrice = 20, .maxLevel = 5},
	{.name = "Luck", .basePrice = 30, .maxLevel = 12},
	{.name = "Charm", .basePrice = 25, .maxLevel = 9999},
}};

const eastl::array<PerkInfo, size_t(Perk::Count)> Sim::c_perks = {{
	{.name = "Big name", .baseRep = 1, .repGrowth = 3, .maxLevel = 999},
	{.name = "Head start", .baseRep = 2, .repGrowth = 4, .maxLevel = 999},
	{.name = "Speed dial", .baseRep = 3, .repGrowth = 1, .maxLevel = 1},
	{.name = "Haggler", .baseRep = 4, .repGrowth = 2, .maxLevel = 5},
}};

static constexpr const char* c_rivals[] = {"Dana", "Mo", "Big Steve", "Ines", "Kev", "Pat", "Ruth", "Ozzie"};
static constexpr uint8_t AUTO_DIGIT_TICKS = 6;		 // redial and autodialer speed, quick fingers takes 1 off per level
static constexpr uint8_t CONNECT_TICKS = 30;		 // after the last digit, before it rings; quick fingers takes 5 off
static constexpr uint8_t LUCK_BUSY_STEP = 6;		 // busy chance off per luck level
static constexpr uint8_t LUCK_BUSY_FLOOR = 10;		 // the station is never always free
static constexpr uint16_t CHARM_MILLI = 1100;		 // prize x1.1 per charm level
static constexpr uint16_t GROWTH_MILLI = 1150;		 // stat prices x1.15 per level...
static constexpr uint16_t HAGGLER_MILLI = 10;		 // ...minus 0.01 per Haggler level, so 1.10 at the cap
static constexpr uint16_t CONSOLATION_MILLI = 50;	 // 5% of the round's prize to everyone who gets on air
// the autodialer starts out bad on purpose: it waits this long on a free line before grabbing it...
static constexpr uint16_t c_autodialIdle[11] = {0, 600, 420, 300, 210, 150, 100, 70, 45, 30, 20};
static constexpr uint8_t AUTODIAL_DIGIT_TICKS = 13; // ...dials a digit every 13 - level ticks...
static constexpr uint8_t AUTODIAL_BOTH_LINES = 3;	// ...and works one line until this level.
static constexpr uint8_t AUTODIAL_TIMING = 6;		// From here it times its calls like a person would: it
static constexpr uint8_t AUTODIAL_SHARP = 8;		// holds until the DJ's count is 2 short of the number, 1 from here,
static constexpr uint16_t AUTODIAL_MASH_TICKS = 10; // then redials a free line almost at once
static constexpr uint16_t INTERMISSION_TICKS = 4 * TICKS_PER_SECOND; // long enough for an autosave to finish
static constexpr uint16_t BUSY_TICKS = 75;
static constexpr uint16_t ON_AIR_TICKS = 100;
static constexpr uint8_t JAMMED_EXTRA_BUSY = 15; // everyone piles on when the winner is close
#ifndef DIALTONE_START_CASH
// `make DEFINES="PCDRV DIALTONE_START_CASH=500"` to try the upgrades; it counts as earned, so a big enough
// value also unlocks the stations and the first new area code
#define DIALTONE_START_CASH 0
#endif

void Sim::Init(Rand* rand, SimListener* listener) {
	m_rand = rand;
	m_listener = listener;
	ResetRun();
	Earn(BigNum::FromUint(DIALTONE_START_CASH));
}

void Sim::ResetRun(void) {
	for (auto& line : m_lines) {
		line = Line();
	}
	m_lines[0].state = LineState::Idle;
	m_handLine = -1;
	m_autoDigitTimer = 0;
	m_cash = StartCash();
	m_runEarned = BigNum();
	m_upgrades = PerkLevel(Perk::SpeedDial) ? 1 << uint8_t(Upgrade::Redial) : 0;
	for (auto& level : m_statLevel) {
		level = 0;
	}
	m_station = 0;
	m_rollover = BigNum();
	StartRound();
}

void Sim::Earn(const BigNum& amount) {
	m_cash += amount;
	m_runEarned += amount;
	m_allTime += amount;
}

uint32_t Sim::RandRange(uint32_t min, uint32_t max) { return min + m_rand->rand(max - min + 1); }

void Sim::SetDJ(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m_djLine, sizeof(m_djLine), fmt, ap);
	va_end(ap);
}

void Sim::StartRound(void) {
	const auto& station = CurrentStation();
	m_roundOpen = true;
	m_roundTimer = station.roundSeconds * TICKS_PER_SECOND;
	m_callerTarget = RandRange(station.callerMin, station.callerMax);
	m_callersSoFar = 0;
	m_prize = (BigNum::FromUint(RandRange(station.prizeMin, station.prizeMax)) * PrizeMultiplier()).Floor() + m_rollover;
	m_rollover = BigNum();
	char prize[12];
	m_prize.Format(prize);
	SetDJ("Caller %d wins $%s! Lines open!", m_callerTarget, prize);
	printf("DIALTONE: round station=%d caller=%d prize=%s charm=%d\n", m_station, m_callerTarget, prize,
		   StatLevel(Stat::Charm));
}

void Sim::EndRound(void) {
	m_roundOpen = false;
	m_roundTimer = INTERMISSION_TICKS;
}

void Sim::Tick(uint32_t ticks) {
	while (ticks--) {
		if (m_roundOpen) {
			if (m_rand->rand(CurrentStation().rivalEvery) == 0) {
				m_callersSoFar++;
				if (m_callersSoFar >= m_callerTarget) {
					char prize[12];
					m_prize.Format(prize);
					SetDJ("Caller %d is %s! $%s gone.", m_callersSoFar, c_rivals[m_rand->rand(8)], prize);
					EndRound();
				}
			}

			if (m_roundOpen && --m_roundTimer == 0) {
				m_rollover = m_prize;
				char prize[12];
				m_prize.Format(prize);
				SetDJ("No winner! $%s rolls over.", prize);
				EndRound();
			}
		} else if (--m_roundTimer == 0) {
			StartRound();
		}

		for (uint8_t i = 0; i < MAX_LINES; i++) {
			TickLine(i);
		}

		// the autodialer works the lines nobody is touching
		if (HasAutodialer()) {
			uint8_t lines = AutodialLines();
			for (int8_t i = MAX_LINES - 1; i >= 0 && lines; i--) {
				auto& line = m_lines[i];
				if (line.state == LineState::Locked) {
					continue;
				}
				lines--;
				uint16_t idle = AutodialWindow() ? eastl::min(AutodialIdleTicks(), AUTODIAL_MASH_TICKS) : AutodialIdleTicks();
				if (line.state == LineState::Idle && line.timer >= idle && AutodialReady()) {
					StartDialing(i, true, true);
				}
			}
		}
	}
}

void Sim::TickLine(uint8_t index) {
	auto& line = m_lines[index];
	switch (line.state) {
		case LineState::Locked:
			break;

		case LineState::Idle:
			if (line.timer < 0xffff) {
				line.timer++;
			}
			break;

		case LineState::Dialing:
			if (line.digitCount == NUMBER_LENGTH) {
				// all digits in, the exchange is putting the call through
				if (line.timer == 0 || --line.timer == 0) {
					FinishDialing(index);
				}
				break;
			}
			if (!line.autoDialing) {
				break;
			}
			if (++line.timer >= (line.byMachine ? AutodialDigitTicks() : AutoDigitTicks())) {
				line.timer = 0;
				char digit = CurrentStation().number[line.digitCount];
				line.digits[line.digitCount++] = digit;
				line.digits[line.digitCount] = 0;
				m_listener->OnDigit(index, digit);
				if (line.digitCount == NUMBER_LENGTH) {
					line.timer = ConnectTicks();
				}
			}
			break;

		case LineState::Ringing:
			if (--line.timer == 0) {
				ConnectToDJ(index);
			}
			break;

		case LineState::Busy:
			if (--line.timer == 0) {
				line.state = LineState::Idle;
				line.outcome = Outcome::Busy;
				m_listener->OnHangUp(index);
			}
			break;

		case LineState::OnAir:
			if (--line.timer == 0) {
				line.state = LineState::Idle;
				m_listener->OnHangUp(index);
			}
			break;
	}
}

int8_t Sim::FindIdleLine(void) const {
	for (uint8_t i = 0; i < MAX_LINES; i++) {
		if (m_lines[i].state == LineState::Idle) {
			return i;
		}
	}
	return -1;
}

void Sim::StartDialing(uint8_t index, bool automatic, bool machine) {
	auto& line = m_lines[index];
	line.state = LineState::Dialing;
	line.digitCount = 0;
	line.digits[0] = 0;
	line.autoDialing = automatic;
	line.byMachine = machine;
	line.timer = 0;
	line.outcome = Outcome::None;
	line.callerNumber = 0;
	line.prizeWon = BigNum();
}

void Sim::FinishDialing(uint8_t index) {
	auto& line = m_lines[index];
	bool rightNumber = true;
	for (uint8_t i = 0; i < NUMBER_LENGTH; i++) {
		if (line.digits[i] != CurrentStation().number[i]) {
			rightNumber = false;
		}
	}

	if (!rightNumber) {
		line.state = LineState::Busy;
		line.timer = BUSY_TICKS;
		m_listener->OnBusy(index);
		// lands as WrongNumber instead of Busy once the tone ends
		line.outcome = Outcome::WrongNumber;
		return;
	}

	uint8_t busyChance = BusyPercent();
	if (m_roundOpen && m_callersSoFar + 2 >= m_callerTarget) {
		busyChance += JAMMED_EXTRA_BUSY;
	}
	printf("DIALTONE: call line=%d busy=%d%% luck=%d\n", index, busyChance, StatLevel(Stat::Luck));

	if (m_rand->rand(100) < busyChance) {
		line.state = LineState::Busy;
		line.timer = BUSY_TICKS;
		m_listener->OnBusy(index);
	} else {
		line.state = LineState::Ringing;
		line.timer = RandRange(45, 110);
		m_listener->OnRing(index);
	}
}

void Sim::ConnectToDJ(uint8_t index) {
	auto& line = m_lines[index];
	line.state = LineState::OnAir;
	line.timer = ON_AIR_TICKS;

	if (!m_roundOpen) {
		line.outcome = Outcome::NoAnswer;
		return;
	}

	line.callerNumber = ++m_callersSoFar;
	line.callerTarget = m_callerTarget;
	if (m_callersSoFar == m_callerTarget) {
		line.outcome = Outcome::Won;
		line.prizeWon = m_prize;
		Earn(m_prize);
		m_wins++;
		char prize[12];
		m_prize.Format(prize);
		SetDJ("Caller %d, that's YOU! $%s!", m_callersSoFar, prize);
		EndRound();
		m_listener->OnWin(index);
	} else {
		// a consolation prize for every caller who gets on air, so a bad run still pays a little
		line.outcome = Outcome::Missed;
		line.prizeWon = Consolation();
		Earn(line.prizeWon);
		char prize[12];
		line.prizeWon.Format(prize);
		SetDJ("Caller %d! Not you. Have $%s.", m_callersSoFar, prize);
	}
}

void Sim::PressDigit(char digit) {
	if (m_handLine < 0) {
		m_handLine = FindIdleLine();
		if (m_handLine < 0) {
			return;
		}
		StartDialing(m_handLine, false);
	}

	auto& line = m_lines[m_handLine];
	line.digits[line.digitCount++] = digit;
	line.digits[line.digitCount] = 0;
	m_listener->OnDigit(m_handLine, digit);
	if (line.digitCount == NUMBER_LENGTH) {
		line.timer = ConnectTicks();
		m_handLine = -1;
	}
}

void Sim::HangUp(void) {
	for (uint8_t i = 0; i < MAX_LINES; i++) {
		auto& line = m_lines[i];
		if (line.state == LineState::Dialing || line.state == LineState::Ringing || line.state == LineState::Busy) {
			line.state = LineState::Idle;
			line.outcome = Outcome::None;
			line.timer = 0;
			m_listener->OnHangUp(i);
		}
	}
	m_handLine = -1;
}

void Sim::Redial(void) {
	if (!HasUpgrade(Upgrade::Redial)) {
		return;
	}
	int8_t index = FindIdleLine();
	if (index >= 0) {
		StartDialing(index, true);
	}
}

int8_t Sim::NextUpgrade(void) const {
	for (uint8_t i = 0; i < uint8_t(Upgrade::Count); i++) {
		if (!HasUpgrade(Upgrade(i))) {
			return i;
		}
	}
	return -1;
}

bool Sim::BuyNextUpgrade(void) {
	int8_t next = NextUpgrade();
	BigNum price = BigNum::FromUint(next < 0 ? 0 : c_upgrades[next].price);
	if (next < 0 || m_cash < price) {
		return false;
	}

	m_cash -= price;
	m_upgrades |= 1 << next;
	if (Upgrade(next) == Upgrade::SecondLine) {
		m_lines[1].state = LineState::Idle;
	}
	return true;
}

uint16_t Sim::GrowthMilli(void) const { return GROWTH_MILLI - HAGGLER_MILLI * PerkLevel(Perk::Haggler); }

BigNum Sim::StatPrice(Stat stat) const {
	const auto& info = c_stats[uint8_t(stat)];
	uint16_t level = StatLevel(stat);
	if (level >= info.maxLevel) {
		return BigNum();
	}
	return (BigNum::FromUint(info.basePrice) * BigNum::FromMilli(GrowthMilli()).Pow(level)).Floor();
}

bool Sim::BuyStat(Stat stat) {
	BigNum price = StatPrice(stat);
	if (price.IsZero() || m_cash < price) {
		return false;
	}
	m_cash -= price;
	m_statLevel[uint8_t(stat)]++;
	return true;
}

BigNum Sim::PerkPrice(Perk perk) const {
	const auto& info = c_perks[uint8_t(perk)];
	uint16_t level = PerkLevel(perk);
	if (level >= info.maxLevel) {
		return BigNum();
	}
	return BigNum::FromUint(info.baseRep) * BigNum::FromUint(info.repGrowth).Pow(level);
}

bool Sim::BuyPerk(Perk perk) {
	BigNum price = PerkPrice(perk);
	if (price.IsZero() || m_reputation < price) {
		return false;
	}
	m_reputation -= price;
	m_perkLevel[uint8_t(perk)]++;
	// the ones that act on the run in progress act now
	if (perk == Perk::SpeedDial) {
		m_upgrades |= 1 << uint8_t(Upgrade::Redial);
	}
	return true;
}

BigNum Sim::StartCash(void) const {
	uint16_t level = PerkLevel(Perk::HeadStart);
	return level ? BigNum::FromUint(10).Pow(level + 1) : BigNum();
}

BigNum Sim::PendingRep(void) const {
	// x / 20000 as x * 50 * 10^-6, written for REP_THRESHOLD dividing a million
	static_assert(1000000 % REP_THRESHOLD == 0);
	BigNum perPoint = BigNum::FromMilli(1000000 / REP_THRESHOLD) * BigNum::FromMilli(1);
	BigNum total = (m_allTime * perPoint).Sqrt().Floor();
	return total > m_repGranted ? total - m_repGranted : BigNum();
}

BigNum Sim::NextRepAt(void) const {
	// total + 1 points needs all-time = (total + 1)^2 * threshold
	BigNum total = m_repGranted + PendingRep() + BigNum::FromUint(1);
	return total * total * BigNum::FromUint(REP_THRESHOLD);
}

bool Sim::NewAreaCode(void) {
	BigNum gain = PendingRep();
	if (gain.IsZero()) {
		return false;
	}
	m_reputation += gain;
	m_repGranted += gain;
	m_areaCodes++;
	HangUp();
	ResetRun();
	SetDJ("New area code, fresh start!");
	return true;
}

int8_t Sim::NextLockedStation(void) const {
	for (uint8_t i = 0; i < STATION_COUNT; i++) {
		if (!IsStationUnlocked(i)) {
			return i;
		}
	}
	return -1;
}

uint8_t Sim::AutoDigitTicks(void) const { return AUTO_DIGIT_TICKS - StatLevel(Stat::QuickFingers); }

uint8_t Sim::AutodialDigitTicks(void) const {
	uint8_t own = AUTODIAL_DIGIT_TICKS - StatLevel(Stat::Autodialer);
	return eastl::max(own, AutoDigitTicks());
}

uint16_t Sim::AutodialIdleTicks(void) const { return c_autodialIdle[eastl::min<uint16_t>(StatLevel(Stat::Autodialer), 10)]; }

uint8_t Sim::AutodialLines(void) const {
	return StatLevel(Stat::Autodialer) >= AUTODIAL_BOTH_LINES ? MAX_LINES : HasAutodialer() ? 1 : 0;
}

uint8_t Sim::AutodialWindow(void) const {
	uint16_t level = StatLevel(Stat::Autodialer);
	return level >= AUTODIAL_SHARP ? 1 : level >= AUTODIAL_TIMING ? 2 : 0;
}

bool Sim::AutodialReady(void) const {
	uint8_t window = AutodialWindow();
	if (window == 0) {
		return true; // blind: dials whatever is going on, contest or not
	}
	return m_roundOpen && m_callersSoFar + window >= m_callerTarget;
}

uint8_t Sim::ConnectTicks(void) const { return CONNECT_TICKS - 5 * StatLevel(Stat::QuickFingers); }

uint8_t Sim::BusyPercent(void) const {
	int16_t busy = int16_t(CurrentStation().busyPercent) - LUCK_BUSY_STEP * StatLevel(Stat::Luck);
	return busy < LUCK_BUSY_FLOOR ? LUCK_BUSY_FLOOR : busy;
}

BigNum Sim::Consolation(void) const {
	BigNum consolation = (m_prize * BigNum::FromMilli(CONSOLATION_MILLI)).Floor();
	return consolation.IsZero() ? BigNum::FromUint(1) : consolation;
}

BigNum Sim::PrizeMultiplier(void) const {
	return BigNum::FromMilli(CHARM_MILLI).Pow(StatLevel(Stat::Charm)) *
		   BigNum::FromUint(2).Pow(PerkLevel(Perk::BigName));
}

void Sim::Save(SimSave& out) const {
	out.cash = m_cash;
	out.runEarned = m_runEarned;
	out.allTimeEarned = m_allTime;
	out.reputation = m_reputation;
	out.repGranted = m_repGranted;
	out.wins = m_wins;
	for (uint8_t i = 0; i < uint8_t(Stat::Count); i++) {
		out.statLevel[i] = m_statLevel[i];
	}
	for (uint8_t i = 0; i < uint8_t(Perk::Count); i++) {
		out.perkLevel[i] = m_perkLevel[i];
	}
	out.areaCodes = m_areaCodes;
	out.upgrades = m_upgrades;
	out.station = m_station;
}

void Sim::Load(const SimSave& in) {
	for (uint8_t i = 0; i < uint8_t(Perk::Count); i++) {
		m_perkLevel[i] = eastl::min(in.perkLevel[i], c_perks[i].maxLevel);
	}
	ResetRun();
	m_cash = in.cash;
	m_runEarned = in.runEarned;
	m_allTime = in.allTimeEarned;
	m_reputation = in.reputation;
	m_repGranted = in.repGranted;
	m_wins = in.wins;
	for (uint8_t i = 0; i < uint8_t(Stat::Count); i++) {
		m_statLevel[i] = eastl::min(in.statLevel[i], c_stats[i].maxLevel);
	}
	m_areaCodes = in.areaCodes;
	m_upgrades = in.upgrades & ((1 << uint8_t(Upgrade::Count)) - 1);
	if (HasUpgrade(Upgrade::SecondLine)) {
		m_lines[1].state = LineState::Idle;
	}
	m_station = in.station < STATION_COUNT && IsStationUnlocked(in.station) ? in.station : 0;
	StartRound();
}

void Sim::TuneStation(int8_t direction) {
	int8_t next = int8_t(m_station) + direction;
	if (next < 0 || next >= STATION_COUNT || !IsStationUnlocked(next)) {
		return;
	}

	HangUp();
	m_station = next;
	m_rollover = BigNum();
	StartRound();
}
