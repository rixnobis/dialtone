#pragma once

#include "sim.hh"
#include "tones.hh"

#include "render/camera.hh"
#include "ui/hud/gameplay_hud.hh"

#include <psyqo/advancedpad.hh>
#include <psyqo/fragments.hh>
#include <psyqo/primitives/rectangles.hh>
#include <psyqo/scene.hh>

class GameObject;

static constexpr uint8_t KEYPAD_COLS = 3;
static constexpr uint8_t KEYPAD_ROWS = 4;
static constexpr uint8_t KEY_COUNT = KEYPAD_COLS * KEYPAD_ROWS;

// the desk: phone, radio, upgrades as objects on it, and a text overlay for the numbers
class PhoneScene final : public psyqo::Scene, public SimListener {
	void start(StartReason reason) override;
	void teardown(TearDownReason reason) override;
	void frame() override;

	// SimListener
	void OnDigit(uint8_t line, char digit) override;
	void OnRing(uint8_t line) override;
	void OnBusy(uint8_t line) override;
	void OnHangUp(uint8_t line) override;
	void OnWin(uint8_t line) override;

	void CreateDesk(void);
	void UpdateDesk(void);
	void HandleButton(psyqo::AdvancedPad::Button button);
	void UpdateText(void);
	void DrawBand(int16_t y, int16_t h);
	void SetRow(uint8_t row, psyqo::Color colour, const char* fmt, ...);
	void SetMeshIfChanged(GameObject* object, uint8_t& current, uint8_t wanted, const char* const* meshes);

	Sim m_sim;
	Tones m_tones;
	Camera m_camera;
	GameplayHUD m_hud = GameplayHUD("Phone HUD", {.pos = {0, 0}, .size = {320, 240}});
	TextHUDElement* m_rows[16] = {nullptr};

	GameObject* m_keys[KEY_COUNT] = {nullptr};
	uint8_t m_keyMesh[KEY_COUNT] = {0};
	GameObject* m_handset = nullptr;
	GameObject* m_needle = nullptr;
	GameObject* m_lampAir = nullptr;
	uint8_t m_lampAirMesh = 0;
	GameObject* m_redial = nullptr;
	GameObject* m_phone2 = nullptr;
	GameObject* m_lampLine2 = nullptr;
	uint8_t m_lampLine2Mesh = 0;
	GameObject* m_autodial = nullptr;
	GameObject* m_lampAuto = nullptr;
	uint8_t m_lampAutoMesh = 0;
	bool m_handsetUp = false;
	uint8_t m_needleStation = 0xff;

	psyqo::Fragments::SimpleFragment<psyqo::Prim::Rectangle> m_bands[2][2];
	uint8_t m_bandCount = 0;

	bool BuyShopItem(void);

	uint8_t m_cursorX = 0, m_cursorY = 0;
	uint8_t m_shopSel = 0; // 0 = the next desk item, then one entry per Stat
	char m_lastKey = 0;
	uint8_t m_keyFlash = 0;
	uint16_t m_flashTimer = 0; // blinks whatever wants attention
	uint16_t m_toastTimer = 0;
	char m_toast[40] = {0};
};
