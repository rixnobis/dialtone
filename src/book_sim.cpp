#include "book_sim.hh"

#include "rand.hh"

#include <EASTL/algorithm.h>
#include <psyqo/xprintf.h>
#include <stdarg.h>

// four residential books, each longer and better than the last: fewer duds, but people pick up less
const eastl::array<Book, BOOK_COUNT> BookSim::c_books = {{
	{.name = "Elm Street", .entries = 16, .dudPercent = 30, .pickupPercent = 40, .fee = 1, .bonus = 6},
	{.name = "Oak Avenue", .entries = 24, .dudPercent = 25, .pickupPercent = 34, .fee = 1, .bonus = 9},
	{.name = "Maple Heights", .entries = 32, .dudPercent = 20, .pickupPercent = 28, .fee = 2, .bonus = 14},
	{.name = "Pine Ridge", .entries = 40, .dudPercent = 15, .pickupPercent = 24, .fee = 2, .bonus = 20},
}};

// all in vsync ticks
static constexpr uint16_t CONNECT_TICKS = 24;
static constexpr uint16_t DEAD_TICKS = 20;
static constexpr uint16_t NO_ANSWER_TICKS = 330;
static constexpr uint16_t PITCH_TICKS = 150;
static constexpr uint16_t HANG_UP_TALK_TICKS = 40;
static constexpr uint16_t NOTE_RING_TICKS = 150;
static constexpr uint16_t FIRST_SUPERVISOR_TICKS = 30 * 60;
static constexpr uint32_t LINE_BASE_PRICE = 30;
static constexpr uint32_t AUTODIALER_PRICE = 25;
static constexpr uint32_t MOTOR_PRICES[BookSim::MAX_MOTOR] = {60, 150};
static constexpr uint32_t TRAY_PRICES[BookSim::MAX_TRAY] = {80, 200};
static constexpr uint8_t MISSED_CALL_FINE = 3;

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
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		for (uint8_t i = 0; i < c_books[b].entries; i++) {
			uint16_t suffix;
			uint32_t k = 0;
			do {
				suffix = Hash(b * 64 + i, k++) % 10000;
			} while (suffix < 100 || used(suffix, b, i));
			m_numbers[b][i] = suffix;
			m_duds[b][i] = Hash(b * 64 + i, 0xd0d) % 100 < c_books[b].dudPercent;
		}
	}
	uint32_t k = 0;
	do {
		m_noteNumber = Hash(0x707e, k++) % 10000;
	} while (m_noteNumber < 100 || used(m_noteNumber, BOOK_COUNT, 0));
}

void BookSim::Init(Rand* rand, BookListener* listener, uint32_t seed) {
	m_rand = rand;
	m_listener = listener;
	m_seed = seed;
	Generate();
	for (uint8_t i = 0; i < BOOK_MAX_LINES; i++) {
		m_lines[i] = BookLine();
		m_lines[i].state = i < m_lineCount ? BookLineState::Idle : BookLineState::Locked;
	}
	m_supTimer = FIRST_SUPERVISOR_TICKS;
	SetCorp("Pick up the book. Start dialing.");
	m_lastOpenBook = OpenBook();
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

uint8_t BookSim::BooksDone(void) const {
	uint8_t n = 0;
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		n += BookDone(b);
	}
	return n;
}

