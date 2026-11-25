#include "book_sim.hh"

#include "rand.hh"

#include <EASTL/algorithm.h>
#include <psyqo/xprintf.h>
#include <stdarg.h>

// four residential books, each longer and better paid than the last, then small business. About a
// third of the people who listen buy; the rest waste your time and pay only the fee
const eastl::array<Book, BOOK_COUNT> BookSim::c_books = {{
	{.name = "Elm Street", .category = 0, .entries = 16, .dudPercent = 30, .pickupPercent = 60, .salePercent = 34,
	 .fee = 1, .bonus = 12, .price = 0, .unlockAfter = 0},
	{.name = "Oak Avenue", .category = 0, .entries = 24, .dudPercent = 25, .pickupPercent = 54, .salePercent = 34,
	 .fee = 1, .bonus = 18, .price = 20, .unlockAfter = 1},
	{.name = "Maple Heights", .category = 0, .entries = 32, .dudPercent = 20, .pickupPercent = 48, .salePercent = 33,
	 .fee = 2, .bonus = 28, .price = 45, .unlockAfter = 1},
	{.name = "Pine Ridge", .category = 0, .entries = 40, .dudPercent = 15, .pickupPercent = 42, .salePercent = 33,
	 .fee = 2, .bonus = 40, .price = 90, .unlockAfter = 2},
	{.name = "Main St Shops", .category = 1, .entries = 32, .dudPercent = 20, .pickupPercent = 50, .salePercent = 36,
	 .fee = 4, .bonus = 60, .price = 0, .unlockAfter = 0},
}};

const char* const BookSim::c_categories[CATEGORY_COUNT] = {"RESIDENTIAL", "SMALL BUSINESS"};
const char* const BookSim::c_perkNames[PERK_COUNT] = {"Fast talker", "Warm voice", "Bulk buyer"};

// all in vsync ticks
static constexpr uint16_t CONNECT_TICKS = 24;
static constexpr uint16_t DEAD_TICKS = 20;
// two rings of a US ringback (2 s on, 4 s off), giving up as the second one ends
static constexpr uint16_t NO_ANSWER_TICKS = 480;
static constexpr uint16_t PITCH_TICKS = 240;	 // a sale: the whole pitch, shorter with Fast talker
static constexpr uint16_t NO_SALE_TICKS = 150; // they listen a while, then "no thanks"
static constexpr uint16_t HANG_UP_TALK_TICKS = 40;
// a pick-up comes after the first ring (2 s of a US ringback) and before the second
static constexpr uint16_t PICKUP_MIN_TICKS = 130;
static constexpr uint16_t NOTE_RING_TICKS = 150;
static constexpr uint16_t FIRST_SUPERVISOR_TICKS = 60 * 60;
static constexpr uint16_t AUTO_STAGGER_TICKS = 30; // between two of the autodialer's calls starting
// measured on book 1 (9 hand calls, seed 12345, Redux): the call itself averages 188 ticks (3.1 s) after
// the 7th digit, and walking the keypad cursor is 12.5 presses per number. At 2 to 3 presses a second by
// hand that is 8 to 10 s a call, 18 to 23 calls in the shortest corp gap (180 s, longer when the corp
// clock pauses for a pitch). 8 is under half of the slow end; the cap is about 60% of the fast end
static constexpr uint16_t QUOTA_PER_LINE_BASE = 8, QUOTA_PER_LINE_STEP = 2, QUOTA_PER_LINE_MAX = 14;
static constexpr uint32_t LINE_BASE_PRICE = 30;
static constexpr uint32_t AUTODIALER_PRICE = 25; // doubles per level, like a line
static constexpr uint32_t AUTO_HIGH_TIER_FACTOR = 3;
static constexpr uint32_t MOTOR_PRICES[BookSim::MAX_MOTOR] = {60, 150};
static constexpr uint32_t TRAY_PRICES[BookSim::MAX_TRAY] = {80, 200};
static constexpr uint8_t MISSED_CALL_FINE = 3;
// perks: per level
static constexpr uint8_t PERK_PITCH_PERCENT = 12, PERK_PICKUP_POINTS = 4, PERK_DISCOUNT_PERCENT = 10;
static constexpr uint32_t PERK_BASE_PRICE = 4;
// standing for a promotion: a point per this many calls, and per this many sales
static constexpr uint32_t STANDING_CALLS = 10, STANDING_SALES = 2;

static constexpr const char* c_corpIdle[] = {
	"Smile and dial.",
	"Read the pitch exactly as written.",
	"Nobody likes a quiet line.",
	"The book does not dial itself.",
	"Management sees every call.",
	"Your chair is company property.",
};

uint32_t BookSim::Hash(uint32_t a, uint32_t b) const {
	uint32_t x = m_seed ^ (a * 0x9e3779b1u) ^ (b * 0x85ebca6bu);
	x ^= x >> 16;
	x *= 0x85ebca6bu;
	x ^= x >> 13;
	x *= 0xc2b2ae35u;
	x ^= x >> 16;
	return x;
}

