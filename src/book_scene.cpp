#include "book_scene.hh"

#include "desk_layout.hh"
#include "madnight.hh"
#include "render/lighting.hh"
#include "render/renderer.hh"

#include <psyqo/gte-kernels.hh>
#include <psyqo/gte-registers.hh>
#include <psyqo/xprintf.h>
#include <stdarg.h>

using namespace psyqo::fixed_point_literals;
using namespace psyqo::trig_literals;

static constexpr psyqo::Color c_bg = {.r = 12, .g = 10, .b = 18};
static constexpr psyqo::Color c_white = {.r = 255, .g = 255, .b = 255};
static constexpr psyqo::Color c_grey = {.r = 150, .g = 150, .b = 150};
static constexpr psyqo::Color c_amber = {.r = 255, .g = 176, .b = 32};
static constexpr psyqo::Color c_green = {.r = 96, .g = 255, .b = 112};
static constexpr psyqo::Color c_red = {.r = 255, .g = 96, .b = 80};
static constexpr psyqo::Color c_yellow = {.r = 255, .g = 240, .b = 90};
static constexpr psyqo::Color c_band = {.r = 0, .g = 0, .b = 0};
static constexpr psyqo::Color c_ink = {.r = 40, .g = 20, .b = 12};

static constexpr char c_keys[4][3] = {{'1', '2', '3'}, {'4', '5', '6'}, {'7', '8', '9'}, {'*', '0', '#'}};
static constexpr const char* c_keyMeshes[] = {"MODELS/KEY.MB", "MODELS/KEYSEL.MB", "MODELS/KEYLIT.MB"};
static constexpr const char* c_lampMeshes[] = {"MODELS/LAMP.MB", "MODELS/LAMPLIT.MB"};

// the same seat as the radio mode
static constexpr psyqo::Vec3 c_eye = {-0.3_ws, -2.0_ws, -1.3_ws};
static constexpr psyqo::Vec3 c_lookAt = {0.0_ws, -0.2_ws, 0.9_ws};
static constexpr uint16_t DESK_PROJECTION = 180;
static constexpr psyqo::FixedPoint<> HANDSET_LIFT = 0.25_ws;
static constexpr psyqo::FixedPoint<> KEY_TOP = 0.05_ws;

// four rows always: cash and the book, the corp, quota and lines, the number on your handset.
// Toasts and banners come and go; the book, the shop and the office are panels on demand.
// Everything stays within lines 12-224
enum Row : uint8_t {
	ROW_TOP,
	ROW_CORP,
	ROW_STATUS,
	ROW_HAND,
	ROW_TOAST,
	ROW_BANNER1,
	ROW_BANNER2,
	ROW_PANEL, // PANEL_ROWS on the right half: title, entries, a key reminder
	ROW_OVER = ROW_PANEL + 9, // OVER_ROWS full width: the intro, the shop, the office, the note
};
static constexpr uint8_t PANEL_ROWS = 8;
static constexpr uint8_t OVER_ROWS = 13;
static constexpr uint8_t ROW_COUNT = ROW_OVER + OVER_ROWS;
static constexpr int16_t c_rowY[ROW_PANEL] = {12, 28, 44, 208, 192, 98, 114};
// below the top band, above the toast row
static constexpr int16_t PANEL_X = 168, PANEL_Y = 64;
static constexpr int16_t TOP_BAND_H = 64, BANNER_Y = 92, BANNER_H = 42;
static constexpr int16_t SHOP_FIRST_Y = 60;
// opening the office saves only if the last save is at least this old
static constexpr uint16_t OFFICE_SAVE_SECONDS = 30;
static constexpr uint16_t NOTE_LINE_TICKS = 170;

static constexpr const char* c_intro[OVER_ROWS] = {
	"      DIALTONE: PHONE BOOKS",
	"",
	"They hired you to make calls. You",
	"don't remember applying. Call every",
	"number in the phone book: the corp",
	"pays a little per call, more if they",
	"listen. Finish a book, get the next.",
	"",
	"D-pad + X  dial     O  undo a digit",
	"TRIANGLE  the book  SQUARE  list it",
	"L2 hang up  R2 answer  SELECT shop",
	"START  the office, and this card",
	"         press X to start",
};

static void FormatNumber(char* out, const char* digits, uint8_t count) {
	uint8_t o = 0;
	for (uint8_t i = 0; i < BOOK_NUMBER_LENGTH; i++) {
		if (i == 3) {
			out[o++] = '-';
		}
		out[o++] = i < count ? digits[i] : '_';
	}
	out[o] = 0;
}

static GameObject* Place(const char* name, const char* mesh, const psyqo::Vec3& pos) {
	auto* object =
		g_madnightEngine.m_gameObjectManager.CreateGameObject(name, pos, {0, 0, 0}, GameObjectTag::ENVIRONMENT);
	if (object) {
		object->SetMesh(mesh);
	}
	return object;
}

static void SetMeshIfChanged(GameObject* object, uint8_t& current, uint8_t wanted, const char* const* meshes) {
	if (object && current != wanted) {
		object->SetMesh(meshes[wanted]);
		current = wanted;
	}
}

static void Remove(GameObject*& object) {
	if (object) {
		g_madnightEngine.m_gameObjectManager.DestroyGameObject(object);
		object = nullptr;
	}
}