uint8_t BookSim::OpenBook(void) const {
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		if (!BookDone(b)) {
			return b;
		}
	}
	return BOOK_COUNT - 1;
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
	// any open book counts, earlier ones too
	for (uint8_t b = 0; ours && b <= OpenBook() && line.entry == ENTRY_NONE; b++) {
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

	const auto& book = c_books[line.book];
	bool sold = m_status[line.book][line.entry] == uint8_t(Entry::Sold);
	// somebody who already heard the pitch hangs up on you
	if (!sold && m_rand->rand(100) < book.pickupPercent) {
		line.pending = CallOutcome::Sold;
		line.timer = 60 + m_rand->rand(91);
	} else if (sold || m_rand->rand(2)) {
		line.pending = CallOutcome::HungUp;
		line.timer = 60 + m_rand->rand(91);
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
				line.paid = book.fee;
				if (status != uint8_t(Entry::Sold)) {
					status = uint8_t(outcome == CallOutcome::NoAnswer ? Entry::NoAnswer : Entry::HungUp);
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
			m_listener->OnBookDone(line.book);
		}
	}
	printf("DIALTONE: call end line=%d machine=%d book=%d entry=%d outcome=%d paid=%d calls=%d\n", index,
		   line.byMachine, line.book + 1, line.entry, uint8_t(outcome), line.paid, m_calls);
	if (outcome == CallOutcome::Sold) {
		m_listener->OnSold(index);
	} else {
		m_listener->OnHangUp(index);
	}
	if (index == m_handLine) {
		m_handLine = -1;
	}
	if (line.byMachine) {
		m_autoPause = AutodialPauseTicks();
	}
	CheckNote();
}

void BookSim::CheckNote(void) {
	if (m_noteState == 0 && BooksDone() == BOOK_COUNT) {
		m_noteState = 1;
		printf("DIALTONE: note slipped in, 555%04d calls=%d earned=%d connects=%d\n", m_noteNumber, m_calls, m_earned,
			   m_connects);
		m_listener->OnNote();
	}
}

