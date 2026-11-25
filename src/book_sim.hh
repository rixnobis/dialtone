#pragma once

#include <stdint.h>

#include <EASTL/array.h>

class Rand;

// Phone books mode: cold calls for a corporation nobody names. Every number is 555 and four digits;
// a book is a seeded list of them, each a dud or not for good. Books are bought, and the categories
// open one after the other by promotion.

static constexpr uint8_t RESIDENTIAL_BOOKS = 4;
static constexpr uint8_t BOOK_COUNT = 5; // the residential books, then small business (one book so far)
static constexpr uint8_t CATEGORY_COUNT = 2;
static constexpr uint8_t BOOK_MAX_ENTRIES = 40;
static constexpr uint8_t BOOK_MAX_LINES = 10;
static constexpr uint8_t BOOK_NUMBER_LENGTH = 7;
static constexpr uint8_t QUEUE_MAX = 12;
static constexpr uint8_t PERK_COUNT = 3;
static constexpr uint8_t PERK_MAX = 5;
static constexpr uint8_t ENTRY_NONE = 0xff; // a dialed number that is in no owned book
static constexpr uint8_t ENTRY_NOTE = 0xfe; // the number on the sticky note

struct Book {
	const char* name;
	uint8_t category; // 0 residential, 1 small business
	uint8_t entries;
	uint8_t dudPercent;	   // numbers out of service, fixed per number
	uint8_t pickupPercent; // chance a live number picks up and listens
	uint8_t salePercent;   // of those who listen, how many buy
	uint8_t fee;		   // paid for every call that rings
	uint8_t bonus;		   // on top, for a sale
	uint16_t price;		   // 0: the category's first book, handed to you
	uint8_t unlockAfter;   // books of its category finished before it is for sale
};

// what the book shows next to a number, the last thing that happened when it was called; Sold sticks
enum class Entry : uint8_t { Unknown, Dud, NoAnswer, HungUp, Sold, NoSale };

enum class BookLineState : uint8_t { Locked, Idle, Dialing, Ringing, Talking };

enum class CallOutcome : uint8_t { None, NotInService, NoAnswer, HungUp, Sold, Abandoned, AllBusy, Note, NoSale };

struct BookLine {
	BookLineState state = BookLineState::Locked;
	char digits[BOOK_NUMBER_LENGTH + 1] = {0};
	uint8_t digitCount = 0;
	bool byMachine = false;
	uint8_t book = 0, entry = ENTRY_NONE;
	uint16_t timer = 0;
	uint16_t rest = 0;						 // an autodialer line waits this long after its call
	CallOutcome pending = CallOutcome::None; // what the call is going to be, rolled when it starts ringing
	CallOutcome outcome = CallOutcome::None; // what the last call was
	uint8_t paid = 0;
};

struct QueueItem {
	uint8_t book, entry;
};

// save version 1 (phase 1), kept to read old saves
struct BookSaveV1 {
	uint32_t seed;
	uint32_t cash, earned, calls, connects, dudCalls;
	uint8_t status[RESIDENTIAL_BOOKS][BOOK_MAX_ENTRIES];
	uint8_t lines;
	uint8_t autodialer, motor, tray;
	uint8_t queueLength, queueCursor;
	QueueItem queue[QUEUE_MAX];
	uint8_t noteState;
	uint8_t pad;
	uint16_t supervisorCalls;
	uint16_t quota;
	uint32_t quotaFrom;
};

// save version 2, written raw into the memory card save
struct BookSave {
	uint32_t seed;
	uint32_t cash, earned, calls, connects, dudCalls;
	uint8_t status[BOOK_COUNT][BOOK_MAX_ENTRIES];
	uint8_t lines;
	uint8_t autoLevel, motor, tray; // autoLevel: calls the autodialer runs at once, 0 = none
	uint8_t queueLength, queueCursor;
	QueueItem queue[QUEUE_MAX];
	uint8_t noteState;
	uint8_t owned; // a bit per book
	uint16_t supervisorCalls;
	uint16_t quota;
	uint32_t quotaFrom;
	// kept across promotions
	uint32_t standing;
	uint8_t perks[PERK_COUNT];
	uint8_t category;
	uint16_t promotions;
	uint16_t pad;
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
	virtual void OnBookGiven(uint8_t book) = 0; // the corp hands you a book you cannot pay for
	virtual void OnNote(void) = 0;		  // the sticky note turns up
	virtual void OnNoteAnswered(void) = 0; // the note's number picks up
	// asked every tick a pick-up is due; true keeps it ringing
	virtual bool HoldPickUp(uint8_t line) { return false; }
	// asked every tick a call on the line is about to end; true keeps it Talking (the caller's voice is unfinished)
	virtual bool HoldTalk(uint8_t line) { return false; }
};