void BookSim::Generate(void) {
	// unique four digit endings across every book and the note, so a number means one entry
	auto used = [this](uint16_t suffix, uint8_t upToBook, uint8_t upToEntry) {
		for (uint8_t b = 0; b <= upToBook && b < BOOK_COUNT; b++) {
			uint8_t n = b == upToBook ? upToEntry : c_books[b].entries;
			for (uint8_t i = 0; i < n; i++) {
				if (m_numbers[b][i] == suffix) {
					return true;
				}
			}
		}
		return false;
	};
	auto generate = [&](uint8_t b) {
		for (uint8_t i = 0; i < c_books[b].entries; i++) {
			uint16_t suffix;
			uint32_t k = 0;
			do {
				suffix = Hash(b * 64 + i, k++) % 10000;
			} while (suffix < 100 || used(suffix, b, i) || (b >= RESIDENTIAL_BOOKS && suffix == m_noteNumber));
			m_numbers[b][i] = suffix;
			m_duds[b][i] = Hash(b * 64 + i, 0xd0d) % 100 < c_books[b].dudPercent;
		}
	};
	// the residential books and the note first, in the order phase 1 drew them, so its saves keep
	// their numbers
	for (uint8_t b = 0; b < RESIDENTIAL_BOOKS; b++) {
		generate(b);
	}
	uint32_t k = 0;
	do {
		m_noteNumber = Hash(0x707e, k++) % 10000;
	} while (m_noteNumber < 100 || used(m_noteNumber, RESIDENTIAL_BOOKS, 0));
	for (uint8_t b = RESIDENTIAL_BOOKS; b < BOOK_COUNT; b++) {
		generate(b);
	}
}

void BookSim::ResetRun(void) {
	m_cash = m_earned = m_calls = m_connects = m_dudCalls = 0;
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		for (uint8_t i = 0; i < BOOK_MAX_ENTRIES; i++) {
			m_status[b][i] = 0;
		}
	}
	// the category's first book is handed to you
	m_owned = 0;
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		if (c_books[b].category == m_category && c_books[b].price == 0) {
			m_owned |= 1 << b;
		}
	}
	m_lineCount = 1;
	for (uint8_t i = 0; i < BOOK_MAX_LINES; i++) {
		m_lines[i] = BookLine();
		m_lines[i].state = i < m_lineCount ? BookLineState::Idle : BookLineState::Locked;
	}
	m_handLine = -1;
	m_lastHandLine = 0;
	m_autoLevel = m_motor = m_tray = 0;
	m_queueLength = m_queueCursor = 0;
	m_autoStagger = 0;
	m_autoShown = -1;
	m_supRinging = false;
	m_supTimer = FIRST_SUPERVISOR_TICKS;
	m_supervisorCalls = 0;
	m_quota = 0;
	m_quotaFrom = 0;
}

void BookSim::Init(Rand* rand, BookListener* listener, uint32_t seed) {
	m_rand = rand;
	m_listener = listener;
	m_seed = seed;
	Generate();
	ResetRun();
	SetCorp("Pick up the book. Start dialing.");
	printf("DIALTONE: books seed=%08x note=555%04d\n", m_seed, m_noteNumber);
#ifdef DIALTONE_SEED
	// a fixed seed is for scripted runs, which need the numbers to dial
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		printf("DIALTONE: book %d", b + 1);
		for (uint8_t i = 0; i < c_books[b].entries; i++) {
			printf(" 555%04d%s", m_numbers[b][i], m_duds[b][i] ? "x" : "");
		}
		printf("\n");
	}
#endif
}

void BookSim::SetCorp(const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m_corpLine, sizeof(m_corpLine), fmt, ap);
	va_end(ap);
	m_corpIdle = 0;
}

void BookSim::EntryNumber(uint8_t book, uint8_t entry, char* out) const {
	snprintf(out, BOOK_NUMBER_LENGTH + 1, "555%04d", m_numbers[book][entry]);
}

void BookSim::NoteNumber(char* out) const { snprintf(out, BOOK_NUMBER_LENGTH + 1, "555%04d", m_noteNumber); }

uint8_t BookSim::BookDialed(uint8_t book) const {
	uint8_t n = 0;
	for (uint8_t i = 0; i < c_books[book].entries; i++) {
		n += m_status[book][i] != uint8_t(Entry::Unknown);
	}
	return n;
}

uint8_t BookSim::BooksIn(uint8_t category) const {
	uint8_t n = 0;
	for (const auto& book : c_books) {
		n += book.category == category;
	}
	return n;
}

uint8_t BookSim::BooksDoneIn(uint8_t category) const {
	uint8_t n = 0;
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		n += c_books[b].category == category && Owned(b) && BookDone(b);
	}
	return n;
}

uint8_t BookSim::FirstOwned(void) const {
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		if (Owned(b) && !BookDone(b)) {
			return b;
		}
	}
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		if (Owned(b)) {
			return b;
		}
	}
	return 0;
}

uint8_t BookSim::NextOwned(uint8_t book, int8_t direction) const {
	for (uint8_t step = 1; step < BOOK_COUNT; step++) {
		uint8_t b = (book + BOOK_COUNT + direction * step) % BOOK_COUNT;
		if (Owned(b)) {
			return b;
		}
	}
	return book;
}

bool BookSim::NextUncalled(uint8_t& book, uint8_t& entry) const {
	// numbers already on the autodialer's list are its job: skip them unless nothing else is left
	for (uint8_t pass = 0; pass < 2; pass++) {
		uint8_t b = book;
		for (uint8_t books = 0; books <= BOOK_COUNT; books++) {
			if (Owned(b)) {
				uint8_t n = c_books[b].entries;
				// the first book from the entry after the cursor, the others from the top
				uint8_t from = books == 0 ? entry + 1 : 0;
				uint8_t count = books == 0 ? n : n;
				for (uint8_t k = 0; k < count; k++) {
					uint8_t i = (from + k) % n;
					if (m_status[b][i] == uint8_t(Entry::Unknown) && (pass == 1 || !IsQueued(b, i))) {
						book = b;
						entry = i;
						return true;
					}
				}
			}
			b = (b + 1) % BOOK_COUNT;
		}
	}
	return false;
}