void BookSim::TickLine(uint8_t index) {
	auto& line = m_lines[index];
	switch (line.state) {
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
			if (--line.timer) {
				break;
			}
			if (line.pending == CallOutcome::Sold) {
				line.state = BookLineState::Talking;
				line.timer = PITCH_TICKS;
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

bool BookSim::AutodialerWorking(void) const {
	for (uint8_t i = 0; i < m_lineCount; i++) {
		if (m_lines[i].byMachine && m_lines[i].state != BookLineState::Idle) {
			return true;
		}
	}
	return false;
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
	// one call at a time, straight down its list and round again, duds and all
	if (!m_autodialer || m_queueLength == 0 || AutodialerWorking()) {
		return;
	}
	if (m_autoPause) {
		m_autoPause--;
		return;
	}
	int8_t free = -1;
	for (uint8_t i = 0; i < m_lineCount; i++) {
		if (m_lines[i].state == BookLineState::Idle && i != m_handLine) {
			free = i;
			break;
		}
	}
	if (free < 0) {
		return;
	}
	if (m_queueCursor >= m_queueLength) {
		m_queueCursor = 0;
	}
	const auto& item = m_queue[m_queueCursor];
	m_queueCursor++; // wrapped when it is next used, so numbers listed meanwhile still get their turn
	auto& line = m_lines[free];
	line.state = BookLineState::Dialing;
	line.byMachine = true;
	line.digitCount = 0;
	line.timer = 0;
	line.outcome = CallOutcome::None;
	line.paid = 0;
	EntryNumber(item.book, item.entry, line.digits);
	printf("DIALTONE: autodial line=%d 555-%04d\n", free, m_numbers[item.book][item.entry]);
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
	m_supTimer = 40 * 60;
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
	uint16_t next = eastl::min<uint16_t>(10 + 5 * m_supervisorCalls, 40);
	if (m_quota == 0) {
		SetCorp("Welcome aboard. %d calls by my next.", next);
	} else if (QuotaDone() >= m_quota) {
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
	m_supervisorCalls++;
	m_supTimer = (80 + m_rand->rand(31)) * 60;
	return true;
}

uint32_t BookSim::ShopPrice(uint8_t item) const {
	switch (item) {
		case 0:
			return m_lineCount < BOOK_MAX_LINES ? LINE_BASE_PRICE << (m_lineCount - 1) : 0;
		case 1:
			if (!m_autodialer) {
				return AUTODIALER_PRICE;
			}
			return m_motor < MAX_MOTOR ? MOTOR_PRICES[m_motor] : 0;
		default:
			return m_autodialer && m_tray < MAX_TRAY ? TRAY_PRICES[m_tray] : 0;
	}
}

bool BookSim::Buy(uint8_t item) {
	uint32_t price = ShopPrice(item);
	if (price == 0 || m_cash < price) {
		return false;
	}
	m_cash -= price;
	switch (item) {
		case 0:
			m_lines[m_lineCount++].state = BookLineState::Idle;
			break;
		case 1:
			if (!m_autodialer) {
				m_autodialer = true;
			} else {
				m_motor++;
			}
			break;
		default:
			m_tray++;
			break;
	}
	printf("DIALTONE: bought item %d price=%d cash=%d lines=%d auto=%d motor=%d tray=%d\n", item, price, m_cash,
		   m_lineCount, m_autodialer, m_motor, m_tray);
	return true;
}

void BookSim::Tick(uint32_t ticks) {
	while (ticks--) {
		for (uint8_t i = 0; i < m_lineCount; i++) {
			TickLine(i);
		}
		TickAutodialer();
		TickSupervisor();
		// a quiet corp line gets a new line of flavour now and then
		if (++m_corpIdle >= 25 * 60 && !m_supRinging) {
			SetCorp("%s", c_corpIdle[m_rand->rand(sizeof(c_corpIdle) / sizeof(c_corpIdle[0]))]);
		}
	}
	m_lastOpenBook = OpenBook();
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
	out.autodialer = m_autodialer;
	out.motor = m_motor;
	out.tray = m_tray;
	out.queueLength = m_queueLength;
	out.queueCursor = m_queueCursor;
	for (uint8_t i = 0; i < QUEUE_MAX; i++) {
		out.queue[i] = m_queue[i];
	}
	out.noteState = m_noteState == 2 ? 1 : m_noteState; // a call cut off by the save is not over
	out.supervisorCalls = m_supervisorCalls;
	out.quota = m_quota;
	out.quotaFrom = m_quotaFrom;
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
			m_status[b][i] = i < c_books[b].entries && in.status[b][i] <= uint8_t(Entry::Sold) ? in.status[b][i] : 0;
		}
	}
	m_lineCount = eastl::clamp<uint8_t>(in.lines, 1, BOOK_MAX_LINES);
	for (uint8_t i = 0; i < BOOK_MAX_LINES; i++) {
		m_lines[i] = BookLine();
		m_lines[i].state = i < m_lineCount ? BookLineState::Idle : BookLineState::Locked;
	}
	m_handLine = -1;
	m_lastHandLine = 0;
	m_autodialer = in.autodialer;
	m_motor = eastl::min<uint8_t>(in.motor, MAX_MOTOR);
	m_tray = eastl::min<uint8_t>(in.tray, MAX_TRAY);
	m_queueLength = 0;
	for (uint8_t i = 0; i < eastl::min<uint8_t>(in.queueLength, QueueCapacity()); i++) {
		if (in.queue[i].book < BOOK_COUNT && in.queue[i].entry < c_books[in.queue[i].book].entries) {
			m_queue[m_queueLength++] = in.queue[i];
		}
	}
	m_queueCursor = m_queueLength ? in.queueCursor % m_queueLength : 0;
	m_noteState = eastl::min<uint8_t>(in.noteState, 3);
	m_supervisorCalls = in.supervisorCalls;
	m_quota = in.quota;
	m_quotaFrom = eastl::min(in.quotaFrom, m_calls);
	m_supRinging = false;
	m_supTimer = FIRST_SUPERVISOR_TICKS;
	m_lastOpenBook = OpenBook();
	CheckNote();
	printf("DIALTONE: books loaded seed=%08x cash=%d calls=%d books=%d note=%d\n", m_seed, m_cash, m_calls,
		   BooksDone(), m_noteState);
}

void BookSim::DebugPredial(uint8_t leave) {
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		uint8_t n = c_books[b].entries;
		if (b == BOOK_COUNT - 1) {
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
