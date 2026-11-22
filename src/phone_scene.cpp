#include "phone_scene.hh"

#include "desk_layout.hh"
#include "madnight.hh"
#include "render/lighting.hh"
#include "render/renderer.hh"

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
static constexpr psyqo::Color c_band = {.r = 0, .g = 0, .b = 0};

static constexpr char c_keys[KEYPAD_ROWS][KEYPAD_COLS] = {
	{'1', '2', '3'}, {'4', '5', '6'}, {'7', '8', '9'}, {'*', '0', '#'}};

static constexpr const char* c_keyMeshes[] = {"MODELS/KEY.MB", "MODELS/KEYSEL.MB", "MODELS/KEYLIT.MB"};
static constexpr const char* c_lampMeshes[] = {"MODELS/LAMP.MB", "MODELS/LAMPLIT.MB"};

// seated at the desk, looking down at the phone and the radio
static constexpr psyqo::Vec3 c_eye = {0.05_ws, -1.85_ws, -1.6_ws};
static constexpr psyqo::Vec3 c_lookAt = {0.3_ws, -0.2_ws, 0.85_ws};
// the engine projects at H = 120 (about 106 degrees across); a desk wants a narrower lens.
// Its visibility check still assumes 120, which only errs towards drawing more
static constexpr uint16_t DESK_PROJECTION = 150;
static constexpr psyqo::FixedPoint<> HANDSET_LIFT = 0.45_ws;

enum TextRow : uint8_t { ROW_TITLE, ROW_STATION, ROW_DJ, ROW_ROUND, ROW_LINE1, ROW_LINE2, ROW_SHOP, ROW_HELP, ROW_COUNT };
static constexpr int16_t c_rowY[ROW_COUNT] = {4, 20, 36, 52, 178, 194, 210, 226};