bool BookSim::ForSale(uint8_t book) const {
	const auto& b = c_books[book];
	return !Owned(book) && b.category == m_category && BooksDoneIn(m_category) >= b.unlockAfter;
}

uint8_t BookSim::ForSaleList(uint8_t* out) const {
	uint8_t n = 0;
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		if (ForSale(b)) {
			out[n++] = b;
		}
	}
	return n;
}

bool BookSim::BuyBook(uint8_t book) {
	if (!ForSale(book) || m_cash < c_books[book].price) {
		return false;
	}
	m_cash -= c_books[book].price;
	m_owned |= 1 << book;
	printf("DIALTONE: bought book %d %s price=%d cash=%d\n", book + 1, c_books[book].name, c_books[book].price,
		   m_cash);
	return true;
}

uint8_t BookSim::PickupPercent(uint8_t book) const {
	return eastl::min<uint8_t>(95, c_books[book].pickupPercent + PERK_PICKUP_POINTS * m_perks[PERK_PICKUP]);
}

uint8_t BookSim::SalePercent(uint8_t book) const { return PickupPercent(book) * c_books[book].salePercent / 100; }

uint16_t BookSim::PitchTicks(void) const {
	return PITCH_TICKS * (100 - PERK_PITCH_PERCENT * m_perks[PERK_PITCH]) / 100;
}

uint8_t BookSim::LinesBusy(void) const {
	uint8_t n = 0;
	for (uint8_t i = 0; i < m_lineCount; i++) {
		n += m_lines[i].state != BookLineState::Idle;
	}
	return n;
}

int8_t BookSim::FreeLine(void) const {
	for (uint8_t i = 0; i < m_lineCount; i++) {
		if (m_lines[i].state == BookLineState::Idle) {
			return i;
		}
	}
	return -1;
}

void BookSim::Pay(uint32_t amount) {
	m_cash += amount;
	m_earned += amount;
}

bool BookSim::PressDigit(char digit) {
	if (m_handLine < 0) {
		int8_t free = FreeLine();
		if (free < 0) {
			auto& line = m_lines[m_lastHandLine];
			if (line.state == BookLineState::Idle) {
				line.outcome = CallOutcome::AllBusy;
			}
			return false;
		}
		m_handLine = m_lastHandLine = free;
		auto& line = m_lines[free];
		line.state = BookLineState::Dialing;
		line.byMachine = false;
		line.digitCount = 0;
		line.outcome = CallOutcome::None;
		line.paid = 0;
		printf("DIALTONE: hand start line=%d t=%d\n", free, m_simTick);
	}
	auto& line = m_lines[m_handLine];
	if (line.state != BookLineState::Dialing || line.digitCount >= BOOK_NUMBER_LENGTH) {
		return true;
	}
	line.digits[line.digitCount++] = digit;
	line.digits[line.digitCount] = 0;
	m_listener->OnDigit(m_handLine, digit);
	if (line.digitCount == BOOK_NUMBER_LENGTH) {
		line.timer = CONNECT_TICKS;
		printf("DIALTONE: hand dialed line=%d t=%d\n", m_handLine, m_simTick);
	}
	return true;
}

void BookSim::DeleteDigit(void) {
	if (m_handLine < 0) {
		return;
	}
	auto& line = m_lines[m_handLine];
	if (line.state != BookLineState::Dialing || line.digitCount == BOOK_NUMBER_LENGTH) {
		return;
	}
	if (--line.digitCount == 0) {
		line.state = BookLineState::Idle;
		m_handLine = -1;
	}
	line.digits[line.digitCount] = 0;
}

void BookSim::HangUp(void) {
	if (m_handLine < 0) {
		return;
	}
	auto& line = m_lines[m_handLine];
	bool calling = line.state == BookLineState::Ringing || line.state == BookLineState::Talking ||
				   line.digitCount == BOOK_NUMBER_LENGTH;
	line.state = BookLineState::Idle;
	line.outcome = calling ? CallOutcome::Abandoned : CallOutcome::None;
	line.paid = 0;
	m_listener->OnHangUp(m_handLine);
	m_handLine = -1;
}