void BookScene::start(StartReason reason) {
	auto& lighting = Lighting::instance();
	lighting.SetAmbientColour({.r = 128, .g = 128, .b = 128});
	Renderer::Instance().SetFogColour(c_bg);
	Renderer::Instance().StartScene();
	psyqo::GTE::write<psyqo::GTE::Register::H, psyqo::GTE::Unsafe>(DESK_PROJECTION);
	Renderer::Instance().SetActiveCamera(&m_camera);

	g_madnightEngine.m_input.setOnEvent([this](auto event) {
		if (event.type == psyqo::AdvancedPad::Event::ButtonPressed && event.pad == psyqo::AdvancedPad::Pad::Pad1a) {
			HandleButton(event.button);
		}
	});

	if (reason != StartReason::Create) {
		return;
	}

	m_camera.SetPosition(c_eye);
	m_camera.LookAt(&c_lookAt);

	for (uint8_t i = 0; i < ROW_COUNT; i++) {
		int16_t x = 8, y, w = 320;
		if (i < ROW_PANEL) {
			y = c_rowY[i];
		} else if (i < ROW_OVER) {
			x = PANEL_X, w = 320 - PANEL_X, y = PANEL_Y + 16 * (i - ROW_PANEL);
		} else {
			y = 12 + 16 * (i - ROW_OVER);
		}
		m_rows[i] = m_hud.AddTextHUDElement(TextHUDElement("row", {.pos = {{.x = x, .y = y}}, .size = {w, 16}}));
		m_rows[i]->SetDisplayText("");
	}

	CreateDesk();
	m_glyphs.Init(g_madnightEngine.gpu());

	uint32_t seed = g_madnightEngine.gpu().now();
#ifdef DIALTONE_SEED
	seed = DIALTONE_SEED;
#endif
	g_madnightEngine.m_rand.seed(seed);
	m_tones.Init();
	m_sim.Init(&g_madnightEngine.m_rand, this, seed);

	auto status = m_save.Load(m_sim, m_playSeconds);
	switch (status) {
		case SaveSlot::Status::Loaded:
			Toast(240, "Welcome back. The book is open.");
			break;
		case SaveSlot::Status::Fresh:
			Toast(240, "New game, saving to card 1");
			break;
		case SaveSlot::Status::Corrupt:
			Toast(240, "Save damaged: starting over");
			break;
		case SaveSlot::Status::Unformatted:
			Toast(240, "Card 1 unformatted: not saving");
			break;
		default:
			Toast(240, "No memory card: not saving");
			break;
	}
#ifdef DIALTONE_START_CASH
	if (status != SaveSlot::Status::Loaded) {
		m_sim.DebugCash(DIALTONE_START_CASH);
	}
#endif
#ifdef DIALTONE_PREDIAL
	// test builds: every number but the last DIALTONE_PREDIAL of the last book already called
	if (status != SaveSlot::Status::Loaded) {
		m_sim.DebugPredial(DIALTONE_PREDIAL);
	}
#endif
	m_shownCash = m_sim.Cash();
	m_panelBook = m_sim.OpenBook();
	m_lastFrame = g_madnightEngine.gpu().getFrameCount();
	m_lastSaveAt = m_playSeconds;
	printf("DIALTONE: books ready\n");
}

void BookScene::teardown(TearDownReason reason) { g_madnightEngine.m_input.setOnEvent(nullptr); }

void BookScene::CreateDesk(void) {
	Place("desk", "MODELS/DESK.MB", {0, 0, 0});
	Place("phone", "MODELS/PHONE.MB", {0, 0, 0});
	Place("lineboard", "MODELS/LINEBOARD.MB", {0, 0, 0});
	Place("mug", "MODELS/MUG.MB", {0, 0, 0});
	m_handset = Place("handset", "MODELS/HANDSET.MB", desk::HANDSET);
	for (uint8_t i = 0; i < KEYS; i++) {
		eastl::fixed_string<char, 8> name;
		name.sprintf("key%u", i);
		m_keys[i] = Place(name.c_str(), c_keyMeshes[0], desk::KEYS[i]);
	}
}

