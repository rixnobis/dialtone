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
static constexpr psyqo::Color c_ledBack = {.r = 24, .g = 6, .b = 6};
static constexpr psyqo::Color c_led = {.r = 255, .g = 48, .b = 32};
static constexpr psyqo::Color c_ledDim = {.r = 150, .g = 30, .b = 20};

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
// title, tabs, up to four items, two description rows
static constexpr int16_t SHOP_PANEL_ROWS = 8;
// the autodialer's readout: 8 characters of 6 pixels, two rows
static constexpr int16_t LED_W = 8 * KeyGlyphs::GLYPH_W + 6, LED_H = 2 * KeyGlyphs::GLYPH_H + 6;
// opening the office saves only if the last save is at least this old
static constexpr uint16_t OFFICE_SAVE_SECONDS = 30;
static constexpr uint16_t NOTE_LINE_TICKS = 170;

static constexpr const char* c_intro[OVER_ROWS] = {
	"      DIALTONE: PHONE BOOKS",
	"",
	"They hired you to make calls. You",
	"don't remember applying. Call every",
	"number in the phone book: the corp",
	"pays a little per call, more for a",
	"sale. SELECT buys lines, books, gear.",
	"",
	"D-pad + X  dial     O  undo a digit",
	"TRIANGLE  the book  SQUARE  list it",
	"L2 hang up  R2 answer/next number",
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
	m_musak.Init();
	m_musak.Start();
	g_voiceLines.Seed(seed ^ 0x2433);
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
#ifdef DIALTONE_FIRSTDONE
	if (status != SaveSlot::Status::Loaded) {
		m_sim.DebugFirstDone();
	}
#endif
#ifdef DIALTONE_PREDIAL
	// test builds: every residential book yours, every number but the last DIALTONE_PREDIAL called
	if (status != SaveSlot::Status::Loaded) {
		m_sim.DebugPredial(DIALTONE_PREDIAL);
	}
#endif
	m_shownCash = m_sim.Cash();
	m_panelBook = m_sim.FirstOwned();
	if (m_sim.Category() == 0 && (m_sim.NoteState() == 1 || m_sim.NoteState() == 2)) {
		// the note is still waiting to be called: the book opens on it
		m_panelBook = RESIDENTIAL_BOOKS - 1;
		m_panelSel = BookSim::c_books[m_panelBook].entries;
	}
	m_lastFrame = g_madnightEngine.gpu().getFrameCount();
	m_lastSaveAt = m_playSeconds;
	printf("DIALTONE: books ready\n");
}

void BookScene::teardown(TearDownReason reason) {
	g_madnightEngine.m_input.setOnEvent(nullptr);
	m_musak.Stop();
	g_voiceLines.Stop();
}

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
		bool owned = i < m_sim.Lines();
		if (owned && !m_lamps[i]) {
			eastl::fixed_string<char, 8> name;
			name.sprintf("lamp%u", i);
			m_lamps[i] = Place(name.c_str(), c_lampMeshes[0], desk::LINE_LAMPS[i]);
			m_lampMesh[i] = 0;
		} else if (!owned) {
			Remove(m_lamps[i]); // a promotion takes the lines away
		}
		bool lit = owned && (m_sim.GetLine(i).state != BookLineState::Idle || flash);
		SetMeshIfChanged(m_lamps[i], m_lampMesh[i], lit ? 1 : 0, c_lampMeshes);
	}

	// your books still to finish on the stack, finished ones on the pile behind it
	uint8_t left = 0, done = 0;
	for (uint8_t b = 0; b < BOOK_COUNT; b++) {
		if (m_sim.Owned(b)) {
			(m_sim.BookDone(b) ? done : left)++;
		}
	}
	for (uint8_t i = 0; i < BOOK_COUNT; i++) {
		bool stack = i < left, pile = i < done;
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

	// the autodialer: its own box with a readout, and a lamp on top while it has a call going
	if (m_sim.HasAutodialer() && !m_autobox) {
		m_autobox = Place("autobox", "MODELS/AUTOBOX.MB", {0, 0, 0});
		m_lampAuto = Place("lamp_auto", c_lampMeshes[0], desk::AUTOBOX_LAMP);
		m_lampAutoMesh = 0;
	} else if (!m_sim.HasAutodialer()) {
		Remove(m_autobox);
		Remove(m_lampAuto);
	}
	SetMeshIfChanged(m_lampAuto, m_lampAutoMesh, m_sim.AutodialerWorking() && (m_flashTimer & 4) ? 1 : 0,
					 c_lampMeshes);
}

uint8_t BookScene::PanelCount(void) const {
	uint8_t n = BookSim::c_books[m_panelBook].entries;
	return m_panelBook == RESIDENTIAL_BOOKS - 1 && m_sim.Category() == 0 && m_sim.NoteState() >= 1 ? n + 1 : n;
}