void BookSim::StartCall(uint8_t index) {
	auto& line = m_lines[index];
	line.book = 0;
	line.entry = ENTRY_NONE;
	bool ours = line.digits[0] == '5' && line.digits[1] == '5' && line.digits[2] == '5';
	uint16_t suffix = 0;
	for (uint8_t i = 3; i < BOOK_NUMBER_LENGTH; i++) {
		suffix = suffix * 10 + (line.digits[i] - '0');
		ours &= line.digits[i] >= '0' && line.digits[i] <= '9';
	}
	// any book you own counts, finished ones too
	for (uint8_t b = 0; ours && b < BOOK_COUNT && line.entry == ENTRY_NONE; b++) {
		if (!Owned(b)) {
			continue;
		}
		for (uint8_t i = 0; i < c_books[b].entries; i++) {
			if (m_numbers[b][i] == suffix) {
				line.book = b;
				line.entry = i;
				break;
			}
		}
	}
	if (ours && m_noteState >= 1 && m_noteState < 3 && suffix == m_noteNumber && !line.byMachine) {
		line.entry = ENTRY_NOTE;
	}
	m_calls++;

	line.state = BookLineState::Ringing;
	if (line.entry == ENTRY_NOTE) {
		line.pending = CallOutcome::Note;
		line.timer = NOTE_RING_TICKS;
		m_listener->OnRing(index);
		return;
	}
	if (line.entry == ENTRY_NONE || m_duds[line.book][line.entry]) {
		line.pending = CallOutcome::NotInService;
		line.timer = DEAD_TICKS;
		m_dudCalls++;
		m_listener->OnDead(index);
		return;
	}

	bool sold = m_status[line.book][line.entry] == uint8_t(Entry::Sold);
	// somebody who already bought hangs up on you; of the ones who listen, about a third buy
	if (!sold && m_rand->rand(100) < PickupPercent(line.book)) {
		line.pending = m_rand->rand(100) < c_books[line.book].salePercent ? CallOutcome::Sold : CallOutcome::NoSale;
		line.timer = PICKUP_MIN_TICKS + m_rand->rand(91);
	} else if (sold || m_rand->rand(2)) {
		line.pending = CallOutcome::HungUp;
		line.timer = PICKUP_MIN_TICKS + m_rand->rand(91);
	} else {
		line.pending = CallOutcome::NoAnswer;
		line.timer = NO_ANSWER_TICKS;
	}
	m_listener->OnRing(index);
}

void BookSim::EndCall(uint8_t index, CallOutcome outcome) {
	auto& line = m_lines[index];
	line.state = BookLineState::Idle;
	line.outcome = outcome;
	line.paid = 0;
	if (line.entry < BOOK_MAX_ENTRIES) {
		const auto& book = c_books[line.book];
		uint8_t& status = m_status[line.book][line.entry];
		bool wasDone = BookDone(line.book);
		switch (outcome) {
			case CallOutcome::NotInService:
				status = uint8_t(Entry::Dud);
				break;
			case CallOutcome::NoAnswer:
			case CallOutcome::HungUp:
			case CallOutcome::NoSale:
				line.paid = book.fee;
				if (status != uint8_t(Entry::Sold)) {
					status = uint8_t(outcome == CallOutcome::NoAnswer ? Entry::NoAnswer
									 : outcome == CallOutcome::HungUp ? Entry::HungUp
																	  : Entry::NoSale);
				}
				break;
			case CallOutcome::Sold:
				line.paid = book.fee + book.bonus;
				status = uint8_t(Entry::Sold);
				m_connects++;
				break;
			default:
				break;
		}
		Pay(line.paid);
		if (!wasDone && BookDone(line.book)) {
			printf("DIALTONE: book %d done calls=%d connects=%d earned=%d\n", line.book + 1, m_calls, m_connects,
				   m_earned);
			PruneQueue();
			m_listener->OnBookDone(line.book);
		}
	}
	printf("DIALTONE: call end line=%d machine=%d book=%d entry=%d outcome=%d paid=%d calls=%d t=%d\n", index,
		   line.byMachine, line.book + 1, line.entry, uint8_t(outcome), line.paid, m_calls, m_simTick);
	if (outcome == CallOutcome::Sold) {
		m_listener->OnSold(index);
	} else {
		m_listener->OnHangUp(index);
	}
	if (index == m_handLine) {
		m_handLine = -1;
	}
	if (line.byMachine) {
		// the line it used rests before it dials on it again
		line.timer = AutodialPauseTicks();
	}
	CheckNote();
}

void BookSim::CheckNote(void) {
	for (uint8_t b = 0; b < RESIDENTIAL_BOOKS; b++) {
		if (!Owned(b) || !BookDone(b)) {
			return;
		}
	}
	if (m_noteState == 0) {
		m_noteState = 1;
		printf("DIALTONE: note slipped in, 555%04d calls=%d earned=%d connects=%d\n", m_noteNumber, m_calls, m_earned,
			   m_connects);
		m_listener->OnNote();
	}
}

void BookSim::TickLine(uint8_t index) {
	auto& line = m_lines[index];
	switch (line.state) {
		case BookLineState::Idle:
			// an autodialer line resting after its call
			if (line.byMachine && line.timer) {
				line.timer--;
			}
			break;
		case BookLineState::Dialing:
			if (line.byMachine && line.digitCount < BOOK_NUMBER_LENGTH) {
				if (++line.timer >= AutodialDigitTicks()) {
					line.timer = 0;
					char digit = line.digits[line.digitCount++];
					m_listener->OnDigit(index, digit);
					if (line.digitCount == BOOK_NUMBER_LENGTH) {
						line.timer = CONNECT_TICKS;
					}
				}
			} else if (line.digitCount == BOOK_NUMBER_LENGTH && --line.timer == 0) {
				StartCall(index);
			}
			break;
		case BookLineState::Ringing:
			// the scene can keep a pick-up ringing while the caller's voice line loads
			if (line.timer == 1 &&
				(line.pending == CallOutcome::Sold || line.pending == CallOutcome::NoSale ||
				 line.pending == CallOutcome::HungUp) &&
				m_listener->HoldPickUp(index)) {
				break;
			}
			if (--line.timer) {
				break;
			}
			if (line.pending == CallOutcome::Sold || line.pending == CallOutcome::NoSale) {
				line.state = BookLineState::Talking;
				line.timer = line.pending == CallOutcome::Sold ? PitchTicks() : NO_SALE_TICKS;
				printf("DIALTONE: listening line=%d machine=%d %s ticks=%d\n", index, line.byMachine,
					   line.pending == CallOutcome::Sold ? "sale" : "no-sale", line.timer);
				m_listener->OnHangUp(index); // ringback stops, they are listening
			} else if (line.pending == CallOutcome::HungUp) {
				line.state = BookLineState::Talking;
				line.timer = HANG_UP_TALK_TICKS;
				m_listener->OnHangUp(index);
			} else if (line.pending == CallOutcome::Note) {
				m_noteState = 2;
				line.state = BookLineState::Idle;
				line.outcome = CallOutcome::Note;
				if (index == m_handLine) {
					m_handLine = -1;
				}
				m_listener->OnHangUp(index);
				printf("DIALTONE: note answered calls=%d earned=%d connects=%d\n", m_calls, m_earned, m_connects);
				m_listener->OnNoteAnswered();
			} else {
				EndCall(index, line.pending);
			}
			break;
		case BookLineState::Talking:
			if (--line.timer == 0) {
				EndCall(index, line.pending);
			}
			break;
		default:
			break;
	}
}