void BookScene::UpdateDesk(void) {
	for (uint8_t i = 0; i < KEYS; i++) {
		uint8_t row = i / 3, col = i % 3;
		uint8_t wanted = (row == m_cursorY && col == m_cursorX) ? 1 : 0;
		if (m_keyFlash && c_keys[row][col] == m_lastKey) {
			wanted = 2;
		}
		SetMeshIfChanged(m_keys[i], m_keyMesh[i], wanted, c_keyMeshes);
	}

	bool up = m_sim.HandLine() >= 0;
	if (m_handset && up != m_handsetUp) {
		psyqo::Vec3 pos = desk::HANDSET;
		if (up) {
			pos.y -= HANDSET_LIFT;
			pos.z -= HANDSET_LIFT;
		}
		m_handset->SetPosition(pos);
		m_handset->SetRotation(up ? 0.08_pi : 0.0_pi, 0.0_pi, 0.0_pi);
		m_handsetUp = up;
	}

	// a lamp per line you own, lit while it is on a call; the corp line flashes them all
	bool flash = m_sim.SupervisorRinging() && (m_flashTimer & 8);
	for (uint8_t i = 0; i < BOOK_MAX_LINES; i++) {
		if (i < m_sim.Lines() && !m_lamps[i]) {
			eastl::fixed_string<char, 8> name;
			name.sprintf("lamp%u", i);
			m_lamps[i] = Place(name.c_str(), c_lampMeshes[0], desk::LINE_LAMPS[i]);
			m_lampMesh[i] = 0;
		}
		bool lit = i < m_sim.Lines() && (m_sim.GetLine(i).state != BookLineState::Idle || flash);
		SetMeshIfChanged(m_lamps[i], m_lampMesh[i], lit ? 1 : 0, c_lampMeshes);
	}

	// books left to call on the stack, finished ones on the pile behind it
	uint8_t done = m_sim.BooksDone();
	for (uint8_t i = 0; i < BOOK_COUNT; i++) {
		bool stack = i < BOOK_COUNT - done, pile = i < done;
		if (stack && !m_stack[i]) {
			psyqo::Vec3 pos = desk::BOOK_STACK;
			pos.y -= desk::BOOK_HEIGHT * int32_t(i);
			m_stack[i] = Place("book", "MODELS/BOOK.MB", pos);
		} else if (!stack) {
			Remove(m_stack[i]);
		}
		if (pile && !m_done[i]) {
			psyqo::Vec3 pos = desk::BOOK_DONE;
			pos.y -= desk::BOOK_HEIGHT * int32_t(i);
			m_done[i] = Place("bookdone", "MODELS/BOOKDONE.MB", pos);
		} else if (!pile) {
			Remove(m_done[i]);
		}
	}

	if (m_sim.HasAutodialer() && !m_autodial) {
		m_autodial = Place("autodial", "MODELS/AUTODIAL.MB", {0, 0, 0});
		m_lampAuto = Place("lamp_auto", c_lampMeshes[0], desk::LAMP_AUTO);
	}
	SetMeshIfChanged(m_lampAuto, m_lampAutoMesh, m_sim.AutodialerWorking() && (m_flashTimer & 4) ? 1 : 0,
					 c_lampMeshes);
}

uint8_t BookScene::PanelCount(void) const {
	uint8_t n = BookSim::c_books[m_panelBook].entries;
	return m_panelBook == BOOK_COUNT - 1 && m_sim.NoteState() >= 1 ? n + 1 : n;
}

void BookScene::Toast(uint16_t ticks, const char* fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m_toast, sizeof(m_toast), fmt, ap);
	va_end(ap);
	m_toastTimer = ticks;
}

void BookScene::Banner(uint16_t ticks, psyqo::Color colour, const char* top, const char* bottom) {
	snprintf(m_banner[0], sizeof(m_banner[0]), "%s", top);
	snprintf(m_banner[1], sizeof(m_banner[1]), "%s", bottom);
	m_bannerColour = colour;
	m_bannerTimer = ticks;
}

void BookScene::SaveNow(void) {
	m_save.Save(m_sim, m_playSeconds);
	m_savePending = false;
	m_lastSaveAt = m_playSeconds;
	printf("DIALTONE: save started played=%ds\n", m_playSeconds);
}

void BookScene::ClearOverlay(void) {
	for (uint8_t i = 0; i < OVER_ROWS; i++) {
		m_rows[ROW_OVER + i]->SetDisplayText("");
	}
}

void BookScene::ClearPanel(void) {
	for (uint8_t i = 0; i < PANEL_ROWS; i++) {
		m_rows[ROW_PANEL + i]->SetDisplayText("");
	}
}

void BookScene::HandleShopButton(psyqo::AdvancedPad::Button button) {
	using Button = psyqo::AdvancedPad::Button;
	switch (button) {
		case Button::Up:
			m_shopSel = (m_shopSel + BookSim::SHOP_ENTRIES - 1) % BookSim::SHOP_ENTRIES;
			break;
		case Button::Down:
			m_shopSel = (m_shopSel + 1) % BookSim::SHOP_ENTRIES;
			break;
		case Button::Cross:
		case Button::Square: {
			uint8_t lines = m_sim.Lines();
			bool hadAuto = m_sim.HasAutodialer();
			if (!m_sim.Buy(m_shopSel)) {
				break;
			}
			if (m_shopSel == 0) {
				Toast(180, "Line %d installed", lines + 1);
			} else if (m_shopSel == 1 && !hadAuto) {
				Toast(180, "Autodialer! TRIANGLE, SQUARE: list");
			} else if (m_shopSel == 1) {
				Toast(180, "Autodialer motor %d: faster", m_sim.Motor());
			} else {
				Toast(180, "Tray holds %d numbers now", m_sim.QueueCapacity());
			}
			break;
		}
		case Button::Select:
		case Button::Circle:
			m_shop = false;
			ClearOverlay();
			break;
		default:
			break;
	}
}

