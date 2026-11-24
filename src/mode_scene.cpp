#include "mode_scene.hh"

#include "madnight.hh"
#include "render/renderer.hh"

#include <psyqo/xprintf.h>

static constexpr psyqo::Color c_white = {.r = 255, .g = 255, .b = 255};
static constexpr psyqo::Color c_grey = {.r = 150, .g = 150, .b = 150};
static constexpr psyqo::Color c_amber = {.r = 255, .g = 176, .b = 32};
static constexpr psyqo::Color c_green = {.r = 96, .g = 255, .b = 112};

void ModeScene::start(StartReason reason) {
	Renderer::Instance().StartScene();
	g_madnightEngine.m_input.setOnEvent([this](auto event) {
		if (event.type != psyqo::AdvancedPad::Event::ButtonPressed || event.pad != psyqo::AdvancedPad::Pad::Pad1a) {
			return;
		}
		using Button = psyqo::AdvancedPad::Button;
		switch (event.button) {
			case Button::Up:
			case Button::Down:
				m_sel ^= 1;
				break;
			case Button::Cross:
			case Button::Start:
				// switched from frame(), not from inside the pad callback
				m_chosen = m_sel;
				break;
			default:
				break;
		}
	});
	printf("DIALTONE: mode select\n");
}

void ModeScene::teardown(TearDownReason reason) { g_madnightEngine.m_input.setOnEvent(nullptr); }

void ModeScene::frame() {
	auto& renderer = Renderer::Instance();
	uint32_t deltaTime = renderer.Process();
	if (deltaTime == 0) {
		return;
	}
	if (m_chosen >= 0) {
		printf("DIALTONE: mode %s\n", m_chosen ? "books" : "radio");
		g_madnightEngine.SwitchScene(m_modes[m_chosen]);
		return;
	}
	m_blink += deltaTime;

	renderer.Clear();
	auto& gpu = renderer.GPU();
	auto* font = renderer.SystemFont();
	font->chainprintf(gpu, {{.x = 104, .y = 40}}, c_amber, "D I A L T O N E");
	static constexpr const char* names[2] = {"Radio contests", "Phone books"};
	static constexpr const char* blurbs[2][2] = {
		{"Phone in to radio contests and be", "exactly the caller the DJ wants."},
		{"Cold calls for a corporation you", "never applied to. Get out."},
	};
	for (uint8_t i = 0; i < 2; i++) {
		bool sel = m_sel == i;
		font->chainprintf(gpu, {{.x = 72, .y = int16_t(80 + 20 * i)}}, sel ? c_amber : c_grey, "%c %s", sel ? '>' : ' ',
						  names[i]);
	}
	for (uint8_t i = 0; i < 2; i++) {
		font->chainprintf(gpu, {{.x = 24, .y = int16_t(136 + 16 * i)}}, c_white, "%s", blurbs[m_sel][i]);
	}
	font->chainprintf(gpu, {{.x = 64, .y = 192}}, (m_blink & 32) ? c_green : c_grey, "UP/DOWN pick   X start");
}