uint8_t BookSim::AutoLines(void) const {
	if (!m_autoLevel) {
		return 0;
	}
	// one line stays the handset's; with a single line the two take turns on it
	uint8_t cap = m_lineCount > 1 ? m_lineCount - 1 : 1;
	return eastl::min(m_autoLevel, cap);
}

uint8_t BookSim::AutoActive(void) const {
	uint8_t n = 0;
	for (uint8_t i = 0; i < m_lineCount; i++) {
		n += m_lines[i].byMachine && m_lines[i].state != BookLineState::Idle;
	}
	return n;
}

uint8_t BookSim::AutodialDigitTicks(void) const {
	static constexpr uint8_t ticks[MAX_MOTOR + 1] = {40, 22, 10};
	return ticks[m_motor];
}

uint16_t BookSim::AutodialPauseTicks(void) const {
	static constexpr uint16_t ticks[MAX_MOTOR + 1] = {180, 100, 50};
	return ticks[m_motor];
}

void BookSim::TickAutodialer(void) {
	// up to AutoLines() calls at once, straight down its list and round again, duds and all
	if (m_autoStagger) {
		m_autoStagger--;
	}
	if (!m_autoLevel || m_queueLength == 0 || m_autoStagger) {
		return;
	}
	// resting lines count against it, so one level is still one call and a rest, as in phase 1
	uint8_t busy = 0;
	for (uint8_t i = 0; i < m_lineCount; i++) {
		busy += m_lines[i].byMachine && (m_lines[i].state != BookLineState::Idle || m_lines[i].timer);
	}
	if (busy >= AutoLines()) {
		return;
	}
	int8_t free = -1;
	for (uint8_t i = 0; i < m_lineCount; i++) {
		const auto& l = m_lines[i];
		if (l.state == BookLineState::Idle && i != m_handLine && !(l.byMachine && l.timer)) {
			free = i;
			break;
		}
	}
	if (free < 0) {
		return;
	}
	// the next number on the list that no other line of its is already calling
	int8_t pick = -1;
	for (uint8_t k = 0; k < m_queueLength && pick < 0; k++) {
		if (m_queueCursor >= m_queueLength) {
			m_queueCursor = 0;
		}
		const auto& item = m_queue[m_queueCursor];
		m_queueCursor++; // wrapped when it is next used, so numbers listed meanwhile still get their turn
		bool taken = false;
		for (uint8_t i = 0; i < m_lineCount; i++) {
			const auto& l = m_lines[i];
			taken |= l.byMachine && l.state != BookLineState::Idle && l.book == item.book && l.entry == item.entry;
		}
		if (!taken) {
			pick = int8_t((m_queueCursor + m_queueLength - 1) % m_queueLength);
		}
	}
	if (pick < 0) {
		return;
	}
	const auto& item = m_queue[pick];
	auto& line = m_lines[free];
	line.state = BookLineState::Dialing;
	line.byMachine = true;
	line.digitCount = 0;
	line.timer = 0;
	line.outcome = CallOutcome::None;
	line.paid = 0;
	line.book = item.book;
	line.entry = item.entry;
	EntryNumber(item.book, item.entry, line.digits);
	m_autoStagger = AUTO_STAGGER_TICKS;
	// its readout follows one call, and moves on when that one is over
	if (m_autoShown < 0 || m_lines[m_autoShown].state == BookLineState::Idle || !m_lines[m_autoShown].byMachine) {
		m_autoShown = free;
	}
	printf("DIALTONE: autodial line=%d 555-%04d active=%d/%d\n", free, m_numbers[item.book][item.entry], AutoActive(),
		   AutoLines());
}

uint8_t BookSim::PruneQueue(void) {
	uint8_t kept = 0, cursor = 0;
	for (uint8_t i = 0; i < m_queueLength; i++) {
		const auto& item = m_queue[i];
		if (Owned(item.book) && !BookDone(item.book)) {
			if (i < m_queueCursor) {
				cursor++;
			}
			m_queue[kept++] = item;
		}
	}
	uint8_t dropped = m_queueLength - kept;
	m_queueLength = kept;
	m_queueCursor = cursor;
	if (dropped) {
		printf("DIALTONE: list dropped %d numbers from finished books, %d left\n", dropped, kept);
	}
	return dropped;
}

bool BookSim::IsQueued(uint8_t book, uint8_t entry) const {
	for (uint8_t i = 0; i < m_queueLength; i++) {
		if (m_queue[i].book == book && m_queue[i].entry == entry) {
			return true;
		}
	}
	return false;
}