void BookScene::HandleButton(psyqo::AdvancedPad::Button button) {
	using Button = psyqo::AdvancedPad::Button;
	if (m_intro) {
		if (button == Button::Cross || button == Button::Start) {
			m_intro = false;
			ClearOverlay();
			printf("DIALTONE: intro dismissed\n");
		}
		return;
	}
	if (m_noteScene) {
		if (m_noteStep >= 6 && (button == Button::Cross || button == Button::Start)) {
			m_noteScene = false;
			m_sim.NoteSceneOver();
			ClearOverlay();
			RequestSave();
			Toast(300, "Keep dialing. They are watching.");
			printf("DIALTONE: to be continued dismissed\n");
		}
		return;
	}
	if (m_menu) {
		if (button == Button::Start || button == Button::Circle) {
			m_menu = false;
			ClearOverlay();
		} else if (button == Button::Cross) {
			m_menu = false;
			m_intro = true;
			ClearOverlay();
		}
		return;
	}
	if (m_shop) {
		HandleShopButton(button);
		return;
	}
	switch (button) {
		case Button::Up:
			m_cursorY = (m_cursorY + 3) % 4;
			break;
		case Button::Down:
			m_cursorY = (m_cursorY + 1) % 4;
			break;
		case Button::Left:
			m_cursorX = (m_cursorX + 2) % 3;
			break;
		case Button::Right:
			m_cursorX = (m_cursorX + 1) % 3;
			break;
		case Button::Cross:
			m_sim.PressDigit(c_keys[m_cursorY][m_cursorX]);
			break;
		case Button::Circle:
			m_sim.DeleteDigit();
			break;
		case Button::L2:
			m_sim.HangUp();
			break;
		case Button::R2:
			if (m_sim.AnswerSupervisor()) {
				m_tones.Line(1, Tones::Cue::None);
				RequestSave();
			}
			break;
		case Button::Triangle:
			m_panel = !m_panel;
			ClearPanel();
			break;
		case Button::L1:
		case Button::R1:
			if (m_panel) {
				uint8_t n = PanelCount();
				m_panelSel = (m_panelSel + (button == Button::L1 ? n - 1 : 1)) % n;
			}
			break;
		case Button::Square:
			if (m_panel) {
				if (m_panelSel >= BookSim::c_books[m_panelBook].entries) {
					Toast(180, "It can't read handwriting.");
				} else if (!m_sim.HasAutodialer()) {
					Toast(180, "No autodialer yet: SELECT, shop");
				} else {
					bool was = m_sim.IsQueued(m_panelBook, m_panelSel);
					if (!m_sim.ToggleQueued(m_panelBook, m_panelSel)) {
						Toast(150, "Tray full: %d numbers", m_sim.QueueCapacity());
					} else {
						char number[8], text[10];
						m_sim.EntryNumber(m_panelBook, m_panelSel, number);
						FormatNumber(text, number, BOOK_NUMBER_LENGTH);
						Toast(150, "%s %s (%d/%d)", text, was ? "off the list" : "listed", m_sim.QueueLength(),
							  m_sim.QueueCapacity());
					}
				}
			}
			break;
		case Button::Select:
			m_shop = true;
			m_panel = false;
			ClearPanel();
			break;
		case Button::Start:
			m_menu = true;
			if (m_playSeconds - m_lastSaveAt >= OFFICE_SAVE_SECONDS) {
				RequestSave(); // the game is paused in the office: a safe moment for the card
			}
			m_shop = false;
			m_panel = false;
			ClearPanel();
			ClearOverlay();
			break;
		default:
			break;
	}
}

uint8_t BookScene::ToneSlot(uint8_t line) const { return m_sim.GetLine(line).byMachine ? 1 : 0; }

void BookScene::OnDigit(uint8_t line, char digit) {
	uint8_t slot = ToneSlot(line);
	if (m_toneStop[slot]) {
		m_tones.Line(slot, Tones::Cue::None);
		m_toneStop[slot] = 0;
	}
	m_tones.Digit(digit);
	m_lastKey = digit;
	m_keyFlash = 6;
}

void BookScene::OnRing(uint8_t line) {
	uint8_t slot = ToneSlot(line);
	if (slot == 1 && m_sim.SupervisorRinging()) {
		return;
	}
	m_toneStop[slot] = 0;
	m_tones.Line(slot, Tones::Cue::Ringback);
}

void BookScene::OnDead(uint8_t line) {
	uint8_t slot = ToneSlot(line);
	if (slot == 1 && m_sim.SupervisorRinging()) {
		return;
	}
	m_toneStop[slot] = 0;
	m_tones.Line(slot, Tones::Cue::Busy);
}

void BookScene::OnHangUp(uint8_t line) {
	uint8_t slot = ToneSlot(line);
	if (slot == 1 && m_sim.SupervisorRinging()) {
		return;
	}
	// a dead number keeps its fast busy for a moment after the call is over, then stops
	if (m_sim.GetLine(line).outcome != CallOutcome::NotInService) {
		m_tones.Line(slot, Tones::Cue::None);
		m_toneStop[slot] = 0;
	} else {
		m_toneStop[slot] = DUD_TONE_TICKS;
	}
}

void BookScene::OnSold(uint8_t line) {
	OnHangUp(line);
	const auto& l = m_sim.GetLine(line);
	char top[40], bottom[40];
	snprintf(bottom, sizeof(bottom), "        they listened: +$%d", l.paid);
	if (l.byMachine) {
		Toast(180, "Autodialer sold one: +$%d", l.paid);
	} else {
		snprintf(top, sizeof(top), "       *** SALE!  %s ***", BookSim::c_books[l.book].name);
		Banner(150, c_amber, top, bottom);
		m_tones.Win();
	}
	printf("DIALTONE: sold line=%d machine=%d paid=%d cash=%d calls=%d connects=%d\n", line, l.byMachine, l.paid,
		   m_sim.Cash(), m_sim.Calls(), m_sim.Connects());
}