class BookSim final {
  public:
	static const eastl::array<Book, BOOK_COUNT> c_books;
	static const char* const c_categories[CATEGORY_COUNT];
	static const char* const c_perkNames[PERK_COUNT];
	// the shop's fixed items: 0 a line, 1 the autodialer then a line more for it, 2 its motor, 3 its tray
	static constexpr uint8_t SHOP_ENTRIES = 4;
	static constexpr uint8_t MAX_MOTOR = 2, MAX_TRAY = 2;
	static constexpr uint8_t AUTO_MAX = BOOK_MAX_LINES - 1;
	static constexpr uint8_t AUTO_RESIDENTIAL_MAX = 4; // levels above need a promotion and cost more
	static constexpr uint16_t RING_TICKS = 8 * 60;	   // how long the corp phone rings before it gives up
	enum Perk : uint8_t { PERK_PITCH, PERK_PICKUP, PERK_AUTOMATION };

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
	// numbers from books that are finished or not yours leave the list (returns how many went)
	uint8_t PruneQueue(void);

	// the corp phone
	bool AnswerSupervisor(void);
	// what the last answered corp call was: 0 the first one, 1 quota met, 2 quota missed
	uint8_t SupervisorResult(void) const { return m_supResult; }
	bool SupervisorRinging(void) const { return m_supRinging; }
	const char* CorpLine(void) const { return m_corpLine; }

	// the shop's fixed items
	uint32_t ShopPrice(uint8_t item) const; // 0: nothing more to buy there
	bool ShopLocked(uint8_t item) const;	// the next level needs a promotion
	bool ShopNeedsLine(uint8_t item) const; // the first autodialer needs a second line to use
	bool Buy(uint8_t item);

	// books
	bool Owned(uint8_t book) const { return m_owned & (1 << book); }
	bool ForSale(uint8_t book) const;
	uint8_t ForSaleList(uint8_t* out) const; // books for sale now, in book order
	bool BuyBook(uint8_t book);
	uint8_t Category(void) const { return m_category; }
	uint8_t BooksIn(uint8_t category) const;
	uint8_t BooksDoneIn(uint8_t category) const;
	uint8_t NextOwned(uint8_t book, int8_t direction) const; // the next owned book after `book`, wrapping
	// the next number nobody has called yet: from (book, entry) on, then the other owned books;
	// false when every owned number has been called
	bool NextUncalled(uint8_t& book, uint8_t& entry) const;
	uint8_t SalePercent(uint8_t book) const; // of every live call, with the perk
	uint8_t PickupPercent(uint8_t book) const;

	// promotion: standing for the run, perks bought with it, kept for good
	bool PromotionReady(void) const;
	uint32_t PromotionStanding(void) const; // what a promotion now would pay
	void Promote(void);
	uint32_t Standing(void) const { return m_standing; }
	uint8_t PerkLevel(uint8_t perk) const { return m_perks[perk]; }
	uint32_t PerkPrice(uint8_t perk) const; // 0: maxed
	bool BuyPerk(uint8_t perk);
	uint16_t Promotions(void) const { return m_promotions; }
	uint16_t PitchTicks(void) const;