bool BookSim::ToggleQueued(uint8_t book, uint8_t entry) {
	for (uint8_t i = 0; i < m_queueLength; i++) {
		if (m_queue[i].book == book && m_queue[i].entry == entry) {
			for (uint8_t j = i; j + 1 < m_queueLength; j++) {
				m_queue[j] = m_queue[j + 1];
			}
			m_queueLength--;
			if (m_queueCursor > i) {
				m_queueCursor--;
			}
			return true;
		}
	}
	if (m_queueLength >= QueueCapacity()) {
		return false;
	}
	m_queue[m_queueLength++] = {book, entry};
	return true;
}

void BookSim::TickSupervisor(void) {
	if (m_noteState == 2) {
		return; // the note's call has the room to itself
	}
	// someone listening on your own line holds the corp's clock: their call never comes mid-pitch
	if (!m_supRinging && m_handLine >= 0 && m_lines[m_handLine].state == BookLineState::Talking) {
		return;
	}
	if (--m_supTimer) {
		return;
	}
	if (!m_supRinging) {
		m_supRinging = true;
		m_supTimer = RING_TICKS;
		m_listener->OnSupervisorRing(true);
		printf("DIALTONE: supervisor ringing calls=%d\n", m_calls);
		return;
	}
	// nobody picked up
	m_supRinging = false;
	m_supTimer = 60 * 60;
	uint32_t fine = eastl::min<uint32_t>(MISSED_CALL_FINE, m_cash);
	m_cash -= fine;
	SetCorp("You missed my call. -$%d.", fine);
	m_listener->OnSupervisorRing(false);
	printf("DIALTONE: supervisor missed fine=%d\n", fine);
}

bool BookSim::AnswerSupervisor(void) {
	if (!m_supRinging) {
		return false;
	}
	m_supRinging = false;
	m_listener->OnSupervisorRing(false);
	// the quota is per line you own, so it stays reachable by hand on one line; m_supervisorCalls is
	// your standing with the corp: up one per quota met, down one per quota missed
	bool met = m_quota != 0 && QuotaDone() >= m_quota;
	m_supResult = m_quota == 0 ? 0 : met ? 1 : 2;
	if (m_quota != 0) {
		if (met) {
			m_supervisorCalls++;
		} else if (m_supervisorCalls) {
			m_supervisorCalls--;
		}
	}
	uint16_t perLine = eastl::min<uint16_t>(QUOTA_PER_LINE_BASE + QUOTA_PER_LINE_STEP * m_supervisorCalls,
											QUOTA_PER_LINE_MAX);
	uint16_t next = perLine * m_lineCount;
	if (m_quota == 0) {
		SetCorp("Welcome aboard. %d calls by my next.", next);
	} else if (met) {
		uint32_t bonus = m_quota / 2;
		Pay(bonus);
		SetCorp("Quota met, +$%d. Now %d calls.", bonus, next);
	} else {
		uint32_t fine = eastl::min<uint32_t>(m_quota - QuotaDone(), m_cash);
		m_cash -= fine;
		SetCorp("%d of %d calls? -$%d. Now %d.", QuotaDone(), m_quota, fine, next);
	}
	printf("DIALTONE: supervisor answered quota %d/%d next=%d cash=%d\n", QuotaDone(), m_quota, next, m_cash);
	m_quota = next;
	m_quotaFrom = m_calls;
	m_supTimer = (180 + m_rand->rand(61)) * 60;
	return true;
}

uint32_t BookSim::AutomationPrice(uint32_t base) const {
	return base * (100 - PERK_DISCOUNT_PERCENT * m_perks[PERK_AUTOMATION]) / 100;
}

bool BookSim::ShopLocked(uint8_t item) const {
	return item == 1 && m_autoLevel >= AUTO_RESIDENTIAL_MAX && m_autoLevel < AUTO_MAX && m_promotions == 0;
}

bool BookSim::ShopNeedsLine(uint8_t item) const { return item == 1 && m_autoLevel == 0 && m_lineCount < 2; }

uint32_t BookSim::ShopPrice(uint8_t item) const {
	switch (item) {
		case 0:
			return m_lineCount < BOOK_MAX_LINES ? LINE_BASE_PRICE << (m_lineCount - 1) : 0;
		case 1: {
			if (m_autoLevel >= AUTO_MAX) {
				return 0;
			}
			uint32_t base = AUTODIALER_PRICE << m_autoLevel;
			if (m_autoLevel >= AUTO_RESIDENTIAL_MAX) {
				base *= AUTO_HIGH_TIER_FACTOR;
			}
			return AutomationPrice(base);
		}
		case 2:
			return m_autoLevel && m_motor < MAX_MOTOR ? AutomationPrice(MOTOR_PRICES[m_motor]) : 0;
		default:
			return m_autoLevel && m_tray < MAX_TRAY ? AutomationPrice(TRAY_PRICES[m_tray]) : 0;
	}
}

bool BookSim::Buy(uint8_t item) {
	uint32_t price = ShopPrice(item);
	if (price == 0 || m_cash < price || ShopLocked(item) || ShopNeedsLine(item)) {
		return false;
	}
	m_cash -= price;
	switch (item) {
		case 0:
			m_lines[m_lineCount++].state = BookLineState::Idle;
			break;
		case 1:
			m_autoLevel++;
			break;
		case 2:
			m_motor++;
			break;
		default:
			m_tray++;
			break;
	}
	printf("DIALTONE: bought item %d price=%d cash=%d lines=%d auto=%d (runs %d) motor=%d tray=%d\n", item, price,
		   m_cash, m_lineCount, m_autoLevel, AutoLines(), m_motor, m_tray);
	return true;
}