void BookScene::OnSupervisorRing(bool ringing) {
	m_toneStop[1] = 0;
	if (!ringing) {
		RequestSave(); // answered or missed, the corp call is over
	}
	m_tones.Line(1, ringing ? Tones::Cue::Ring : Tones::Cue::None);
}

void BookScene::OnBookDone(uint8_t book) {
	char top[40], bottom[40];
	snprintf(top, sizeof(top), "   %s: every number called", BookSim::c_books[book].name);
	if (book + 1 < BOOK_COUNT) {
		snprintf(bottom, sizeof(bottom), "   Next book: %s", BookSim::c_books[book + 1].name);
	} else {
		snprintf(bottom, sizeof(bottom), "   That was the last residential book");
	}
	Banner(240, c_green, top, bottom);
	m_tones.Win();
	RequestSave();
}

void BookScene::OnNote(void) {
	char number[8], text[10], top[40];
	m_sim.NoteNumber(number);
	FormatNumber(text, number, BOOK_NUMBER_LENGTH);
	snprintf(top, sizeof(top), " A sticky note in the book: %s", text);
	Banner(600, c_yellow, top, " \"Saw your numbers. I want out too.\"");
	// the book opens on the note once you look in it
	m_panelBook = BOOK_COUNT - 1;
	m_panelSel = BookSim::c_books[m_panelBook].entries;
	m_panel = false;
	ClearPanel();
	RequestSave();
}

void BookScene::OnNoteAnswered(void) {
	m_noteScene = true;
	m_noteStep = 1;
	m_noteTimer = NOTE_LINE_TICKS;
	m_shop = m_menu = m_panel = false;
	ClearPanel();
	ClearOverlay();
}

void BookScene::SetRow(uint8_t row, psyqo::Color colour, const char* fmt, ...) {
	char text[GAMEPLAY_HUD_ELEMENT_MAX_STR_LEN];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	m_rows[row]->SetDisplayText(text);
	m_rows[row]->SetColour(colour);
}

void BookScene::DrawBand(int16_t x, int16_t y, int16_t w, int16_t h, psyqo::Color colour) {
	auto& gpu = g_madnightEngine.gpu();
	auto& fragment = m_bands[gpu.getParity()][m_bandCount++];
	fragment.primitive.position = {{.x = x, .y = y}};
	fragment.primitive.size = {{.x = w, .y = h}};
	fragment.primitive.setColor(colour);
	fragment.primitive.setSemiTrans();
	gpu.chain(fragment);
}

void BookScene::UpdateHandRow(void) {
	int8_t index = m_sim.HandLine() >= 0 ? m_sim.HandLine() : m_sim.LastHandLine();
	const auto& line = m_sim.GetLine(index);
	bool blink = m_flashTimer & 16;
	char number[12];
	bool blank = line.state == BookLineState::Idle && (line.outcome == CallOutcome::None);
	FormatNumber(number, line.digits, blank ? 0 : (line.byMachine ? BOOK_NUMBER_LENGTH : line.digitCount));
	if (m_sim.HandLine() < 0 && line.byMachine) {
		// the hand row is yours: what the machine did on that line is not
		FormatNumber(number, line.digits, 0);
		blank = true;
	}

	switch (line.state) {
		case BookLineState::Dialing:
			SetRow(ROW_HAND, c_white, "%s  %s", number, line.digitCount == BOOK_NUMBER_LENGTH ? "connecting" : "");
			return;
		case BookLineState::Ringing:
			if (line.pending == CallOutcome::NotInService) {
				SetRow(ROW_HAND, c_red, "%s  not in service", number);
			} else {
				SetRow(ROW_HAND, blink ? c_amber : c_white, "%s  ringing...", number);
			}
			return;
		case BookLineState::Talking:
			if (line.pending == CallOutcome::Sold) {
				SetRow(ROW_HAND, c_green, "%s  they're listening...", number);
			} else {
				SetRow(ROW_HAND, c_red, "%s  \"Not interested.\" *click*", number);
			}
			return;
		default:
			break;
	}
	if (blank) {
		if (m_sim.Calls() == 0) {
			SetRow(ROW_HAND, c_grey, "%s  TRIANGLE: open the book", number);
		} else {
			SetRow(ROW_HAND, c_grey, "%s", number);
		}
		return;
	}
	switch (line.outcome) {
		case CallOutcome::NotInService:
			SetRow(ROW_HAND, c_red, "%s  not in service", number);
			break;
		case CallOutcome::NoAnswer:
			SetRow(ROW_HAND, c_grey, "%s  no answer  +$%d", number, line.paid);
			break;
		case CallOutcome::HungUp:
			SetRow(ROW_HAND, c_grey, "%s  hung up  +$%d", number, line.paid);
			break;
		case CallOutcome::Sold:
			SetRow(ROW_HAND, c_amber, "%s  SALE  +$%d", number, line.paid);
			break;
		case CallOutcome::Abandoned:
			SetRow(ROW_HAND, c_grey, "%s  you hung up", number);
			break;
		case CallOutcome::AllBusy:
			SetRow(ROW_HAND, c_red, "every line is busy");
			break;
		case CallOutcome::Note:
			SetRow(ROW_HAND, c_yellow, "%s  the line is dead now", number);
			break;
		default:
			SetRow(ROW_HAND, c_grey, "%s", number);
			break;
	}
}