static void FormatNumber(char* out, const char* digits, uint8_t count) {
	// 555-0188 with blanks for what is not dialed yet
	uint8_t o = 0;
	for (uint8_t i = 0; i < NUMBER_LENGTH; i++) {
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

void PhoneScene::start(StartReason reason) {
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
		m_rows[i] = m_hud.AddTextHUDElement(
			TextHUDElement("row", {.pos = {{.x = 8, .y = c_rowY[i]}}, .size = {320, 16}}));
	}

	CreateDesk();

	g_madnightEngine.m_rand.seed(g_madnightEngine.gpu().now());
	m_tones.Init();
	m_sim.Init(&g_madnightEngine.m_rand, this);
	printf("DIALTONE: ready\n");
}

void PhoneScene::teardown(TearDownReason reason) { g_madnightEngine.m_input.setOnEvent(nullptr); }

void PhoneScene::CreateDesk(void) {
	Place("desk", "MODELS/DESK.MB", {0, 0, 0});
	Place("phone", "MODELS/PHONE.MB", {0, 0, 0});
	Place("radio", "MODELS/RADIO.MB", {0, 0, 0});
	Place("mug", "MODELS/MUG.MB", {0, 0, 0});
	m_handset = Place("handset", "MODELS/HANDSET.MB", desk::HANDSET);
	m_needle = Place("needle", "MODELS/NEEDLE.MB", desk::NEEDLE[0]);
	m_lampAir = Place("lamp_air", c_lampMeshes[0], desk::LAMP_AIR);
	for (uint8_t i = 0; i < KEY_COUNT; i++) {
		eastl::fixed_string<char, 8> name;
		name.sprintf("key%u", i);
		m_keys[i] = Place(name.c_str(), c_keyMeshes[0], desk::KEYS[i]);
	}
}

void PhoneScene::SetMeshIfChanged(GameObject* object, uint8_t& current, uint8_t wanted, const char* const* meshes) {
	if (object && current != wanted) {
		object->SetMesh(meshes[wanted]);
		current = wanted;
	}
}

void PhoneScene::UpdateDesk(void) {
	bool blink = m_flashTimer & 16;

	// keypad: the cursor key glows, the key just dialed (by you or the machine) lights up
	for (uint8_t i = 0; i < KEY_COUNT; i++) {
		uint8_t row = i / KEYPAD_COLS, col = i % KEYPAD_COLS;
		uint8_t wanted = (row == m_cursorY && col == m_cursorX) ? 1 : 0;
		if (m_keyFlash && c_keys[row][col] == m_lastKey) {
			wanted = 2;
		}
		SetMeshIfChanged(m_keys[i], m_keyMesh[i], wanted, c_keyMeshes);
	}

	// handset comes off the cradle while line 1 is in use
	auto lineState = m_sim.GetLine(0).state;
	bool up = lineState != LineState::Idle && lineState != LineState::Locked;
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

	if (m_needle && m_needleStation != m_sim.StationIndex()) {
		m_needleStation = m_sim.StationIndex();
		m_needle->SetPosition(desk::NEEDLE[m_needleStation]);
	}

	bool onAir = m_sim.IsRoundOpen() && !(m_sim.SecondsLeft() <= 5 && blink);
	SetMeshIfChanged(m_lampAir, m_lampAirMesh, onAir ? 1 : 0, c_lampMeshes);

	// upgrades show up on the desk as you buy them
	if (m_sim.HasUpgrade(Upgrade::Redial) && !m_redial) {
		m_redial = Place("redial", "MODELS/REDIAL.MB", desk::REDIAL);
	}
	if (m_sim.HasUpgrade(Upgrade::SecondLine) && !m_phone2) {
		m_phone2 = Place("phone2", "MODELS/PHONE2.MB", {0, 0, 0});
		m_lampLine2 = Place("lamp_line2", c_lampMeshes[0], desk::LAMP_LINE2);
	}
	if (m_sim.HasUpgrade(Upgrade::Autodialer) && !m_autodial) {
		m_autodial = Place("autodial", "MODELS/AUTODIAL.MB", {0, 0, 0});
		m_lampAuto = Place("lamp_auto", c_lampMeshes[0], desk::LAMP_AUTO);
	}

	auto line2 = m_sim.GetLine(1).state;
	SetMeshIfChanged(m_lampLine2, m_lampLine2Mesh, line2 != LineState::Idle && line2 != LineState::Locked ? 1 : 0,
					 c_lampMeshes);

	bool autoBusy = false;
	for (uint8_t i = 0; i < MAX_LINES; i++) {
		const auto& line = m_sim.GetLine(i);
		autoBusy |= line.state == LineState::Dialing && line.autoDialing;
	}
	SetMeshIfChanged(m_lampAuto, m_lampAutoMesh, autoBusy && (m_flashTimer & 4) ? 1 : 0, c_lampMeshes);
}

void PhoneScene::HandleButton(psyqo::AdvancedPad::Button button) {
	using Button = psyqo::AdvancedPad::Button;
	switch (button) {
		case Button::Up:
			m_cursorY = (m_cursorY + KEYPAD_ROWS - 1) % KEYPAD_ROWS;
			break;
		case Button::Down:
			m_cursorY = (m_cursorY + 1) % KEYPAD_ROWS;
			break;
		case Button::Left:
			m_cursorX = (m_cursorX + KEYPAD_COLS - 1) % KEYPAD_COLS;
			break;
		case Button::Right:
			m_cursorX = (m_cursorX + 1) % KEYPAD_COLS;
			break;
		case Button::Cross:
			m_sim.PressDigit(c_keys[m_cursorY][m_cursorX]);
			break;
		case Button::Circle:
			m_sim.HangUp();
			break;
		case Button::Triangle:
			m_sim.Redial();
			break;
		case Button::Square: {
			int8_t next = m_sim.NextUpgrade();
			if (next >= 0 && m_sim.BuyNextUpgrade()) {
				snprintf(m_toast, sizeof(m_toast), "Bought: %s!", Sim::c_upgrades[next].name);
				printf("DIALTONE: bought %s cash=%d\n", Sim::c_upgrades[next].name, m_sim.Cash());
				m_toastTimer = 120;
			}
			break;
		}
		case Button::L1:
			m_sim.TuneStation(-1);
			break;
		case Button::R1:
			m_sim.TuneStation(1);
			break;
		default:
			break;
	}
}

void PhoneScene::OnDigit(uint8_t line, char digit) {
	m_tones.Digit(digit);
	m_lastKey = digit;
	m_keyFlash = 6;
}
void PhoneScene::OnRing(uint8_t line) { m_tones.Line(line, Tones::Cue::Ringback); }
void PhoneScene::OnBusy(uint8_t line) { m_tones.Line(line, Tones::Cue::Busy); }
void PhoneScene::OnHangUp(uint8_t line) { m_tones.Line(line, Tones::Cue::None); }
void PhoneScene::OnWin(uint8_t line) {
	m_tones.Line(line, Tones::Cue::None);
	m_tones.Win();
	printf("DIALTONE: win cash=%d wins=%d\n", m_sim.Cash(), m_sim.Wins());
}

void PhoneScene::SetRow(uint8_t row, psyqo::Color colour, const char* fmt, ...) {
	char text[GAMEPLAY_HUD_ELEMENT_MAX_STR_LEN];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(text, sizeof(text), fmt, ap);
	va_end(ap);
	m_rows[row]->SetDisplayText(text);
	m_rows[row]->SetColour(colour);
}

void PhoneScene::DrawBand(int16_t y, int16_t h) {
	auto& gpu = g_madnightEngine.gpu();
	auto& fragment = m_bands[gpu.getParity()][m_bandCount++];
	fragment.primitive.position = {{.x = 0, .y = y}};
	fragment.primitive.size = {{.x = 320, .y = h}};
	fragment.primitive.setColor(c_band);
	fragment.primitive.setSemiTrans();
	gpu.chain(fragment);
}

void PhoneScene::UpdateText(void) {
	const auto& station = m_sim.CurrentStation();
	char number[12];

	SetRow(ROW_TITLE, c_amber, "DIALTONE                     $%7d", m_sim.Cash());

	FormatNumber(number, station.number, NUMBER_LENGTH);
	SetRow(ROW_STATION, c_white, "RADIO %-12s CALL %s", station.name, number);
	SetRow(ROW_DJ, c_green, "\"%s\"", m_sim.DJLine());

	if (m_sim.IsRoundOpen()) {
		bool hurry = m_sim.SecondsLeft() <= 5 && (m_flashTimer & 16);
		SetRow(ROW_ROUND, hurry ? c_red : c_white, "Callers %2d of %-2d  prize $%-5d %2ds", m_sim.CallersSoFar(),
			   m_sim.CallerTarget(), m_sim.Prize(), m_sim.SecondsLeft());
	} else {
		SetRow(ROW_ROUND, c_grey, "Next contest in %ds...", m_sim.SecondsLeft());
	}

	bool blink = m_flashTimer & 16;
	for (uint8_t i = 0; i < MAX_LINES; i++) {
		const auto& line = m_sim.GetLine(i);
		uint8_t row = i == 0 ? ROW_LINE1 : ROW_LINE2;
		FormatNumber(number, line.digits, line.digitCount);

		switch (line.state) {
			case LineState::Locked:
				SetRow(row, c_grey, "LINE %d  not installed", i + 1);
				break;
			case LineState::Idle:
				switch (line.outcome) {
					case Outcome::Busy:
						SetRow(row, c_white, "LINE %d  was busy", i + 1);
						break;
					case Outcome::WrongNumber:
						SetRow(row, c_red, "LINE %d  wrong number", i + 1);
						break;
					case Outcome::NoAnswer:
						SetRow(row, c_white, "LINE %d  no answer", i + 1);
						break;
					case Outcome::Missed:
						SetRow(row, c_white, "LINE %d  caller #%d, not you", i + 1, line.callerNumber);
						break;
					case Outcome::Won:
						SetRow(row, c_amber, "LINE %d  won $%d!", i + 1, line.prizeWon);
						break;
					default:
						SetRow(row, c_white, "LINE %d  dial tone", i + 1);
						break;
				}
				break;
			case LineState::Dialing:
				SetRow(row, c_white, "LINE %d  %-12s %s", i + 1, line.autoDialing ? "auto-dialing" : "dialing", number);
				break;
			case LineState::Ringing:
				SetRow(row, blink ? c_amber : c_white, "LINE %d  ringing...   %s", i + 1, number);
				break;
			case LineState::Busy:
				SetRow(row, c_red, "LINE %d  %-12s %s", i + 1,
					   line.outcome == Outcome::WrongNumber ? "beep beep" : "BUSY", number);
				break;
			case LineState::OnAir:
				if (line.outcome == Outcome::Won) {
					SetRow(row, blink ? c_amber : c_green, "LINE %d  WINNER! $%d", i + 1, line.prizeWon);
				} else if (line.outcome == Outcome::NoAnswer) {
					SetRow(row, c_white, "LINE %d  no answer", i + 1);
				} else {
					SetRow(row, c_green, "LINE %d  ON AIR as caller #%d", i + 1, line.callerNumber);
				}
				break;
		}
	}

	int8_t next = m_sim.NextUpgrade();
	if (!m_sim.IsStationUnlocked(1) && (m_flashTimer & 256)) {
		// takes turns with the shop
		SetRow(ROW_SHOP, c_grey, "WHOT 104.7 tunes in at $%d earned", Sim::c_stations[1].unlockAt);
	} else if (next < 0) {
		SetRow(ROW_SHOP, c_grey, "Shop sold out. The phone does the work.");
	} else {
		const auto& upgrade = Sim::c_upgrades[next];
		bool afford = m_sim.Cash() >= upgrade.price;
		SetRow(ROW_SHOP, afford ? c_amber : c_grey, "SQUARE buys %s  $%d", upgrade.name, upgrade.price);
	}

	if (m_toastTimer) {
		SetRow(ROW_HELP, c_amber, "%s", m_toast);
	} else {
		SetRow(ROW_HELP, c_grey, "X dial  O hang up%s%s", m_sim.HasUpgrade(Upgrade::Redial) ? "  TRI redial" : "",
			   m_sim.IsStationUnlocked(1) ? "  L1/R1 tune" : "");
	}
}

void PhoneScene::frame() {
	auto& renderer = Renderer::Instance();
	uint32_t deltaTime = renderer.Process();
	if (deltaTime == 0) {
		return;
	}

	m_sim.Tick(deltaTime);
	m_tones.Tick(deltaTime);
	m_flashTimer += deltaTime;
	m_keyFlash = m_keyFlash > deltaTime ? m_keyFlash - deltaTime : 0;
	m_toastTimer = m_toastTimer > deltaTime ? m_toastTimer - deltaTime : 0;

	UpdateDesk();
	renderer.Render(deltaTime);

	m_bandCount = 0;
	DrawBand(0, 70);
	DrawBand(174, 66);

	UpdateText();
	m_hud.Render();
}