bool BookSim::PromotionReady(void) const {
	if (m_category == 0) {
		return m_noteState == 3;
	}
	return BooksDoneIn(m_category) == BooksIn(m_category);
}

uint32_t BookSim::PromotionStanding(void) const {
	return eastl::max<uint32_t>(1, m_calls / STANDING_CALLS + m_connects / STANDING_SALES);
}

void BookSim::Promote(void) {
	uint32_t pay = PromotionStanding();
	m_standing += pay;
	m_promotions++;
	m_category = eastl::min<uint8_t>(m_category + 1, CATEGORY_COUNT - 1);
	printf("DIALTONE: promoted #%d to %s standing +%d = %d (calls=%d sales=%d)\n", m_promotions,
		   c_categories[m_category], pay, m_standing, m_calls, m_connects);
	// new books, a new desk
	m_seed = m_seed * 1664525u + 1013904223u;
	Generate();
	ResetRun();
	SetCorp("Promoted. Do not let it show.");
#ifdef DIALTONE_SEED
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		if (c_books[b].category == m_category) {
			printf("DIALTONE: book %d", b + 1);
			for (uint8_t i = 0; i < c_books[b].entries; i++) {
				printf(" 555%04d%s", m_numbers[b][i], m_duds[b][i] ? "x" : "");
			}
			printf("\n");
		}
	}
#endif
}

uint32_t BookSim::PerkPrice(uint8_t perk) const {
	return m_perks[perk] < PERK_MAX ? PERK_BASE_PRICE * (m_perks[perk] + 1) : 0;
}

bool BookSim::BuyPerk(uint8_t perk) {
	uint32_t price = PerkPrice(perk);
	if (price == 0 || m_standing < price) {
		return false;
	}
	m_standing -= price;
	m_perks[perk]++;
	printf("DIALTONE: perk %s lv%d price=%d standing=%d pitch=%d pickup+%d discount=%d%%\n", c_perkNames[perk],
		   m_perks[perk], price, m_standing, PitchTicks(), PERK_PICKUP_POINTS * m_perks[PERK_PICKUP],
		   PERK_DISCOUNT_PERCENT * m_perks[PERK_AUTOMATION]);
	return true;
}

// every owned number called, nothing in flight and too little cash for the cheapest book on sale: nobody
// could ever pay for the next book, so the corp hands the cheapest one over
void BookSim::CheckRescue(void) {
	if (m_supRinging || LinesBusy() || AutoActive()) {
		return;
	}
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		if (Owned(b) && !BookDone(b)) {
			return;
		}
	}
	uint8_t books[BOOK_COUNT];
	uint8_t count = ForSaleList(books);
	if (!count) {
		return;
	}
	uint8_t cheapest = books[0];
	for (uint8_t i = 1; i < count; i++) {
		if (c_books[books[i]].price < c_books[cheapest].price) {
			cheapest = books[i];
		}
	}
	if (m_cash >= c_books[cheapest].price) {
		return;
	}
	m_owned |= 1 << cheapest;
	SetCorp("Take %s. On the house.", c_books[cheapest].name);
	printf("DIALTONE: corp gave book %d %s cash=%d price=%d\n", cheapest + 1, c_books[cheapest].name, m_cash,
		   c_books[cheapest].price);
	m_listener->OnBookGiven(cheapest);
}

void BookSim::Tick(uint32_t ticks) {
	while (ticks--) {
		m_simTick++;
		for (uint8_t i = 0; i < m_lineCount; i++) {
			TickLine(i);
		}
		TickAutodialer();
		TickSupervisor();
		if (m_simTick % 60 == 0) {
			CheckRescue();
		}
		// a quiet corp line gets a new line of flavour now and then
		if (++m_corpIdle >= 25 * 60 && !m_supRinging) {
			SetCorp("%s", c_corpIdle[m_rand->rand(sizeof(c_corpIdle) / sizeof(c_corpIdle[0]))]);
		}
	}
}

void BookSim::Save(BookSave& out) const {
	out = BookSave{};
	out.seed = m_seed;
	out.cash = m_cash;
	out.earned = m_earned;
	out.calls = m_calls;
	out.connects = m_connects;
	out.dudCalls = m_dudCalls;
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		for (uint8_t i = 0; i < BOOK_MAX_ENTRIES; i++) {
			out.status[b][i] = m_status[b][i];
		}
	}
	out.lines = m_lineCount;
	out.autoLevel = m_autoLevel;
	out.motor = m_motor;
	out.tray = m_tray;
	out.queueLength = m_queueLength;
	out.queueCursor = m_queueCursor;
	for (uint8_t i = 0; i < QUEUE_MAX; i++) {
		out.queue[i] = m_queue[i];
	}
	out.noteState = m_noteState == 2 ? 1 : m_noteState; // a call cut off by the save is not over
	out.owned = m_owned;
	out.supervisorCalls = m_supervisorCalls;
	out.quota = m_quota;
	out.quotaFrom = m_quotaFrom;
	out.standing = m_standing;
	for (uint8_t p = 0; p < PERK_COUNT; p++) {
		out.perks[p] = m_perks[p];
	}
	out.category = m_category;
	out.promotions = m_promotions;
}