void BookScene::UpdatePanel(void) {
	const auto& book = BookSim::c_books[m_panelBook];
	uint8_t count = PanelCount();
	if (m_panelSel >= count) {
		m_panelSel = 0;
	}
	SetRow(ROW_PANEL, c_amber, "%-13s%2d/%d", book.name, m_sim.BookDialed(m_panelBook), book.entries);
	uint8_t first = m_panelSel / PANEL_ENTRIES * PANEL_ENTRIES;
	for (uint8_t r = 0; r < PANEL_ENTRIES; r++) {
		uint8_t i = first + r;
		uint8_t row = ROW_PANEL + 1 + r;
		if (i >= count) {
			SetRow(row, c_grey, "");
			continue;
		}
		char cursor = i == m_panelSel ? '>' : ' ';
		char digits[8], number[10];
		if (i >= book.entries) {
			m_sim.NoteNumber(digits);
			FormatNumber(number, digits, BOOK_NUMBER_LENGTH);
			SetRow(row, c_yellow, "%c%s  note", cursor, number);
			continue;
		}
		m_sim.EntryNumber(m_panelBook, i, digits);
		FormatNumber(number, digits, BOOK_NUMBER_LENGTH);
		char queued = m_sim.IsQueued(m_panelBook, i) ? '*' : ' ';
		static constexpr const char* words[] = {"", "dud", "n/a", "hung", "SALE"};
		static constexpr psyqo::Color colours[] = {c_white, c_red, c_grey, c_grey, c_green};
		Entry status = m_sim.EntryStatus(m_panelBook, i);
		SetRow(row, i == m_panelSel ? c_amber : colours[uint8_t(status)], "%c%s%c %s", cursor, number, queued,
			   words[uint8_t(status)]);
	}
	uint8_t pages = (count + PANEL_ENTRIES - 1) / PANEL_ENTRIES;
	if (m_panelSel >= book.entries) {
		SetRow(ROW_PANEL + PANEL_ROWS - 1, c_yellow, "\"I want out too.\"");
	} else if (m_sim.HasAutodialer()) {
		SetRow(ROW_PANEL + PANEL_ROWS - 1, c_grey, "L1/R1 %d/%d  SQ list", first / PANEL_ENTRIES + 1, pages);
	} else {
		SetRow(ROW_PANEL + PANEL_ROWS - 1, c_grey, "L1/R1 move  pg %d/%d", first / PANEL_ENTRIES + 1, pages);
	}
}

void BookScene::UpdateShop(void) {
	uint8_t r = ROW_OVER + (SHOP_FIRST_Y - 12) / 16;
	SetRow(r++, c_amber, "SUPPLIES  $%-10d SELECT close", m_sim.Cash());
	for (uint8_t i = 0; i < BookSim::SHOP_ENTRIES; i++) {
		char cursor = m_shopSel == i ? '>' : ' ';
		uint32_t price = m_sim.ShopPrice(i);
		char name[24];
		switch (i) {
			case 0:
				snprintf(name, sizeof(name), price ? "Line %d" : "Lines", m_sim.Lines() + 1);
				break;
			case 1:
				if (!m_sim.HasAutodialer()) {
					snprintf(name, sizeof(name), "Autodialer");
				} else {
					snprintf(name, sizeof(name), "Faster motor %d", m_sim.Motor() + (price ? 1 : 0));
				}
				break;
			default:
				snprintf(name, sizeof(name), "Bigger tray %d", m_sim.Tray() + (price ? 1 : 0));
				break;
		}
		if (i == 2 && !m_sim.HasAutodialer()) {
			SetRow(r++, c_grey, "%c%-17s needs autodialer", cursor, name);
		} else if (price == 0) {
			SetRow(r++, c_grey, "%c%-17s all bought", cursor, name);
		} else {
			bool afford = m_sim.Cash() >= price;
			SetRow(r++, afford ? (m_shopSel == i ? c_amber : c_white) : c_grey, "%c%-17s $%d", cursor, name, price);
		}
	}
	// what the selected thing does
	switch (m_shopSel) {
		case 0:
			SetRow(r++, c_green, "%d line%s now. One more to dial on", m_sim.Lines(), m_sim.Lines() > 1 ? "s" : "");
			break;
		case 1:
			if (!m_sim.HasAutodialer()) {
				SetRow(r++, c_green, "Dials a list you give it. Badly.");
			} else {
				SetRow(r++, c_green, "Digit every %d ticks, rests %ds", m_sim.AutodialDigitTicks(),
					   m_sim.AutodialPauseTicks() / 60);
			}
			break;
		default:
			SetRow(r++, c_green, "Its list holds %d numbers", m_sim.QueueCapacity());
			break;
	}
	SetRow(r++, c_grey, "Up to 10 lines. Promotions: later.");
}

