#pragma once

#include <stdint.h>

#include <EASTL/array.h>

class Rand;

// Phone books mode: cold calls for a corporation nobody names, one residential phone book at a time.
// Every number is 555 and four digits; a book is a seeded list of them, each a dud or not for good.

static constexpr uint8_t BOOK_COUNT = 4;		 // residential books, the only category in phase 1
static constexpr uint8_t BOOK_MAX_ENTRIES = 40;
static constexpr uint8_t BOOK_MAX_LINES = 10;
static constexpr uint8_t BOOK_NUMBER_LENGTH = 7;
static constexpr uint8_t QUEUE_MAX = 12;
static constexpr uint8_t ENTRY_NONE = 0xff; // a dialed number that is in no open book
static constexpr uint8_t ENTRY_NOTE = 0xfe; // the number on the sticky note

struct Book {
	const char* name;
	uint8_t entries;
	uint8_t dudPercent;	   // numbers out of service, fixed per number
	uint8_t pickupPercent; // chance a live number listens to the whole pitch
	uint8_t fee;		   // paid for every call that rings
	uint8_t bonus;		   // on top, when they listen
};

// what the book shows next to a number, the last thing that happened when it was called; Sold sticks
enum class Entry : uint8_t { Unknown, Dud, NoAnswer, HungUp, Sold };

enum class BookLineState : uint8_t { Locked, Idle, Dialing, Ringing, Talking };

enum class CallOutcome : uint8_t { None, NotInService, NoAnswer, HungUp, Sold, Abandoned, AllBusy, Note };

struct BookLine {
	BookLineState state = BookLineState::Locked;
	char digits[BOOK_NUMBER_LENGTH + 1] = {0};
	uint8_t digitCount = 0;
	bool byMachine = false;
	uint8_t book = 0, entry = ENTRY_NONE;
	uint16_t timer = 0;
	CallOutcome pending = CallOutcome::None; // what the call is going to be, rolled when it starts ringing
	CallOutcome outcome = CallOutcome::None; // what the last call was
	uint8_t paid = 0;
};

struct QueueItem {
	uint8_t book, entry;
};

// written raw into the memory card save
struct BookSave {
	uint32_t seed;
	uint32_t cash, earned, calls, connects, dudCalls;
	uint8_t status[BOOK_COUNT][BOOK_MAX_ENTRIES];
	uint8_t lines;
	uint8_t autodialer, motor, tray; // autodialer 0/1, then upgrade levels
	uint8_t queueLength, queueCursor;
	QueueItem queue[QUEUE_MAX];
	uint8_t noteState;
	uint8_t pad;
	uint16_t supervisorCalls;
	uint16_t quota;
	uint32_t quotaFrom; // calls made when the quota was set
};

class BookListener {
  public:
	virtual void OnDigit(uint8_t line, char digit) = 0;
	virtual void OnRing(uint8_t line) = 0;
	virtual void OnDead(uint8_t line) = 0; // not in service
	virtual void OnHangUp(uint8_t line) = 0;
	virtual void OnSold(uint8_t line) = 0;
	virtual void OnSupervisorRing(bool ringing) = 0;
	virtual void OnBookDone(uint8_t book) = 0;
	virtual void OnNote(void) = 0;		  // the sticky note turns up
	virtual void OnNoteAnswered(void) = 0; // the note's number picks up
};

class BookSim final {
  public:
	static const eastl::array<Book, BOOK_COUNT> c_books;
	static constexpr uint8_t SHOP_ENTRIES = 3;
	static constexpr uint8_t MAX_MOTOR = 2, MAX_TRAY = 2;
	static constexpr uint16_t RING_TICKS = 8 * 60;	// how long the corp phone rings before it gives up

	void Init(Rand* rand, BookListener* listener, uint32_t seed);
	void Tick(uint32_t ticks);

	// hand dialing on the keypad
	bool PressDigit(char digit); // false when every line is taken
	void DeleteDigit(void);
	void HangUp(void);

	// the autodialer's list
	bool ToggleQueued(uint8_t book, uint8_t entry); // false when the tray is full
	bool IsQueued(uint8_t book, uint8_t entry) const;
	void ClearQueue(void) { m_queueLength = m_queueCursor = 0; }

	// the corp phone
	bool AnswerSupervisor(void);
	bool SupervisorRinging(void) const { return m_supRinging; }
	const char* CorpLine(void) const { return m_corpLine; }