	void Save(BookSave& out) const;
	void Load(const BookSave& in);
	static void Migrate(const BookSaveV1& in, BookSave& out);

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
	uint8_t FirstOwned(void) const; // the first owned unfinished book, else the first owned one
	uint8_t BookDialed(uint8_t book) const;
	bool BookDone(uint8_t book) const { return BookDialed(book) == c_books[book].entries; }
	Entry EntryStatus(uint8_t book, uint8_t entry) const { return Entry(m_status[book][entry]); }
	void EntryNumber(uint8_t book, uint8_t entry, char* out) const; // 7 digits
	void NoteNumber(char* out) const;
	bool HasAutodialer(void) const { return m_autoLevel > 0; }
	uint8_t AutoLevel(void) const { return m_autoLevel; }
	uint8_t AutoLines(void) const; // calls it runs at once: its level, at most every line but one
	uint8_t AutoActive(void) const; // calls it has going now
	int8_t AutoShownLine(void) const { return m_autoShown; } // the line on its readout, -1 none
	uint8_t Motor(void) const { return m_motor; }
	uint8_t Tray(void) const { return m_tray; }
	uint8_t QueueCapacity(void) const { return 4 + 4 * m_tray; }
	uint8_t QueueLength(void) const { return m_queueLength; }
	uint8_t QueueCursor(void) const { return m_queueCursor; }
	uint8_t AutodialDigitTicks(void) const;
	uint16_t AutodialPauseTicks(void) const;
	bool AutodialerWorking(void) const { return AutoActive() > 0; }
	uint16_t Quota(void) const { return m_quota; }
	uint32_t QuotaDone(void) const { return m_calls - m_quotaFrom; }
	uint16_t SupervisorCalls(void) const { return m_supervisorCalls; }
	// 0 none, 1 the note is in the book, 2 its number answered, 3 the scene is over
	uint8_t NoteState(void) const { return m_noteState; }
	void NoteSceneOver(void) { m_noteState = 3; }

	// test builds only (DIALTONE_START_CASH, DIALTONE_PREDIAL, DIALTONE_START_STANDING)
	void DebugCash(uint32_t amount) { Pay(amount); }
	void DebugStanding(uint32_t amount) { m_standing += amount; }
	// owns every residential book and marks every number called but the last `leave` of the last one
	void DebugPredial(uint8_t leave);
	// test builds (DIALTONE_FIRSTDONE): the first book called to the end, nothing else yours, no cash
	void DebugFirstDone(void);

  private:
	void Generate(void);
	void ResetRun(void);
	uint32_t Hash(uint32_t a, uint32_t b) const;
	int8_t FreeLine(void) const;
	void StartCall(uint8_t index);
	void EndCall(uint8_t index, CallOutcome outcome);
	void TickLine(uint8_t index);
	void TickAutodialer(void);
	void TickSupervisor(void);
	void Pay(uint32_t amount);
	uint32_t AutomationPrice(uint32_t base) const;
	void SetCorp(const char* fmt, ...);
	void CheckNote(void);
	void CheckRescue(void);

	Rand* m_rand = nullptr;
	BookListener* m_listener = nullptr;
	uint32_t m_seed = 0;

	uint16_t m_numbers[BOOK_COUNT][BOOK_MAX_ENTRIES] = {{0}};
	uint8_t m_duds[BOOK_COUNT][BOOK_MAX_ENTRIES] = {{0}};
	uint16_t m_noteNumber = 0;
	uint8_t m_status[BOOK_COUNT][BOOK_MAX_ENTRIES] = {{0}};
	uint8_t m_owned = 1;

	uint32_t m_cash = 0, m_earned = 0, m_calls = 0, m_connects = 0, m_dudCalls = 0;
	eastl::array<BookLine, BOOK_MAX_LINES> m_lines;
	uint8_t m_lineCount = 1;
	int8_t m_handLine = -1;
	int8_t m_lastHandLine = 0;

	uint8_t m_autoLevel = 0;
	uint8_t m_motor = 0, m_tray = 0;
	QueueItem m_queue[QUEUE_MAX] = {};
	uint8_t m_queueLength = 0, m_queueCursor = 0;
	uint16_t m_autoStagger = 0; // between two machine calls starting
	int8_t m_autoShown = -1;

	bool m_supRinging = false;
	uint8_t m_supResult = 0;
	uint16_t m_supTimer = 0;
	uint16_t m_supervisorCalls = 0;
	uint32_t m_simTick = 0; // ticks since the scene started, for the log
	uint16_t m_quota = 0;
	uint32_t m_quotaFrom = 0;
	char m_corpLine[40] = {0};
	uint16_t m_corpIdle = 0;

	uint8_t m_noteState = 0;

	uint32_t m_standing = 0;
	uint8_t m_perks[PERK_COUNT] = {0};
	uint8_t m_category = 0;
	uint16_t m_promotions = 0;
};