void BookScene::UpdateMenu(void) {
	uint8_t r = ROW_OVER;
	SetRow(r++, c_amber, "          THE OFFICE");
	SetRow(r++, c_white, "Calls %-5d sales %-4d duds %d", m_sim.Calls(), m_sim.Connects(), m_sim.DudCalls());
	SetRow(r++, c_white, "Earned $%-7d played %dh%02dm", m_sim.Earned(), m_playSeconds / 3600, m_playSeconds / 60 % 60);
	SetRow(r++, c_green, "RESIDENTIAL         %d/%d books", m_sim.BooksDone(), BOOK_COUNT);
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		const auto& book = BookSim::c_books[b];
		bool open = b <= m_sim.OpenBook();
		SetRow(r++, open ? c_white : c_grey, "  %-15s %s%2d/%d", book.name, m_sim.BookDone(b) ? "done " : "     ",
			   open ? m_sim.BookDialed(b) : 0, book.entries);
	}
	SetRow(r++, c_grey, "SMALL BUSINESS   locked: promotion");
	SetRow(r++, c_grey, "LARGE BUSINESS   locked: promotion");
	SetRow(r++, c_grey, "CEOS             locked: promotion");
	if (m_save.WritingFile()) {
		SetRow(r++, c_amber, "SAVING to card 1 - hands off");
	} else if (m_save.CardOk()) {
		SetRow(r++, c_grey, "Card 1: saves at pauses");
	} else {
		SetRow(r++, c_grey, "No memory card in slot 1: not saving");
	}
	SetRow(r++, c_grey, "X how to play       START back");
}

void BookScene::UpdateOverlayText(const char* const* rows, uint8_t count, bool blinkLast) {
	for (uint8_t i = 0; i < count; i++) {
		bool last = i == count - 1;
		SetRow(ROW_OVER + i, i == 0 || last ? c_amber : c_white, "%s",
			   last && blinkLast && (m_flashTimer & 32) ? "" : rows[i]);
	}
}

void BookScene::UpdateNoteScene(void) {
	char calls[40];
	snprintf(calls, sizeof(calls), "  \"So you're the one. %d calls.\"", m_sim.Calls());
	const char* lines[6] = {
		"  A voice, low, close to the phone:",
		calls,
		"  \"Three floors up. They own me too.\"",
		"  \"Get yourself promoted. Small",
		"   business. I'll leave a note.\"",
		"  *click*",
	};
	uint8_t r = ROW_OVER + 1;
	for (uint8_t i = 0; i < 6; i++) {
		SetRow(r++, i == 0 ? c_grey : c_yellow, "%s", i < m_noteStep ? lines[i] : "");
	}
	if (m_noteStep >= 6) {
		SetRow(r++, c_white, "");
		SetRow(r++, c_amber, "        TO BE CONTINUED");
		SetRow(r++, c_white, "  Calls %d   sales %d   earned $%d", m_sim.Calls(), m_sim.Connects(), m_sim.Earned());
		SetRow(r++, c_white, "");
		SetRow(r++, (m_flashTimer & 32) ? c_amber : c_grey, "           press X");
	} else {
		for (; r < ROW_OVER + OVER_ROWS; r++) {
			SetRow(r, c_white, "");
		}
	}
}

void BookScene::UpdateText(void) {
	const auto& book = BookSim::c_books[m_sim.OpenBook()];
	SetRow(ROW_TOP, c_amber, "$%-9d %13s %2d/%d", m_shownCash, book.name, m_sim.BookDialed(m_sim.OpenBook()),
		   book.entries);
	if (m_sim.SupervisorRinging()) {
		SetRow(ROW_CORP, (m_flashTimer & 16) ? c_red : c_white, "CORP LINE RINGING   R2 to answer");
	} else {
		SetRow(ROW_CORP, c_green, "\"%s\"", m_sim.CorpLine());
	}

	char quota[16] = "", autod[16] = "";
	if (m_sim.Quota()) {
		snprintf(quota, sizeof(quota), "Quota %d/%d", eastl::min<uint32_t>(m_sim.QuotaDone(), m_sim.Quota()),
				 m_sim.Quota());
	}
	if (m_sim.HasAutodialer()) {
		snprintf(autod, sizeof(autod), "Auto %d/%d", m_sim.QueueLength(), m_sim.QueueCapacity());
	}
	bool met = m_sim.Quota() && m_sim.QuotaDone() >= m_sim.Quota();
	SetRow(ROW_STATUS, met ? c_green : c_white, "%-12s Lines %d/%d  %s", quota, m_sim.LinesBusy(), m_sim.Lines(),
		   autod);

	UpdateHandRow();
	if (m_save.WritingFile()) {
		SetRow(ROW_TOAST, c_red, "   SAVING to memory card - hands off");
	} else {
		SetRow(ROW_TOAST, c_amber, "%s", m_toastTimer ? m_toast : "");
	}

	bool banner = m_bannerTimer && !m_panel && !m_shop;
	SetRow(ROW_BANNER1, m_bannerColour, "%s", banner ? m_banner[0] : "");
	SetRow(ROW_BANNER2, c_white, "%s", banner ? m_banner[1] : "");

	if (m_panel) {
		UpdatePanel();
	}
	if (m_shop) {
		UpdateShop();
	}
}

void BookScene::DrawKeyDigits(void) {
	auto& gpu = g_madnightEngine.gpu();
	psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::Rotation>(m_camera.rotationMatrix());
	psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::Translation>(psyqo::Vec3{0, 0, 0});
	for (uint8_t i = 0; i < KEYS; i++) {
		psyqo::Vec3 top = desk::KEYS[i];
		top.y -= KEY_TOP;
		psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::V0>(top - m_camera.pos());
		psyqo::GTE::Kernels::rtps();
		psyqo::Vertex screen;
		psyqo::GTE::read<psyqo::GTE::Register::SXY2>(&screen.packed);
		m_glyphs.Draw(gpu, c_keys[i / 3][i % 3], screen, c_ink);
	}
}