void BookSim::Migrate(const BookSaveV1& in, BookSave& out) {
	out = BookSave{};
	out.seed = in.seed;
	out.cash = in.cash;
	out.earned = in.earned;
	out.calls = in.calls;
	out.connects = in.connects;
	out.dudCalls = in.dudCalls;
	// phase 1 handed you the books in order: every finished one and the first unfinished one are yours
	bool open = true;
	for (uint8_t b = 0; b < RESIDENTIAL_BOOKS; b++) {
		uint8_t dialed = 0;
		for (uint8_t i = 0; i < BOOK_MAX_ENTRIES; i++) {
			out.status[b][i] = in.status[b][i];
			dialed += i < c_books[b].entries && in.status[b][i] != 0;
		}
		if (open) {
			out.owned |= 1 << b;
		}
		open &= dialed == c_books[b].entries;
	}
	out.lines = in.lines;
	out.autoLevel = in.autodialer ? 1 : 0;
	out.motor = in.motor;
	out.tray = in.tray;
	out.queueLength = in.queueLength;
	out.queueCursor = in.queueCursor;
	for (uint8_t i = 0; i < QUEUE_MAX; i++) {
		out.queue[i] = in.queue[i];
	}
	out.noteState = in.noteState;
	out.supervisorCalls = in.supervisorCalls;
	out.quota = in.quota;
	out.quotaFrom = in.quotaFrom;
	printf("DIALTONE: migrated a phase 1 save: owned=%02x auto=%d cash=%d calls=%d note=%d\n", out.owned,
		   out.autoLevel, out.cash, out.calls, out.noteState);
}

void BookSim::Load(const BookSave& in) {
	m_seed = in.seed;
	Generate();
	m_cash = in.cash;
	m_earned = in.earned;
	m_calls = in.calls;
	m_connects = in.connects;
	m_dudCalls = in.dudCalls;
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		for (uint8_t i = 0; i < BOOK_MAX_ENTRIES; i++) {
			m_status[b][i] =
				i < c_books[b].entries && in.status[b][i] <= uint8_t(Entry::NoSale) ? in.status[b][i] : 0;
		}
	}
	m_category = eastl::min<uint8_t>(in.category, CATEGORY_COUNT - 1);
	m_owned = in.owned & ((1 << BOOK_COUNT) - 1);
	if (!m_owned) {
		m_owned = 1 << (m_category == 0 ? 0 : RESIDENTIAL_BOOKS);
	}
	m_lineCount = eastl::clamp<uint8_t>(in.lines, 1, BOOK_MAX_LINES);
	for (uint8_t i = 0; i < BOOK_MAX_LINES; i++) {
		m_lines[i] = BookLine();
		m_lines[i].state = i < m_lineCount ? BookLineState::Idle : BookLineState::Locked;
	}
	m_handLine = -1;
	m_lastHandLine = 0;
	m_autoLevel = eastl::min<uint8_t>(in.autoLevel, AUTO_MAX);
	m_motor = eastl::min<uint8_t>(in.motor, MAX_MOTOR);
	m_tray = eastl::min<uint8_t>(in.tray, MAX_TRAY);
	m_autoStagger = 0;
	m_autoShown = -1;
	m_queueLength = 0;
	for (uint8_t i = 0; i < eastl::min<uint8_t>(in.queueLength, QueueCapacity()); i++) {
		if (in.queue[i].book < BOOK_COUNT && in.queue[i].entry < c_books[in.queue[i].book].entries) {
			m_queue[m_queueLength++] = in.queue[i];
		}
	}
	m_queueCursor = m_queueLength ? in.queueCursor % m_queueLength : 0;
	PruneQueue();
	m_noteState = eastl::min<uint8_t>(in.noteState, 3);
	m_supervisorCalls = in.supervisorCalls;
	m_quota = in.quota;
	m_quotaFrom = eastl::min(in.quotaFrom, m_calls);
	m_supRinging = false;
	m_supTimer = FIRST_SUPERVISOR_TICKS;
	m_standing = in.standing;
	for (uint8_t p = 0; p < PERK_COUNT; p++) {
		m_perks[p] = eastl::min<uint8_t>(in.perks[p], PERK_MAX);
	}
	m_promotions = in.promotions;
	CheckNote();
	printf("DIALTONE: books loaded seed=%08x cash=%d calls=%d owned=%02x done=%d note=%d category=%d standing=%d\n",
		   m_seed, m_cash, m_calls, m_owned, BooksDoneIn(m_category), m_noteState, m_category, m_standing);
}

void BookSim::DebugFirstDone(void) {
	for (uint8_t i = 0; i < c_books[0].entries; i++) {
		m_status[0][i] = uint8_t(m_duds[0][i] ? Entry::Dud : Entry::NoAnswer);
		m_calls++;
	}
	printf("DIALTONE: first book called to the end\n");
}

void BookSim::DebugPredial(uint8_t leave) {
	for (uint8_t b = 0; b < RESIDENTIAL_BOOKS; b++) {
		m_owned |= 1 << b;
		uint8_t n = c_books[b].entries;
		if (b == RESIDENTIAL_BOOKS - 1) {
			n = n > leave ? n - leave : 0;
		}
		for (uint8_t i = 0; i < n; i++) {
			if (m_status[b][i] == uint8_t(Entry::Unknown)) {
				m_status[b][i] = uint8_t(m_duds[b][i] ? Entry::Dud : Entry::NoAnswer);
				m_calls++;
			}
		}
	}
	printf("DIALTONE: predialed, %d left in the last book\n", leave);
	CheckNote();
}
