#pragma once

#include "book_sim.hh"
#include "key_glyphs.hh"
#include "save.hh"
#include "tones.hh"

#include "render/camera.hh"
#include "ui/hud/gameplay_hud.hh"

#include <psyqo/advancedpad.hh>
#include <psyqo/fragments.hh>
#include <psyqo/primitives/rectangles.hh>
#include <psyqo/scene.hh>

class GameObject;

// Phone books mode: the same desk and phone, a line board, the phone books you own and the
// autodialer's box, the book you are working as a panel on the right (TRIANGLE)
class BookScene final : public psyqo::Scene, public BookListener {
	void start(StartReason reason) override;
	void teardown(TearDownReason reason) override;
	void frame() override;

	// BookListener
	void OnDigit(uint8_t line, char digit) override;
	void OnRing(uint8_t line) override;
	void OnDead(uint8_t line) override;
	void OnHangUp(uint8_t line) override;
	void OnSold(uint8_t line) override;
	void OnSupervisorRing(bool ringing) override;
	void OnBookDone(uint8_t book) override;
	void OnBookGiven(uint8_t book) override;
	void OnNote(void) override;
	void OnNoteAnswered(void) override;

	static constexpr uint8_t KEYS = 12;
	static constexpr uint8_t PANEL_ENTRIES = 6;
	static constexpr uint8_t SHOP_MAX = BookSim::SHOP_ENTRIES + BOOK_COUNT;

	void CreateDesk(void);
	void UpdateDesk(void);
	void HandleButton(psyqo::AdvancedPad::Button button);
	void HandleShopButton(psyqo::AdvancedPad::Button button);
	void HandleMenuButton(psyqo::AdvancedPad::Button button);
	void HandlePromoButton(psyqo::AdvancedPad::Button button);
	void MovePanel(int8_t step);
	void JumpToUncalled(void);
	void UpdateText(void);
	void UpdateHandRow(void);
	void UpdatePanel(void);
	void UpdateShop(void);
	void UpdateMenu(void);
	void UpdatePromo(void);
	void UpdateOverlayText(const char* const* rows, uint8_t count, bool blinkLast);
	void UpdateNoteScene(void);
	void ClearOverlay(void);
	void ClearPanel(void);
	void StopTones(void);
	void DrawKeyDigits(void);
	void DrawReadout(void);
	void DrawBand(int16_t x, int16_t y, int16_t w, int16_t h, psyqo::Color colour, bool semiTrans = true);
	void TickClock(void);
	void SaveNow(void);
	void RequestSave(void) { m_savePending = true; }
	void Toast(uint16_t ticks, const char* fmt, ...);
	void Banner(uint16_t ticks, psyqo::Color colour, const char* top, const char* bottom);
	void SetRow(uint8_t row, psyqo::Color colour, const char* fmt, ...);
	uint8_t PanelCount(void) const; // entries in the panel's book, plus the note once it is there
	uint8_t ShopCount(void);		// fills m_shopItems: the fixed items, then the books for sale
	bool ShopCategoryAffordable(uint8_t cat) const;
	static constexpr uint8_t SHOP_CATS = 3;
	bool Audible(uint8_t line) const; // the hand line, and the one autodialer call its readout shows
	void AfterPromotion(void);

	BookSim m_sim;
	Tones m_tones;
	Camera m_camera;
	GameplayHUD m_hud = GameplayHUD("Books HUD", {.pos = {0, 0}, .size = {320, 240}});
	TextHUDElement* m_rows[32] = {nullptr};

	GameObject* m_keys[KEYS] = {nullptr};
	uint8_t m_keyMesh[KEYS] = {0};
	GameObject* m_handset = nullptr;
	bool m_handsetUp = false;
	GameObject* m_lamps[BOOK_MAX_LINES] = {nullptr};
	uint8_t m_lampMesh[BOOK_MAX_LINES] = {0};
	GameObject* m_stack[BOOK_COUNT] = {nullptr};
	GameObject* m_done[BOOK_COUNT] = {nullptr};
	GameObject* m_autobox = nullptr;
	GameObject* m_lampAuto = nullptr;
	uint8_t m_lampAutoMesh = 0;

	psyqo::Fragments::SimpleFragment<psyqo::Prim::Rectangle> m_bands[2][8];
	uint8_t m_bandCount = 0;
	KeyGlyphs m_glyphs;

	uint8_t m_cursorX = 0, m_cursorY = 0;
	char m_lastKey = 0;
	uint8_t m_keyFlash = 0;
	uint16_t m_toneStop[2] = {0, 0}; // a dud's fast busy stops when this runs out
	static constexpr uint16_t DUD_TONE_TICKS = 90;
	static constexpr uint16_t CLEAR_HOLD_TICKS = 60;
	uint16_t m_flashTimer = 0;

	bool m_intro = true;
	bool m_panel = false;
	uint16_t m_squareHold = 0; // ticks Square has been held with the book open; a second clears the list
	uint8_t m_panelSel = 0;
	uint8_t m_panelBook = 0;
	bool m_shop = false;
	uint8_t m_shopSel = 0; // within the open category
	uint8_t m_shopCat = 0; // 0 lines, 1 autodialer, 2 books
	uint8_t m_shopItems[SHOP_MAX] = {0}; // < SHOP_ENTRIES: a fixed item, else SHOP_ENTRIES + book
	bool m_menu = false;
	uint8_t m_promo = 0; // 1 the offer, 2 the perks (the promotion has happened)
	uint8_t m_promoSel = 0;

	uint16_t m_toastTimer = 0;
	char m_toast[40] = {0};
	uint16_t m_bannerTimer = 0;
	psyqo::Color m_bannerColour;
	char m_banner[2][40] = {{0}};
	uint32_t m_shownCash = 0;

	// the note's call: lines of it appear one by one, then the "to be continued" card
	bool m_noteScene = false;
	uint8_t m_noteStep = 0;
	uint16_t m_noteTimer = 0;

	BookSaveGame m_save;
	uint32_t m_playSeconds = 0;
	uint32_t m_lastFrame = 0;
	uint32_t m_frameAcc = 0;
	bool m_savePending = false; // saves wait for a natural pause, never an idle timer
	uint32_t m_lastSaveAt = 0;	// play seconds when the last write started
};