void BookScene::TickClock(void) {
	auto& gpu = g_madnightEngine.gpu();
	uint32_t now = gpu.getFrameCount();
	m_frameAcc += now - m_lastFrame;
	m_lastFrame = now;
	uint32_t rate = gpu.getRefreshRate();
	while (m_frameAcc >= rate) {
		m_frameAcc -= rate;
		m_playSeconds++;
	}
	// the card has the pad to itself while it writes (about 3 s), so a save waits for a natural pause:
	// a book finished, the office opened, a corp call answered or missed, the note. In play it also waits
	// for your line to be free and the shop and book panel to be shut; the office pauses the game anyway
	bool calm = m_menu || (m_sim.HandLine() < 0 && !m_sim.SupervisorRinging() && !m_shop && !m_panel);
	if (m_savePending && !m_save.Writing() && !m_intro && !m_noteScene && calm) {
		SaveNow();
	}
}

void BookScene::frame() {
	auto& renderer = Renderer::Instance();
	uint32_t deltaTime = renderer.Process();
	if (deltaTime == 0) {
		return;
	}

	m_flashTimer += deltaTime;
	TickClock();
	bool paused = m_intro || m_menu;
	if (!paused) {
		if (!m_noteScene) {
			m_sim.Tick(deltaTime);
		}
		m_tones.Tick(deltaTime);
		m_keyFlash = m_keyFlash > deltaTime ? m_keyFlash - deltaTime : 0;
		for (uint8_t slot = 0; slot < 2; slot++) {
			if (m_toneStop[slot]) {
				m_toneStop[slot] = m_toneStop[slot] > deltaTime ? m_toneStop[slot] - deltaTime : 0;
				if (!m_toneStop[slot]) {
					m_tones.Line(slot, Tones::Cue::None);
				}
			}
		}
		m_toastTimer = m_toastTimer > deltaTime ? m_toastTimer - deltaTime : 0;
		m_bannerTimer = m_bannerTimer > deltaTime ? m_bannerTimer - deltaTime : 0;
		if (m_noteScene && m_noteStep < 6) {
			if (m_noteTimer > deltaTime) {
				m_noteTimer -= deltaTime;
			} else {
				m_noteStep++;
				m_noteTimer = NOTE_LINE_TICKS;
				if (m_noteStep == 6) {
					m_tones.Line(0, Tones::Cue::Busy);
					printf("DIALTONE: to be continued calls=%d earned=%d connects=%d\n", m_sim.Calls(), m_sim.Earned(),
						   m_sim.Connects());
				}
			}
		}
		if (m_noteScene && m_noteStep == 6 && m_noteTimer < NOTE_LINE_TICKS - 90) {
			m_tones.Line(0, Tones::Cue::None);
		}

		// a finished book goes on the pile and the next one opens
		if (m_panelBook != m_sim.OpenBook() && m_sim.NoteState() == 0) {
			m_panelBook = m_sim.OpenBook();
			m_panelSel = 0;
			ClearPanel();
		}

		uint32_t cash = m_sim.Cash();
		if (m_shownCash > cash) {
			m_shownCash = cash;
		} else if (m_shownCash < cash) {
			uint32_t step = (cash - m_shownCash + 15) / 16;
			m_shownCash = eastl::min(cash, m_shownCash + step * deltaTime);
		}
	}

	UpdateDesk();
	renderer.Render(deltaTime);

	m_bandCount = 0;
	if (m_intro || m_menu || m_noteScene) {
		DrawBand(0, 0, 320, 240, c_band);
		DrawBand(0, 0, 320, 240, c_band);
		if (m_intro) {
			UpdateOverlayText(c_intro, OVER_ROWS, true);
		} else if (m_menu) {
			UpdateMenu();
		} else {
			UpdateNoteScene();
		}
		for (uint8_t i = 0; i < ROW_OVER; i++) {
			m_rows[i]->SetDisplayText("");
		}
		m_hud.Render();
		return;
	}

	if (!m_shop) {
		DrawKeyDigits();
	}
	DrawBand(0, 0, 320, TOP_BAND_H, c_band);
	int16_t bottom = m_toastTimer || m_save.WritingFile() ? c_rowY[ROW_TOAST] : c_rowY[ROW_HAND];
	DrawBand(0, bottom - 4, 320, 240 - (bottom - 4), c_band);
	if (m_shop) {
		DrawBand(0, SHOP_FIRST_Y - 2, 320, 16 * 6 + 4, c_band);
		DrawBand(0, SHOP_FIRST_Y - 2, 320, 16 * 6 + 4, c_band);
	} else if (m_panel) {
		DrawBand(PANEL_X - 6, PANEL_Y, 320 - PANEL_X + 6, 16 * PANEL_ROWS, c_band);
		DrawBand(PANEL_X - 6, PANEL_Y, 320 - PANEL_X + 6, 16 * PANEL_ROWS, c_band);
	} else if (m_bannerTimer) {
		DrawBand(0, BANNER_Y, 320, BANNER_H, c_band);
	}

	UpdateText();
	m_hud.Render();
}
