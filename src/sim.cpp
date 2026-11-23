#include "sim.hh"

#include "rand.hh"

#include <psyqo/xprintf.h>
#include <stdarg.h>

const eastl::array<Station, STATION_COUNT> Sim::c_stations = {{
	{.name = "KDLT 88.5",
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
	 .number = "5551047",
	 .prizeMin = 80,
	 .prizeMax = 200,
	 .callerMin = 8,
	 .callerMax = 14,
	 .rivalEvery = 80,
	 .busyPercent = 65,
	 .roundSeconds = 25,
	 .unlockAt = 200},
}};

const eastl::array<UpgradeInfo, size_t(Upgrade::Count)> Sim::c_upgrades = {{
	{.name = "Redial button", .price = 25},
	{.name = "Second line", .price = 60},
	{.name = "Autodialer", .price = 120},
}};

const eastl::array<StatInfo, size_t(Stat::Count)> Sim::c_stats = {{
	{.name = "Quick fingers", .prices = {20, 45, 90, 160, 260}},
	{.name = "Luck", .prices = {30, 60, 110, 190, 300}},
	{.name = "Charm", .prices = {25, 55, 100, 170, 280}},
}};

static constexpr const char* c_rivals[] = {"Dana", "Mo", "Big Steve", "Ines", "Kev", "Pat", "Ruth", "Ozzie"};
static constexpr uint8_t AUTO_DIGIT_TICKS = 6;		 // redial and autodialer speed, quick fingers takes 1 off per level
static constexpr uint8_t CONNECT_TICKS = 30;		 // after the last digit, before it rings; quick fingers takes 5 off
static constexpr uint8_t LUCK_BUSY_STEP = 6;		 // busy chance off per luck level
static constexpr uint8_t LUCK_BUSY_FLOOR = 10;		 // the station is never always free
static constexpr uint8_t CHARM_PRIZE_STEP = 15;		 // percent on the prize per charm level
static constexpr uint16_t AUTODIAL_IDLE_TICKS = 20;	 // leave a free line alone this long before grabbing it
static constexpr uint16_t INTERMISSION_TICKS = 3 * TICKS_PER_SECOND;
static constexpr uint16_t BUSY_TICKS = 75;
static constexpr uint16_t ON_AIR_TICKS = 100;
static constexpr uint8_t JAMMED_EXTRA_BUSY = 15; // everyone piles on when the winner is close
static constexpr uint32_t CONSOLATION = 1; // charm adds 1 per level
#ifndef DIALTONE_START_CASH
#define DIALTONE_START_CASH 0 // `make DEFINES=-DDIALTONE_START_CASH=500` to try the upgrades
#endif

void Sim::Init(Rand* rand, SimListener* listener) {
	m_rand = rand;
	m_listener = listener;
	for (auto& line : m_lines) {
		line = Line();
	}
	m_lines[0].state = LineState::Idle;
	m_cash = DIALTONE_START_CASH;
	StartRound();
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
	m_prize = RandRange(station.prizeMin, station.prizeMax) * PrizePercent() / 100 + m_rollover;
	m_rollover = 0;
	SetDJ("Caller %d wins $%d! Lines open!", m_callerTarget, m_prize);
	printf("DIALTONE: round caller=%d prize=%d charm=%d\n", m_callerTarget, m_prize, StatLevel(Stat::Charm));
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
					SetDJ("Caller %d is %s! $%d gone.", m_callersSoFar, c_rivals[m_rand->rand(8)], m_prize);
					EndRound();
				}
			}

			if (m_roundOpen && --m_roundTimer == 0) {
				m_rollover = m_prize;
				SetDJ("No winner! $%d rolls over.", m_prize);
				EndRound();
			}
		} else if (--m_roundTimer == 0) {
			StartRound();
		}

		for (uint8_t i = 0; i < MAX_LINES; i++) {
			TickLine(i);
		}

		// the autodialer works the lines nobody is touching
		if (HasUpgrade(Upgrade::Autodialer)) {
			for (uint8_t i = 0; i < MAX_LINES; i++) {
				auto& line = m_lines[i];
				if (line.state == LineState::Idle && line.timer >= AUTODIAL_IDLE_TICKS) {
					StartDialing(i, true);
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
			if (++line.timer >= AutoDigitTicks()) {
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

void Sim::StartDialing(uint8_t index, bool automatic) {
	auto& line = m_lines[index];
	line.state = LineState::Dialing;
	line.digitCount = 0;
	line.digits[0] = 0;
	line.autoDialing = automatic;
	line.timer = 0;
	line.outcome = Outcome::None;
	line.callerNumber = 0;
	line.prizeWon = 0;
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
	if (m_callersSoFar == m_callerTarget) {
		line.outcome = Outcome::Won;
		line.prizeWon = m_prize;
		m_cash += m_prize;
		m_lifetime += m_prize;
		m_wins++;
		SetDJ("Caller %d, that's YOU! $%d!", m_callersSoFar, m_prize);
		EndRound();
		m_listener->OnWin(index);
	} else {
		// a consolation prize for every caller who gets on air, so a bad run still pays a little
		line.outcome = Outcome::Missed;
		line.prizeWon = Consolation();
		m_cash += line.prizeWon;
		m_lifetime += line.prizeWon;
		SetDJ("Caller %d! Not this time, have $%d.", m_callersSoFar, line.prizeWon);
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
	if (next < 0 || m_cash < c_upgrades[next].price) {
		return false;
	}

	m_cash -= c_upgrades[next].price;
	m_upgrades |= 1 << next;
	if (Upgrade(next) == Upgrade::SecondLine) {
		m_lines[1].state = LineState::Idle;
	}
	return true;
}

uint32_t Sim::StatPrice(Stat stat) const {
	uint8_t level = StatLevel(stat);
	return level < STAT_MAX_LEVEL ? c_stats[uint8_t(stat)].prices[level] : 0;
}

bool Sim::BuyStat(Stat stat) {
	uint32_t price = StatPrice(stat);
	if (price == 0 || m_cash < price) {
		return false;
	}
	m_cash -= price;
	m_statLevel[uint8_t(stat)]++;
	return true;
}

uint8_t Sim::AutoDigitTicks(void) const { return AUTO_DIGIT_TICKS - StatLevel(Stat::QuickFingers); }

uint8_t Sim::ConnectTicks(void) const { return CONNECT_TICKS - 5 * StatLevel(Stat::QuickFingers); }

uint8_t Sim::BusyPercent(void) const {
	int16_t busy = int16_t(CurrentStation().busyPercent) - LUCK_BUSY_STEP * StatLevel(Stat::Luck);
	return busy < LUCK_BUSY_FLOOR ? LUCK_BUSY_FLOOR : busy;
}

uint32_t Sim::Consolation(void) const { return CONSOLATION + StatLevel(Stat::Charm); }

uint16_t Sim::PrizePercent(void) const { return 100 + CHARM_PRIZE_STEP * StatLevel(Stat::Charm); }

void Sim::TuneStation(int8_t direction) {
	int8_t next = int8_t(m_station) + direction;
	if (next < 0 || next >= STATION_COUNT || !IsStationUnlocked(next)) {
		return;
	}

	HangUp();
	m_station = next;
	m_rollover = 0;
	StartRound();
}