// the items of the open category: lines, the autodialer and its parts, the books for sale
uint8_t BookScene::ShopCount(void) {
	uint8_t n = 0;
	if (m_shopCat == 0) {
		m_shopItems[n++] = 0;
	} else if (m_shopCat == 1) {
		for (uint8_t i = 1; i < BookSim::SHOP_ENTRIES; i++) {
			m_shopItems[n++] = i;
		}
	} else {
		uint8_t books[BOOK_COUNT];
		uint8_t count = m_sim.ForSaleList(books);
		for (uint8_t i = 0; i < count; i++) {
			m_shopItems[n++] = BookSim::SHOP_ENTRIES + books[i];
		}
	}
	if (n == 0) {
		m_shopSel = 0;
	} else if (m_shopSel >= n) {
		m_shopSel = n - 1;
	}
	return n;
}

// something in the category you could buy right now (the tab gets an asterisk)
bool BookScene::ShopCategoryAffordable(uint8_t cat) const {
	uint8_t books[BOOK_COUNT];
	if (cat == 2) {
		uint8_t count = m_sim.ForSaleList(books);
		for (uint8_t i = 0; i < count; i++) {
			if (m_sim.Cash() >= BookSim::c_books[books[i]].price) {
				return true;
			}
		}
		return false;
	}
	uint8_t first = cat == 0 ? 0 : 1, last = cat == 0 ? 1 : BookSim::SHOP_ENTRIES;
	for (uint8_t i = first; i < last; i++) {
		uint32_t price = m_sim.ShopPrice(i);
		if (price && !(i >= 2 && !m_sim.HasAutodialer()) && !m_sim.ShopLocked(i) && !m_sim.ShopNeedsLine(i) &&
			m_sim.Cash() >= price) {
			return true;
		}
	}
	return false;
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
	uint8_t count = ShopCount();
	switch (button) {
		case Button::L1:
		case Button::R1:
			m_shopCat = (m_shopCat + (button == Button::L1 ? SHOP_CATS - 1 : 1)) % SHOP_CATS;
			m_shopSel = 0;
			break;
		case Button::Up:
			if (count) {
				m_shopSel = (m_shopSel + count - 1) % count;
			}
			break;
		case Button::Down:
			if (count) {
				m_shopSel = (m_shopSel + 1) % count;
			}
			break;
		case Button::Cross:
		case Button::Square: {
			if (!count) {
				break;
			}
			uint8_t item = m_shopItems[m_shopSel];
			if (item >= BookSim::SHOP_ENTRIES) {
				uint8_t book = item - BookSim::SHOP_ENTRIES;
				if (m_sim.BuyBook(book)) {
					Toast(180, "%s is yours: TRIANGLE", BookSim::c_books[book].name);
					// a new book goes straight to the panel when the one there is finished
					if (m_sim.BookDone(m_panelBook)) {
						m_panelBook = book;
						m_panelSel = 0;
					}
				}
				break;
			}
			uint8_t lines = m_sim.Lines();
			bool hadAuto = m_sim.HasAutodialer();
			if (!m_sim.Buy(item)) {
				break;
			}
			if (item == 0) {
				Toast(180, "Line %d installed", lines + 1);
			} else if (item == 1 && !hadAuto) {
				Toast(180, "Autodialer! TRIANGLE, SQUARE: list");
			} else if (item == 1) {
				Toast(180, "Autodialer runs %d at once now", m_sim.AutoLines());
			} else if (item == 2) {
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

void BookScene::HandleMenuButton(psyqo::AdvancedPad::Button button) {
	using Button = psyqo::AdvancedPad::Button;
	if (button == Button::Start || button == Button::Circle) {
		m_menu = false;
		ClearOverlay();
	} else if (button == Button::Triangle) {
		m_musak.SetEnabled(!m_musak.Enabled());
		printf("DIALTONE: musak %s\n", m_musak.Enabled() ? "on" : "off");
	} else if (button == Button::Cross && m_sim.PromotionReady()) {
		m_menu = false;
		m_promo = 1;
		ClearOverlay();
	} else if (button == Button::Cross || button == Button::Square) {
		m_menu = false;
		m_intro = true;
		ClearOverlay();
	}
}

void BookScene::AfterPromotion(void) {
	// a new desk: the lines, the autodialer and the books are gone, whatever was ringing stops
	StopTones();
	m_panelBook = m_sim.FirstOwned();
	m_panelSel = 0;
	m_panel = m_shop = false;
	m_shopSel = 0;
	m_shopCat = 0;
	m_shownCash = m_sim.Cash();
	m_bannerTimer = 0;
	ClearPanel();
}

void BookScene::HandlePromoButton(psyqo::AdvancedPad::Button button) {
	using Button = psyqo::AdvancedPad::Button;
	if (m_promo == 1) {
		if (button == Button::Cross) {
			m_sim.Promote();
			AfterPromotion();
			m_promo = 2;
			m_promoSel = 0;
			RequestSave(); // paused here: a safe moment for the card
			ClearOverlay();
		} else if (button == Button::Circle || button == Button::Start) {
			m_promo = 0;
			m_menu = true;
			ClearOverlay();
		}
		return;
	}
	switch (button) {
		case Button::Up:
			m_promoSel = (m_promoSel + PERK_COUNT) % (PERK_COUNT + 1);
			break;
		case Button::Down:
			m_promoSel = (m_promoSel + 1) % (PERK_COUNT + 1);
			break;
		case Button::Cross:
		case Button::Square:
			if (m_promoSel < PERK_COUNT) {
				m_sim.BuyPerk(m_promoSel);
			} else if (button == Button::Cross) {
				m_promo = 0;
				ClearOverlay();
				RequestSave();
				Toast(300, "%s is yours. Start dialing.", BookSim::c_books[m_panelBook].name);
				printf("DIALTONE: started in %s\n", BookSim::c_categories[m_sim.Category()]);
			}
			break;
		case Button::Start:
			m_promoSel = PERK_COUNT;
			break;
		default:
			break;
	}
}

void BookScene::MovePanel(int8_t step) {
	// past either end of a book, the panel turns to your next (or previous) book
	uint8_t n = PanelCount();
	int16_t sel = int16_t(m_panelSel) + step;
	if (sel >= 0 && sel < n) {
		m_panelSel = sel;
		return;
	}
	uint8_t next = m_sim.NextOwned(m_panelBook, step);
	if (next == m_panelBook) {
		m_panelSel = sel < 0 ? n - 1 : 0;
		return;
	}
	m_panelBook = next;
	m_panelSel = sel < 0 ? PanelCount() - 1 : 0;
	ClearPanel();
}

void BookScene::JumpToUncalled(void) {
	uint8_t book = m_panelBook, entry = m_panelSel;
	if (entry >= BookSim::c_books[book].entries) {
		entry = 0;
	}
	// with the book shut, the number under the cursor counts too: R2 then opens the book right there
	// if nobody has called it yet; with the book open it moves on
	bool here = !m_panel && m_sim.EntryStatus(book, entry) == Entry::Unknown && !m_sim.IsQueued(book, entry);
	if (here) {
		m_panel = true;
		return;
	}
	if (!m_sim.NextUncalled(book, entry)) {
		Toast(180, m_sim.ForSaleList(m_shopItems) ? "Every number called: SELECT, a book"
												  : "Every number called");
		return;
	}
	if (book != m_panelBook) {
		ClearPanel();
	}
	m_panelBook = book;
	m_panelSel = entry;
	m_panel = true;
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
			StopTones();
			m_musak.Start();
			m_sim.NoteSceneOver();
			ClearOverlay();
			RequestSave();
			Toast(300, "The office (START): a promotion.");
			printf("DIALTONE: to be continued dismissed\n");
		}
		return;
	}
	if (m_promo) {
		HandlePromoButton(button);
		return;
	}
	if (m_menu) {
		HandleMenuButton(button);
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
			// the corp first; otherwise the book opens at the next number nobody has called
			if (m_sim.AnswerSupervisor()) {
				m_tones.Line(1, Tones::Cue::None);
				g_voiceLines.Supervisor(VoiceLines::Kind(VoiceLines::SUP_WELCOME + m_sim.SupervisorResult()));
				RequestSave();
			} else {
				JumpToUncalled();
			}
			break;
		case Button::Triangle:
			m_panel = !m_panel;
			ClearPanel();
			break;
		case Button::L1:
		case Button::R1:
			if (m_panel) {
				MovePanel(button == Button::L1 ? -1 : 1);
			}
			break;
		case Button::Square:
			if (m_panel) {
				if (m_panelSel >= BookSim::c_books[m_panelBook].entries) {
					Toast(180, "It can't read handwriting.");
				} else if (!m_sim.HasAutodialer()) {
					Toast(180, "No autodialer yet: SELECT, shop");
				} else if (m_sim.BookDone(m_panelBook)) {
					Toast(180, "That book is finished.");
				} else {
					bool was = m_sim.IsQueued(m_panelBook, m_panelSel);
					if (!m_sim.ToggleQueued(m_panelBook, m_panelSel)) {
						Toast(180, "List full (%d): hold SQUARE to clear", m_sim.QueueCapacity());
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

// the tones follow your own line and one of the autodialer's calls, the one on its readout; the other
// calls it has going are only lamps
bool BookScene::Audible(uint8_t line) const {
	return !m_sim.GetLine(line).byMachine || int8_t(line) == m_sim.AutoShownLine();
}

void BookScene::OnDigit(uint8_t line, char digit) {
	if (!Audible(line)) {
		return;
	}
	uint8_t slot = m_sim.GetLine(line).byMachine ? 1 : 0;
	if (m_toneStop[slot]) {
		m_tones.Line(slot, Tones::Cue::None);
		m_toneStop[slot] = 0;
	}
	m_tones.Digit(digit);
	// the keypad lights for your own presses only; the autodialer has its readout
	if (slot == 0) {
		m_lastKey = digit;
		m_keyFlash = 6;
	}
}

void BookScene::OnRing(uint8_t line) {
	if (!Audible(line)) {
		return;
	}
	uint8_t slot = m_sim.GetLine(line).byMachine ? 1 : 0;
	if (slot == 1 && m_sim.SupervisorRinging()) {
		return;
	}
	m_toneStop[slot] = 0;
	m_tones.Line(slot, Tones::Cue::RingbackUS);
	// a call that will pick up: its lines are read off the CD while it rings
	const auto& call = m_sim.GetLine(line);
	if (call.pending == CallOutcome::Sold || call.pending == CallOutcome::NoSale ||
		call.pending == CallOutcome::HungUp) {
		VoiceLines::Kind closing = call.pending == CallOutcome::Sold	 ? VoiceLines::SALE
								   : call.pending == CallOutcome::NoSale ? VoiceLines::NOSALE
																		 : VoiceLines::HANGUP;
		bool again = m_sim.EntryStatus(call.book, call.entry) != Entry::Unknown;
		g_voiceLines.Ring(slot, line, uint16_t(call.book) << 8 | call.entry,
						  VoiceLines::Tier(BookSim::c_books[call.book].category), closing, again);
	}
}

bool BookScene::HoldPickUp(uint8_t line) { return Audible(line) && g_voiceLines.HoldPickUp(line); }

void BookScene::OnDead(uint8_t line) {
	if (!Audible(line)) {
		return;
	}
	uint8_t slot = m_sim.GetLine(line).byMachine ? 1 : 0;
	if (slot == 1 && m_sim.SupervisorRinging()) {
		return;
	}
	m_toneStop[slot] = 0;
	m_tones.Line(slot, Tones::Cue::Busy);
}

void BookScene::OnHangUp(uint8_t line) {
	if (!Audible(line)) {
		return;
	}
	const auto& call = m_sim.GetLine(line);
	if (call.state == BookLineState::Talking) {
		g_voiceLines.PickUp(line);
	} else if (call.outcome == CallOutcome::NoSale) {
		g_voiceLines.CallEnded(line);
	}
	uint8_t slot = m_sim.GetLine(line).byMachine ? 1 : 0;
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
	if (Audible(line)) {
		g_voiceLines.CallEnded(line);
	}
	const auto& l = m_sim.GetLine(line);
	char top[40], bottom[40];
	snprintf(bottom, sizeof(bottom), "        they bought it: +$%d", l.paid);
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
	// the panel moves on to a book of yours still to finish, if there is one
	uint8_t next = m_sim.FirstOwned();
	uint8_t forSale = m_sim.ForSaleList(m_shopItems);
	if (!m_sim.BookDone(next)) {
		snprintf(bottom, sizeof(bottom), "   Next: %s", BookSim::c_books[next].name);
		if (book == m_panelBook) {
			m_panelBook = next;
			m_panelSel = 0;
			ClearPanel();
		}
	} else if (forSale) {
		snprintf(bottom, sizeof(bottom), "   Buy the next book: SELECT");
	} else if (m_sim.Category() == 0) {
		snprintf(bottom, sizeof(bottom), "   That was the last residential book");
	} else {
		snprintf(bottom, sizeof(bottom), "   Promotion: START, the office");
	}
	Banner(240, c_green, top, bottom);
	m_tones.Win();
	RequestSave();
}

void BookScene::OnBookGiven(uint8_t book) {
	char top[40];
	snprintf(top, sizeof(top), "   The corp sends you %s", BookSim::c_books[book].name);
	Banner(240, c_amber, top, "   On the house. Start dialing.");
	m_tones.Win();
	if (m_sim.BookDone(m_panelBook)) {
		m_panelBook = book;
		m_panelSel = 0;
		ClearPanel();
	}
	RequestSave();
}

void BookScene::OnNote(void) {
	char number[8], text[10], top[40];
	m_sim.NoteNumber(number);
	FormatNumber(text, number, BOOK_NUMBER_LENGTH);
	snprintf(top, sizeof(top), " A sticky note in the book: %s", text);
	Banner(600, c_yellow, top, " \"Saw your numbers. I want out too.\"");
	// the book opens on the note once you look in it
	m_panelBook = RESIDENTIAL_BOOKS - 1;
	m_panelSel = BookSim::c_books[m_panelBook].entries;
	m_panel = false;
	ClearPanel();
	RequestSave();
}

void BookScene::StopTones(void) {
	m_tones.Line(0, Tones::Cue::None);
	m_tones.Line(1, Tones::Cue::None);
	m_toneStop[0] = m_toneStop[1] = 0;
}

void BookScene::OnNoteAnswered(void) {
	StopTones();
	// the note's call has the room to itself: no musak, no callers
	m_musak.Stop();
	g_voiceLines.Stop();
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

void BookScene::DrawBand(int16_t x, int16_t y, int16_t w, int16_t h, psyqo::Color colour, bool semiTrans) {
	auto& gpu = g_madnightEngine.gpu();
	auto& fragment = m_bands[gpu.getParity()][m_bandCount++];
	fragment.primitive.position = {{.x = x, .y = y}};
	fragment.primitive.size = {{.x = w, .y = h}};
	fragment.primitive.setColor(colour);
	if (semiTrans) {
		fragment.primitive.setSemiTrans();
	} else {
		fragment.primitive.setOpaque();
	}
	gpu.chain(fragment);
}

void BookScene::UpdateHandRow(void) {
	int8_t index = m_sim.HandLine() >= 0 ? m_sim.HandLine() : m_sim.LastHandLine();
	const auto& line = m_sim.GetLine(index);
	bool blink = m_flashTimer & 16;
	char number[12];
	bool blank = line.state == BookLineState::Idle && (line.outcome == CallOutcome::None);
	FormatNumber(number, line.digits, blank ? 0 : (line.byMachine ? BOOK_NUMBER_LENGTH : line.digitCount));
	bool machine = m_sim.HandLine() < 0 && line.byMachine;
	if (machine) {
		// the hand row is yours: what the machine does or did on that line is not
		FormatNumber(number, line.digits, 0);
		blank = true;
	}

	switch (machine ? BookLineState::Idle : line.state) {
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
			// a sale and a waste of time sound the same until the end
			if (line.pending == CallOutcome::Sold || line.pending == CallOutcome::NoSale) {
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
		case CallOutcome::NoSale:
			SetRow(ROW_HAND, c_grey, "%s  \"No thanks.\"  +$%d", number, line.paid);
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
		static constexpr const char* words[] = {"", "dud", "n/a", "hung", "SALE", "no sale"};
		static constexpr psyqo::Color colours[] = {c_white, c_red, c_grey, c_grey, c_green, c_grey};
		Entry status = m_sim.EntryStatus(m_panelBook, i);
		SetRow(row, i == m_panelSel ? c_amber : colours[uint8_t(status)], "%c%s%c %s", cursor, number, queued,
			   words[uint8_t(status)]);
	}
	uint8_t pages = (count + PANEL_ENTRIES - 1) / PANEL_ENTRIES;
	uint8_t help = ROW_PANEL + PANEL_ROWS - 1;
	if (m_panelSel >= book.entries) {
		SetRow(help, c_yellow, "\"I want out too.\"");
		return;
	}
	// the panel is 19 columns: the key reminders take turns
	uint8_t turn = (m_flashTimer / 120) % (m_sim.HasAutodialer() ? 3 : 2);
	if (turn == 0) {
		SetRow(help, c_grey, "L1/R1 pg %d/%d R2 new", first / PANEL_ENTRIES + 1, pages);
	} else if (turn == 1) {
		SetRow(help, c_grey, "R2: next uncalled");
	} else {
		SetRow(help, c_grey, "SQ list, hold:clear");
	}
}

void BookScene::UpdateShop(void) {
	uint8_t count = ShopCount();
	uint8_t r = ROW_OVER + (SHOP_FIRST_Y - 12) / 16;
	static const char* const c_catNames[SHOP_CATS] = {"LINES", "AUTODIALER", "BOOKS"};
	SetRow(r++, c_amber, "SUPPLIES: %-10s $%d", c_catNames[m_shopCat], m_sim.Cash());
	{
		char tabs[48];
		int len = snprintf(tabs, sizeof(tabs), "L1");
		for (uint8_t c = 0; c < SHOP_CATS; c++) {
			len += snprintf(tabs + len, sizeof(tabs) - len, c == m_shopCat ? " [%s%s]" : " %s%s", 
							c == 0 ? "Lines" : c == 1 ? "Dialer" : "Books", ShopCategoryAffordable(c) ? "*" : "");
		}
		snprintf(tabs + len, sizeof(tabs) - len, " R1");
		SetRow(r++, c_white, "%s", tabs);
	}
	for (uint8_t s = 0; s < count; s++) {
		char cursor = m_shopSel == s ? '>' : ' ';
		uint8_t i = m_shopItems[s];
		if (i >= BookSim::SHOP_ENTRIES) {
			const auto& book = BookSim::c_books[i - BookSim::SHOP_ENTRIES];
			bool afford = m_sim.Cash() >= book.price;
			SetRow(r++, afford ? (m_shopSel == s ? c_amber : c_yellow) : c_grey, "%cBook: %-11s $%d", cursor,
				   book.name, book.price);
			continue;
		}
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
					snprintf(name, sizeof(name), "Autodialer lv %d", m_sim.AutoLevel() + (price ? 1 : 0));
				}
				break;
			case 2:
				snprintf(name, sizeof(name), "Faster motor %d", m_sim.Motor() + (price ? 1 : 0));
				break;
			default:
				snprintf(name, sizeof(name), "Bigger tray %d", m_sim.Tray() + (price ? 1 : 0));
				break;
		}
		if (i >= 2 && !m_sim.HasAutodialer()) {
			SetRow(r++, c_grey, "%c%-17s needs autodialer", cursor, name);
		} else if (price == 0) {
			SetRow(r++, c_grey, "%c%-17s all bought", cursor, name);
		} else if (m_sim.ShopNeedsLine(i)) {
			SetRow(r++, c_grey, "%c%-17s needs a 2nd line", cursor, name);
		} else if (m_sim.ShopLocked(i)) {
			SetRow(r++, c_grey, "%c%-17s after promotion", cursor, name);
		} else {
			bool afford = m_sim.Cash() >= price;
			SetRow(r++, afford ? (m_shopSel == s ? c_amber : c_white) : c_grey, "%c%-17s $%d", cursor, name, price);
		}
	}
	// what the selected thing does
	if (!count) {
		SetRow(r++, c_grey, " No books for sale yet.");
		SetRow(r++, c_grey, " Finish a book to see more.");
		for (; r < ROW_OVER + OVER_ROWS; r++) {
			SetRow(r, c_white, "");
		}
		return;
	}
	uint8_t i = m_shopItems[m_shopSel];
	if (i >= BookSim::SHOP_ENTRIES) {
		uint8_t b = i - BookSim::SHOP_ENTRIES;
		const auto& book = BookSim::c_books[b];
		SetRow(r++, c_green, " %d numbers", book.entries);
		SetRow(r++, c_green, " up to %d%% duds", book.dudPercent);
		for (; r < ROW_OVER + OVER_ROWS; r++) {
			SetRow(r, c_white, "");
		}
		return;
	}
	switch (i) {
		case 0:
			SetRow(r++, c_green, "%d line%s now. One stays yours", m_sim.Lines(), m_sim.Lines() > 1 ? "s" : "");
			break;
		case 1:
			if (m_sim.ShopNeedsLine(1)) {
				SetRow(r++, c_green, "Dials on a line of its own: buy");
				SetRow(r++, c_green, "a 2nd line first.");
			} else if (!m_sim.HasAutodialer()) {
				SetRow(r++, c_green, "Dials a list you give it. Badly.");
			} else if (m_sim.AutoLines() < m_sim.AutoLevel()) {
				SetRow(r++, c_green, "Lv %d runs %d at once: needs lines", m_sim.AutoLevel(), m_sim.AutoLines());
			} else {
				SetRow(r++, c_green, "Runs %d call%s at once, a line each", m_sim.AutoLines(),
					   m_sim.AutoLines() > 1 ? "s" : "");
			}
			break;
		case 2:
			SetRow(r++, c_green, "Digit every %d ticks, rests %ds", m_sim.AutodialDigitTicks(),
				   m_sim.AutodialPauseTicks() / 60);
			break;
		default:
			SetRow(r++, c_green, "Its list holds %d numbers", m_sim.QueueCapacity());
			break;
	}
	for (; r < ROW_OVER + OVER_ROWS; r++) {
		SetRow(r, c_white, "");
	}
}

void BookScene::UpdateMenu(void) {
	uint8_t r = ROW_OVER;
	uint8_t category = m_sim.Category();
	SetRow(r++, c_amber, "          THE OFFICE");
	SetRow(r++, c_white, "Calls %-5d sales %-4d duds %d", m_sim.Calls(), m_sim.Connects(), m_sim.DudCalls());
	SetRow(r++, c_white, "Earned $%-7d played %dh%02dm", m_sim.Earned(), m_playSeconds / 3600, m_playSeconds / 60 % 60);
	SetRow(r++, c_green, "%-19s %d/%d books", BookSim::c_categories[category], m_sim.BooksDoneIn(category),
		   m_sim.BooksIn(category));
	// the category's books: yours, for sale, or locked until enough of the others are done
	uint8_t shown = 0;
	for (uint8_t b = 0; b < BOOK_COUNT && shown < 4; b++) {
		const auto& book = BookSim::c_books[b];
		if (book.category != category) {
			continue;
		}
		shown++;
		if (m_sim.Owned(b)) {
			SetRow(r++, c_white, "  %-15s %s%2d/%d", book.name, m_sim.BookDone(b) ? "done " : "     ",
				   m_sim.BookDialed(b), book.entries);
		} else if (m_sim.ForSale(b)) {
			SetRow(r++, c_yellow, "  %-15s for sale $%d", book.name, book.price);
		} else {
			SetRow(r++, c_grey, "  %-15s after %d done", book.name, book.unlockAfter);
		}
	}
	if (shown < 4) {
		SetRow(r++, c_grey, "  more books here: later");
		shown++;
	}
	for (; shown < 4; shown++) {
		SetRow(r++, c_white, "");
	}
	if (category == 0) {
		SetRow(r++, c_grey, "SMALL BUSINESS      by promotion");
	} else {
		SetRow(r++, c_grey, "RESIDENTIAL         done");
	}
	SetRow(r++, c_grey, "LARGE BUSINESS, CEOS   later");
	if (m_sim.Promotions() || m_sim.Standing()) {
		SetRow(r++, c_white, "Standing %-4d  promoted %d time%s", m_sim.Standing(), m_sim.Promotions(),
			   m_sim.Promotions() == 1 ? "" : "s");
	} else {
		SetRow(r++, c_white, "");
	}
	if (m_save.WritingFile()) {
		SetRow(r++, c_amber, "SAVING to card 1 - hands off");
	} else if (m_save.CardOk()) {
		SetRow(r++, c_grey, "Card 1: saves at pauses  TRI musak %s", m_musak.Enabled() ? "on" : "off");
	} else {
		SetRow(r++, c_grey, "No card: not saving  TRI musak %s", m_musak.Enabled() ? "on" : "off");
	}
	if (m_sim.PromotionReady()) {
		SetRow(r++, (m_flashTimer & 32) ? c_amber : c_yellow, "X PROMOTION   SQ how to play");
	} else {
		SetRow(r++, c_grey, "X how to play       START back");
	}
}

void BookScene::UpdatePromo(void) {
	uint8_t r = ROW_OVER;
	if (m_promo == 1) {
		bool again = m_sim.Category() + 1 >= CATEGORY_COUNT;
		SetRow(r++, c_amber, "           PROMOTION");
		SetRow(r++, c_white, "");
		SetRow(r++, c_white, again ? "Small business again, a new desk." : "Small business wants you upstairs.");
		SetRow(r++, c_white, "");
		SetRow(r++, c_white, "This run: %d calls, %d sales", m_sim.Calls(), m_sim.Connects());
		SetRow(r++, c_green, "Standing: +%d", m_sim.PromotionStanding());
		SetRow(r++, c_white, "");
		SetRow(r++, c_white, "You keep: standing and perks.");
		SetRow(r++, c_white, "You lose: cash, books, lines and");
		SetRow(r++, c_white, "the autodialer. Perks come next.");
		SetRow(r++, c_white, "");
		SetRow(r++, c_white, "");
		SetRow(r++, (m_flashTimer & 32) ? c_amber : c_yellow, "X accept              O not yet");
		return;
	}
	SetRow(r++, c_amber, "  PERKS          standing %d", m_sim.Standing());
	SetRow(r++, c_white, "");
	for (uint8_t p = 0; p < PERK_COUNT; p++) {
		char cursor = m_promoSel == p ? '>' : ' ';
		uint32_t price = m_sim.PerkPrice(p);
		if (price == 0) {
			SetRow(r++, c_grey, "%c%-12s lv %d   maxed", cursor, BookSim::c_perkNames[p], m_sim.PerkLevel(p));
		} else {
			bool afford = m_sim.Standing() >= price;
			SetRow(r++, afford ? (m_promoSel == p ? c_amber : c_white) : c_grey, "%c%-12s lv %d   %d standing",
				   cursor, BookSim::c_perkNames[p], m_sim.PerkLevel(p), price);
		}
	}
	SetRow(r++, c_white, "");
	switch (m_promoSel) {
		case BookSim::PERK_PITCH:
			SetRow(r++, c_green, "A sale's pitch: 12%% shorter a level");
			SetRow(r++, c_white, "Now %d.%d s", m_sim.PitchTicks() / 60, m_sim.PitchTicks() % 60 / 6);
			break;
		case BookSim::PERK_PICKUP:
			SetRow(r++, c_green, "Pick-up odds: +4 points a level");
			SetRow(r++, c_white, "Now +%d", 4 * m_sim.PerkLevel(BookSim::PERK_PICKUP));
			break;
		case BookSim::PERK_AUTOMATION:
			SetRow(r++, c_green, "Autodialer, motor, tray: 10%% off");
			SetRow(r++, c_white, "a level. Now %d%% off", 10 * m_sim.PerkLevel(BookSim::PERK_AUTOMATION));
			break;
		default:
			SetRow(r++, c_green, "Unspent standing keeps.");
			SetRow(r++, c_white, "");
			break;
	}
	SetRow(r++, c_white, "");
	SetRow(r++, m_promoSel == PERK_COUNT ? c_amber : c_white, "%cStart: %s", m_promoSel == PERK_COUNT ? '>' : ' ',
		   BookSim::c_books[m_panelBook].name);
	SetRow(r++, c_white, "");
	if (m_save.WritingFile()) {
		SetRow(r++, c_amber, "SAVING to card 1 - hands off");
	} else {
		SetRow(r++, c_grey, "Up/Down, X buy or start");
	}
	for (; r < ROW_OVER + OVER_ROWS; r++) {
		SetRow(r, c_white, "");
	}
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
	const auto& book = BookSim::c_books[m_panelBook];
	SetRow(ROW_TOP, c_amber, "$%-9d %13s %2d/%d", m_shownCash, book.name, m_sim.BookDialed(m_panelBook),
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

void BookScene::DrawReadout(void) {
	// the autodialer's own display, like an alarm clock's: the number its shown call is on, then how
	// many calls it has going out of how many it may run, and how long its list is
	if (!m_sim.HasAutodialer()) {
		return;
	}
	auto& gpu = g_madnightEngine.gpu();
	psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::Rotation>(m_camera.rotationMatrix());
	psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::Translation>(psyqo::Vec3{0, 0, 0});
	psyqo::GTE::writeSafe<psyqo::GTE::PseudoRegister::V0>(desk::AUTOBOX_LED - m_camera.pos());
	psyqo::GTE::Kernels::rtps();
	psyqo::Vertex centre;
	psyqo::GTE::read<psyqo::GTE::Register::SXY2>(&centre.packed);
	int16_t x = centre.x - LED_W / 2, y = centre.y - LED_H / 2;
	DrawBand(x, y, LED_W, LED_H, c_ledBack, false);

	char top[12], bottom[12];
	int8_t shown = m_sim.AutoShownLine();
	const BookLine* line = shown >= 0 ? &m_sim.GetLine(shown) : nullptr;
	if (line && line->byMachine && line->state != BookLineState::Idle) {
		uint8_t o = 0;
		for (uint8_t i = 0; i < line->digitCount; i++) {
			if (i == 3) {
				top[o++] = '-';
			}
			top[o++] = line->digits[i];
		}
		top[o] = 0;
	} else {
		snprintf(top, sizeof(top), "%s", m_sim.QueueLength() ? " READY" : "NO LIST");
	}
	snprintf(bottom, sizeof(bottom), "L%d/%d Q%d", m_sim.AutoActive(), m_sim.AutoLines(), m_sim.QueueLength());
	m_glyphs.DrawText(gpu, top, {{.x = int16_t(x + 3), .y = int16_t(y + 3)}}, c_led);
	m_glyphs.DrawText(gpu, bottom, {{.x = int16_t(x + 3), .y = int16_t(y + 3 + KeyGlyphs::GLYPH_H + 1)}},
					  m_sim.AutodialerWorking() ? c_led : c_ledDim);
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
	// a book finished, the office opened, a corp call answered or missed, the note, a promotion. In play
	// it also waits for your line to be free and the shop and book panel to be shut; the office and
	// the promotion screens pause the game anyway
	bool calm = m_menu || m_promo || (m_sim.HandLine() < 0 && !m_sim.SupervisorRinging() && !m_shop && !m_panel);
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
	g_voiceLines.Tick(deltaTime);
	m_musak.Duck(g_voiceLines.Live() || m_tones.Ringing());
	m_musak.Tick(deltaTime);
	bool paused = m_intro || m_menu || m_promo;
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
		// holding Square with the book open empties the autodialer's list
		if (m_panel && !m_shop &&
			g_madnightEngine.m_input.isButtonPressed(psyqo::AdvancedPad::Pad::Pad1a, psyqo::AdvancedPad::Button::Square)) {
			if (m_squareHold < CLEAR_HOLD_TICKS && m_squareHold + deltaTime >= CLEAR_HOLD_TICKS &&
				m_sim.QueueLength()) {
				m_sim.ClearQueue();
				Toast(150, "Autodialer list cleared (0/%d)", m_sim.QueueCapacity());
				printf("DIALTONE: list cleared\n");
			}
			m_squareHold = m_squareHold + deltaTime < CLEAR_HOLD_TICKS ? m_squareHold + deltaTime : CLEAR_HOLD_TICKS;
		} else {
			m_squareHold = 0;
		}
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
		if (m_noteScene && m_noteStep == 6) {
			// the dead line's fast busy lasts 90 ticks, then the scene is silent
			m_noteTimer = m_noteTimer > deltaTime ? m_noteTimer - deltaTime : 0;
			if (m_noteTimer < NOTE_LINE_TICKS - 90) {
				m_tones.Line(0, Tones::Cue::None);
			}
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
	if (m_intro || m_menu || m_noteScene || m_promo) {
		DrawBand(0, 0, 320, 240, c_band);
		DrawBand(0, 0, 320, 240, c_band);
		if (m_intro) {
			UpdateOverlayText(c_intro, OVER_ROWS, true);
		} else if (m_menu) {
			UpdateMenu();
		} else if (m_promo) {
			UpdatePromo();
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
		DrawReadout();
	}
	DrawBand(0, 0, 320, TOP_BAND_H, c_band);
	int16_t bottom = m_toastTimer || m_save.WritingFile() ? c_rowY[ROW_TOAST] : c_rowY[ROW_HAND];
	DrawBand(0, bottom - 4, 320, 240 - (bottom - 4), c_band);
	if (m_shop) {
		int16_t h = 16 * SHOP_PANEL_ROWS + 4;
		DrawBand(0, SHOP_FIRST_Y - 2, 320, h, c_band);
		DrawBand(0, SHOP_FIRST_Y - 2, 320, h, c_band);
	} else if (m_panel) {
		DrawBand(PANEL_X - 6, PANEL_Y, 320 - PANEL_X + 6, 16 * PANEL_ROWS, c_band);
		DrawBand(PANEL_X - 6, PANEL_Y, 320 - PANEL_X + 6, 16 * PANEL_ROWS, c_band);
	} else if (m_bannerTimer) {
		DrawBand(0, BANNER_Y, 320, BANNER_H, c_band);
	}

	UpdateText();
	m_hud.Render();
}