	// the shop: 0 the next line, 1 the autodialer then its motor, 2 its tray
	uint32_t ShopPrice(uint8_t item) const; // 0: nothing more to buy there
	bool Buy(uint8_t item);

	void Save(BookSave& out) const;
	void Load(const BookSave& in);

	// state for the ui
	uint32_t Cash(void) const { return m_cash; }
	uint32_t Earned(void) const { return m_earned; }
	uint32_t Calls(void) const { return m_calls; }
	uint32_t Connects(void) const { return m_connects; }
	uint32_t DudCalls(void) const { return m_dudCalls; }
	uint8_t Lines(void) const { return m_lineCount; }
	uint8_t LinesBusy(void) const;
	const BookLine& GetLine(uint8_t index) const { return m_lines[index]; }
	int8_t HandLine(void) const { return m_handLine; }
	int8_t LastHandLine(void) const { return m_lastHandLine; }
	uint8_t OpenBook(void) const; // the first unfinished book, the last one when all are done
	uint8_t BookDialed(uint8_t book) const;
	bool BookDone(uint8_t book) const { return BookDialed(book) == c_books[book].entries; }
	uint8_t BooksDone(void) const;
	Entry EntryStatus(uint8_t book, uint8_t entry) const { return Entry(m_status[book][entry]); }
	void EntryNumber(uint8_t book, uint8_t entry, char* out) const; // 7 digits
	void NoteNumber(char* out) const;
	bool HasAutodialer(void) const { return m_autodialer; }
	uint8_t Motor(void) const { return m_motor; }
	uint8_t Tray(void) const { return m_tray; }
	uint8_t QueueCapacity(void) const { return 4 + 4 * m_tray; }
	uint8_t QueueLength(void) const { return m_queueLength; }
	uint8_t QueueCursor(void) const { return m_queueCursor; }
	uint8_t AutodialDigitTicks(void) const;
	uint16_t AutodialPauseTicks(void) const;
	bool AutodialerWorking(void) const;
	uint16_t Quota(void) const { return m_quota; }
	uint32_t QuotaDone(void) const { return m_calls - m_quotaFrom; }
	uint16_t SupervisorCalls(void) const { return m_supervisorCalls; }
	// 0 none, 1 the note is in the book, 2 its number answered, 3 the scene is over
	uint8_t NoteState(void) const { return m_noteState; }
	void NoteSceneOver(void) { m_noteState = 3; }

	// test builds only (DIALTONE_START_CASH, DIALTONE_PREDIAL)
	void DebugCash(uint32_t amount) { Pay(amount); }
	void DebugPredial(uint8_t leave); // marks every number called but the last `leave` of the last book

  private:
	void Generate(void);
	uint32_t Hash(uint32_t a, uint32_t b) const;
	int8_t FreeLine(void) const;
	void StartCall(uint8_t index);
	void EndCall(uint8_t index, CallOutcome outcome);
	void TickLine(uint8_t index);
	void TickAutodialer(void);
	void TickSupervisor(void);
	void Pay(uint32_t amount);
	void SetCorp(const char* fmt, ...);
	void CheckNote(void);

	Rand* m_rand = nullptr;
	BookListener* m_listener = nullptr;
	uint32_t m_seed = 0;

	uint16_t m_numbers[BOOK_COUNT][BOOK_MAX_ENTRIES] = {{0}};
	uint8_t m_duds[BOOK_COUNT][BOOK_MAX_ENTRIES] = {{0}};
	uint16_t m_noteNumber = 0;
	uint8_t m_status[BOOK_COUNT][BOOK_MAX_ENTRIES] = {{0}};

	uint32_t m_cash = 0, m_earned = 0, m_calls = 0, m_connects = 0, m_dudCalls = 0;
	eastl::array<BookLine, BOOK_MAX_LINES> m_lines;
	uint8_t m_lineCount = 1;
	int8_t m_handLine = -1;
	int8_t m_lastHandLine = 0;

	bool m_autodialer = false;
	uint8_t m_motor = 0, m_tray = 0;
	QueueItem m_queue[QUEUE_MAX] = {};
	uint8_t m_queueLength = 0, m_queueCursor = 0;
	uint16_t m_autoPause = 0;

	bool m_supRinging = false;
	uint16_t m_supTimer = 0;
	uint16_t m_supervisorCalls = 0;
	uint16_t m_quota = 0;
	uint32_t m_quotaFrom = 0;
	char m_corpLine[40] = {0};
	uint16_t m_corpIdle = 0;

	uint8_t m_noteState = 0;
	uint8_t m_lastOpenBook = 0;
};
