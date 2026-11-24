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

// Phone books mode: the same desk and phone, a line board and a stack of phone books where the
// radio was, the open book as a panel on the right (TRIANGLE)
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
	void OnNote(void) override;
	void OnNoteAnswered(void) override;

	static constexpr uint8_t KEYS = 12;
	static constexpr uint8_t PANEL_ENTRIES = 6;

	void CreateDesk(void);
	void UpdateDesk(void);
	void HandleButton(psyqo::AdvancedPad::Button button);
	void HandleShopButton(psyqo::AdvancedPad::Button button);
	void UpdateText(void);
	void UpdateHandRow(void);
	void UpdatePanel(void);
	void UpdateShop(void);
	void UpdateMenu(void);
	void UpdateOverlayText(const char* const* rows, uint8_t count, bool blinkLast);
	void UpdateNoteScene(void);
	void ClearOverlay(void);
	void ClearPanel(void);
	void DrawKeyDigits(void);
	void DrawBand(int16_t x, int16_t y, int16_t w, int16_t h, psyqo::Color colour);
	void TickClock(void);
	void SaveNow(void);
	void Toast(uint16_t ticks, const char* fmt, ...);
	void Banner(uint16_t ticks, psyqo::Color colour, const char* top, const char* bottom);
	void SetRow(uint8_t row, psyqo::Color colour, const char* fmt, ...);
	uint8_t PanelCount(void) const; // entries in the open book, plus the note once it is there
	uint8_t ToneSlot(uint8_t line) const;

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
	GameObject* m_autodial = nullptr;
	GameObject* m_lampAuto = nullptr;
	uint8_t m_lampAutoMesh = 0;

	psyqo::Fragments::SimpleFragment<psyqo::Prim::Rectangle> m_bands[2][6];
	uint8_t m_bandCount = 0;
	KeyGlyphs m_glyphs;

	uint8_t m_cursorX = 0, m_cursorY = 0;
	char m_lastKey = 0;
	uint8_t m_keyFlash = 0;
	uint16_t m_flashTimer = 0;

	bool m_intro = true;
	bool m_panel = false;
	uint8_t m_panelSel = 0;
	uint8_t m_panelBook = 0;
	bool m_shop = false;
	uint8_t m_shopSel = 0;
	bool m_menu = false;

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
	uint32_t m_nextSaveAt = 0;
	uint16_t m_quietTicks = 0; // since the last button press
};
