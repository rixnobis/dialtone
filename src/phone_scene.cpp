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

static constexpr uint8_t SHOP_ENTRIES = 1 + uint8_t(Stat::Count);

enum TextRow : uint8_t {
	ROW_TITLE,
	ROW_STATION,
	ROW_DJ,
	ROW_ROUND,
	ROW_LINE1,
	ROW_LINE2,
	ROW_SHOP, // the shop and nothing else
	ROW_TOAST,
	ROW_HELP,
	ROW_BANNER1,
	ROW_BANNER2,
	ROW_INTRO, // first of INTRO_ROWS
};
static constexpr uint8_t INTRO_ROWS = 13;
static constexpr uint8_t ROW_COUNT = ROW_INTRO + INTRO_ROWS;
static constexpr int16_t c_rowY[ROW_INTRO] = {4, 20, 36, 52, 160, 176, 192, 208, 224, 98, 114};

static constexpr int16_t TOP_BAND_H = 70, BOTTOM_BAND_Y = 156, BANNER_Y = 92, BANNER_H = 42;
static constexpr uint16_t WIN_BANNER_TICKS = 150;
// the card has the pad bus to itself while it writes (about 3 s in Redux), so the autosave waits for
// the break between contests once it is due
static constexpr uint16_t AUTOSAVE_SECONDS = 120;
static constexpr uint8_t MENU_ENTRIES = 1 + uint8_t(Perk::Count); // the move, then the perks

static constexpr const char* c_intro[INTRO_ROWS] = {
	"         HOW TO PLAY DIALTONE",
	"",
	"The radio DJ says \"caller N wins\".",
	"Dial the station's number on the phone",
	"and get through as exactly caller N.",
	"Too early: small prize, call again.",
	"BUSY: hang up and redial, keep trying.",
	"",
	"D-pad + X  dial a key     O  hang up",
	"SELECT  pick shop item  SQUARE  buy",
	"Later: TRI redial L1/R1 tune START rep",
	"",
	"         press X to start",
};

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

// "x1.21" for small factors, "x4096", then "x1.23e7"
static void FormatFactor(char* out, const BigNum& factor) {
	*out++ = 'x';
	if (factor < BigNum::FromUint(100)) {
		uint32_t hundredths = (factor * BigNum::FromUint(100)).ToUint();
		snprintf(out, 12, "%d.%02d", hundredths / 100, hundredths % 100);
	} else {
		factor.Format(out);
	}
}

// the first word of a station name, "KNAT 96.1" -> "KNAT"
static void ShortName(char* out, const char* name) {
	uint8_t i = 0;
	while (name[i] && name[i] != ' ' && i < 5) {
		out[i] = name[i];
		i++;
	}
	out[i] = 0;
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
		int16_t y = i < ROW_INTRO ? c_rowY[i] : 12 + 16 * (i - ROW_INTRO);
		m_rows[i] = m_hud.AddTextHUDElement(TextHUDElement("row", {.pos = {{.x = 8, .y = y}}, .size = {320, 16}}));
		m_rows[i]->SetDisplayText("");
	}

	CreateDesk();

	g_madnightEngine.m_rand.seed(g_madnightEngine.gpu().now());
	m_tones.Init();
	m_sim.Init(&g_madnightEngine.m_rand, this);

	// a missing or bad card is reported once and then the game plays unsaved
	auto status = m_save.Load(m_sim, m_playSeconds);
	m_shownCash = m_sim.Cash();
	switch (status) {
		case SaveGame::Status::Loaded:
			snprintf(m_toast, sizeof(m_toast), "Welcome back! Save loaded.");
			break;
		case SaveGame::Status::Fresh:
			snprintf(m_toast, sizeof(m_toast), "New game, saving to card 1");
			break;
		case SaveGame::Status::Corrupt:
			snprintf(m_toast, sizeof(m_toast), "Save damaged: starting over");
			break;
		case SaveGame::Status::Unformatted:
			snprintf(m_toast, sizeof(m_toast), "Card 1 unformatted: not saving");
			break;
		default:
			snprintf(m_toast, sizeof(m_toast), "No memory card: not saving");
			break;
	}
	m_toastTimer = 240;
	m_lastFrame = g_madnightEngine.gpu().getFrameCount();
	m_nextSaveAt = m_playSeconds + AUTOSAVE_SECONDS;
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
		// the layout gives the two ends of the dial, the stations sit evenly between them
		m_needleStation = m_sim.StationIndex();
		psyqo::Vec3 pos = desk::NEEDLE[0];
		pos.x += (desk::NEEDLE[1].x - desk::NEEDLE[0].x) * int32_t(m_needleStation) / int32_t(STATION_COUNT - 1);
		m_needle->SetPosition(pos);
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
	if (m_sim.HasAutodialer() && !m_autodial) {
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

void PhoneScene::RemoveDeskUpgrades(void) {
	auto& objects = g_madnightEngine.m_gameObjectManager;
	for (GameObject** object : {&m_redial, &m_phone2, &m_lampLine2, &m_autodial, &m_lampAuto}) {
		if (*object) {
			objects.DestroyGameObject(*object);
			*object = nullptr;
		}
	}
	m_lampLine2Mesh = m_lampAutoMesh = 0;
}

void PhoneScene::ClearOverlay(void) {
	for (uint8_t i = 0; i < INTRO_ROWS; i++) {
		m_rows[ROW_INTRO + i]->SetDisplayText("");
	}
}

void PhoneScene::HandleMenuButton(psyqo::AdvancedPad::Button button) {
	using Button = psyqo::AdvancedPad::Button;
	switch (button) {
		case Button::Up:
			m_menuSel = (m_menuSel + MENU_ENTRIES - 1) % MENU_ENTRIES;
			m_menuArmed = false;
			break;
		case Button::Down:
			m_menuSel = (m_menuSel + 1) % MENU_ENTRIES;
			m_menuArmed = false;
			break;
		case Button::Start:
		case Button::Circle:
			m_menu = false;
			m_menuArmed = false;
			ClearOverlay();
			printf("DIALTONE: menu closed\n");
			break;
		case Button::Cross:
			if (m_menuSel == 0) {
				if (m_sim.PendingRep().IsZero()) {
					break;
				}
				if (!m_menuArmed) {
					m_menuArmed = true;
					break;
				}
				char gain[12];
				m_sim.PendingRep().Format(gain);
				m_sim.NewAreaCode();
				RemoveDeskUpgrades();
				m_shownCash = m_sim.Cash();
				m_bannerTimer = 0;
				m_menuArmed = false;
				char rep[12];
				m_sim.Reputation().Format(rep);
				printf("DIALTONE: new area code #%d +%s rep, rep=%s\n", m_sim.AreaCodes(), gain, rep);
				m_save.Save(m_sim, m_playSeconds);
				m_nextSaveAt = m_playSeconds + AUTOSAVE_SECONDS;
			} else {
				Perk perk = Perk(m_menuSel - 1);
				if (m_sim.BuyPerk(perk)) {
					m_nextSaveAt = m_playSeconds; // due now, written at the next break between contests
					char rep[12];
					m_sim.Reputation().Format(rep);
					printf("DIALTONE: perk %s lv%d rep=%s\n", Sim::c_perks[uint8_t(perk)].name, m_sim.PerkLevel(perk),
						   rep);
				}
			}
			break;
		default:
			break;
	}
}

void PhoneScene::HandleButton(psyqo::AdvancedPad::Button button) {
	using Button = psyqo::AdvancedPad::Button;
	if (m_intro) {
		if (button == Button::Cross || button == Button::Start) {
			m_intro = false;
			ClearOverlay();
			printf("DIALTONE: intro dismissed\n");
		}
		return;
	}
	if (m_menu) {
		HandleMenuButton(button);
		return;
	}
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
		case Button::Square:
			BuyShopItem();
			break;
		case Button::Select:
			m_shopSel = (m_shopSel + 1) % SHOP_ENTRIES;
			break;
		case Button::L1:
			m_sim.TuneStation(-1);
			break;
		case Button::R1:
			m_sim.TuneStation(1);
			break;
		case Button::Start:
			m_menu = true;
			m_menuSel = 0;
			m_menuArmed = false;
			printf("DIALTONE: menu opened\n");
			break;
		default:
			break;
	}
}

bool PhoneScene::BuyShopItem(void) {
	if (m_shopSel == 0) {
		int8_t next = m_sim.NextUpgrade();
		if (next < 0 || !m_sim.BuyNextUpgrade()) {
			return false;
		}
		snprintf(m_toast, sizeof(m_toast), "Bought: %s!", Sim::c_upgrades[next].name);
		char cash[12];
		m_sim.Cash().Format(cash);
		printf("DIALTONE: bought %s cash=%s\n", Sim::c_upgrades[next].name, cash);
	} else {
		Stat stat = Stat(m_shopSel - 1);
		if (!m_sim.BuyStat(stat)) {
			return false;
		}
		uint16_t level = m_sim.StatLevel(stat);
		char factor[16];
		FormatFactor(factor, m_sim.PrizeMultiplier());
		switch (stat) {
			case Stat::Autodialer:
				if (m_sim.AutodialWindow()) {
					snprintf(m_toast, sizeof(m_toast), "Autodialer %d: dials %d before the winner", level,
							 m_sim.AutodialWindow());
				} else {
					snprintf(m_toast, sizeof(m_toast), "Autodialer %d: %d line%s, waits %ds", level,
							 m_sim.AutodialLines(), m_sim.AutodialLines() > 1 ? "s" : "",
							 m_sim.AutodialIdleTicks() / TICKS_PER_SECOND);
				}
				break;
			case Stat::QuickFingers:
				snprintf(m_toast, sizeof(m_toast), "Quick fingers %d: dialing is faster", level);
				break;
			case Stat::Luck:
				snprintf(m_toast, sizeof(m_toast), "Luck %d: busy %d%% of the time", level, m_sim.BusyPercent());
				break;
			default:
				snprintf(m_toast, sizeof(m_toast), "Charm %d: prizes %s", level, factor);
				break;
		}
		char cash[12], price[12];
		m_sim.Cash().Format(cash);
		m_sim.StatPrice(stat).Format(price);
		printf("DIALTONE: bought %s lv%d cash=%s next=%s digit=%d connect=%d busy=%d%% prize=%s auto=%d/%d/%d\n",
			   Sim::c_stats[uint8_t(stat)].name, level, cash, price, m_sim.AutoDigitTicks(), m_sim.ConnectTicks(),
			   m_sim.BusyPercent(), factor, m_sim.AutodialLines(), m_sim.AutodialIdleTicks(), m_sim.AutodialDigitTicks());
	}
	m_toastTimer = 120;
	return true;
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
	m_bannerTimer = WIN_BANNER_TICKS;
	m_bannerLine = line;
	char cash[12], all[12];
	m_sim.Cash().Format(cash);
	m_sim.AllTimeEarned().Format(all);
	printf("DIALTONE: win cash=%s wins=%d alltime=%s\n", cash, m_sim.Wins(), all);
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

void PhoneScene::DrawBand(int16_t y, int16_t h, psyqo::Color colour) {
	auto& gpu = g_madnightEngine.gpu();
	auto& fragment = m_bands[gpu.getParity()][m_bandCount++];
	fragment.primitive.position = {{.x = 0, .y = y}};
	fragment.primitive.size = {{.x = 320, .y = h}};
	fragment.primitive.setColor(colour);
	fragment.primitive.setSemiTrans();
	gpu.chain(fragment);
}

void PhoneScene::UpdateIntro(void) {
	for (uint8_t i = 0; i < INTRO_ROWS; i++) {
		bool prompt = i == INTRO_ROWS - 1;
		SetRow(ROW_INTRO + i, i == 0 || prompt ? c_amber : c_white, "%s", prompt && (m_flashTimer & 32) ? "" : c_intro[i]);
	}
}

void PhoneScene::UpdateLineRow(uint8_t index) {
	const auto& line = m_sim.GetLine(index);
	const auto& station = m_sim.CurrentStation();
	uint8_t row = index == 0 ? ROW_LINE1 : ROW_LINE2;
	bool blink = m_flashTimer & 16;
	char number[12];
	FormatNumber(number, line.digits, line.digitCount);

	switch (line.state) {
		case LineState::Locked:
			SetRow(row, c_grey, "LINE %d  not bought yet", index + 1);
			break;
		case LineState::Idle:
			// an old "too late" means nothing once the next contest is open
			switch (line.outcome == Outcome::NoAnswer && m_sim.IsRoundOpen() ? Outcome::None : line.outcome) {
				case Outcome::Busy:
					SetRow(row, c_white, "LINE %d  was busy: dial again", index + 1);
					break;
				case Outcome::WrongNumber:
					char want[12];
					FormatNumber(want, station.number, NUMBER_LENGTH);
					SetRow(row, c_red, "LINE %d  wrong number, try %s", index + 1, want);
					break;
				case Outcome::NoAnswer:
					SetRow(row, c_white, "LINE %d  too late, contest over", index + 1);
					break;
				case Outcome::Missed:
					SetRow(row, c_white, "LINE %d  you #%d, DJ wanted #%d", index + 1, line.callerNumber,
						   line.callerTarget);
					break;
				case Outcome::Won:
					char won[12];
					line.prizeWon.Format(won);
					SetRow(row, c_amber, "LINE %d  you were #%d: won $%s", index + 1, line.callerNumber, won);
					break;
				default:
					SetRow(row, c_white, "LINE %d  free%s", index + 1,
						   m_sim.IsRoundOpen() ? ": dial the station now" : "");
					break;
			}
			break;
		case LineState::Dialing:
			SetRow(row, c_white, "LINE %d  %-12s %s", index + 1,
				   line.digitCount == NUMBER_LENGTH ? "connecting"
				   : line.autoDialing				? "auto-dialing"
													: "dialing",
				   number);
			break;
		case LineState::Ringing:
			SetRow(row, blink ? c_amber : c_white, "LINE %d  ringing the DJ... %s", index + 1, number);
			break;
		case LineState::Busy:
			if (line.outcome == Outcome::WrongNumber) {
				SetRow(row, c_red, "LINE %d  WRONG NUMBER   %s", index + 1, number);
			} else {
				SetRow(row, c_red, "LINE %d  BUSY, redial %s", index + 1, m_sim.HasUpgrade(Upgrade::Redial) ? "TRI" : "");
			}
			break;
		case LineState::OnAir:
			if (line.outcome == Outcome::Won) {
				SetRow(row, blink ? c_amber : c_green, "LINE %d  WINNER! caller #%d", index + 1, line.callerNumber);
			} else if (line.outcome == Outcome::NoAnswer) {
				SetRow(row, c_white, "LINE %d  no contest on, wait", index + 1);
			} else {
				SetRow(row, c_green, "LINE %d  ON AIR: you #%d, wants #%d", index + 1, line.callerNumber,
					   line.callerTarget);
			}
			break;
	}
}

void PhoneScene::UpdateText(void) {
	const auto& station = m_sim.CurrentStation();
	char number[12];

	// the next goal sits up here so the shop row below is never shared
	char cash[12], goal[12], target[12], name[6];
	m_shownCash.Format(cash);
	int8_t locked = m_sim.NextLockedStation();
	BigNum pending = m_sim.PendingRep();
	if (locked >= 0) {
		const auto& next = Sim::c_stations[locked];
		ShortName(name, next.name);
		m_sim.RunEarned().Format(goal);
		BigNum::FromUint(next.unlockAt).Format(target);
		SetRow(ROW_TITLE, c_amber, "DIALTONE $%-7s %s at $%s/$%s", cash, name, goal, target);
	} else if (!pending.IsZero()) {
		pending.Format(goal);
		SetRow(ROW_TITLE, (m_flashTimer & 32) ? c_amber : c_green, "DIALTONE $%-7s START: +%s rep", cash, goal);
	} else {
		SetRow(ROW_TITLE, c_amber, "DIALTONE                 $%s", cash);
	}

	FormatNumber(number, station.number, NUMBER_LENGTH);
	SetRow(ROW_STATION, c_white, "RADIO %-12s CALL %s", station.name, number);
	// the DJ speaks in quotes; no "DJ:" label, a six-digit prize needs the room
	SetRow(ROW_DJ, c_green, "\"%s\"", m_sim.DJLine());

	if (m_sim.IsRoundOpen()) {
		bool hurry = m_sim.SecondsLeft() <= 5 && (m_flashTimer & 16);
		char prize[12];
		m_sim.Prize().Format(prize);
		SetRow(ROW_ROUND, hurry ? c_red : c_white, "WANTS #%-2d SO FAR %-2d  $%-7s %2ds", m_sim.CallerTarget(),
			   m_sim.CallersSoFar(), prize, m_sim.SecondsLeft());
	} else {
		SetRow(ROW_ROUND, c_grey, "Contest closed, next in %ds%s", m_sim.SecondsLeft(),
			   m_save.Writing() ? "  SAVING" : "");
	}

	for (uint8_t i = 0; i < MAX_LINES; i++) {
		UpdateLineRow(i);
	}

	if (m_shopSel == 0) {
		int8_t next = m_sim.NextUpgrade();
		if (next < 0) {
			SetRow(ROW_SHOP, c_grey, "SHOP %d/%d desk complete", 1, SHOP_ENTRIES);
		} else {
			const auto& upgrade = Sim::c_upgrades[next];
			bool afford = m_sim.Cash() >= BigNum::FromUint(upgrade.price);
			SetRow(ROW_SHOP, afford ? c_amber : c_grey, "SHOP %d/%d %-13s $%d", 1, SHOP_ENTRIES, upgrade.name,
				   upgrade.price);
		}
	} else {
		Stat stat = Stat(m_shopSel - 1);
		BigNum price = m_sim.StatPrice(stat);
		const char* name = Sim::c_stats[uint8_t(stat)].name;
		if (price.IsZero()) {
			SetRow(ROW_SHOP, c_grey, "SHOP %d/%d %-13s Lv%d MAX", m_shopSel + 1, SHOP_ENTRIES, name,
				   m_sim.StatLevel(stat));
		} else {
			char text[12];
			price.Format(text);
			bool afford = m_sim.Cash() >= price;
			SetRow(ROW_SHOP, afford ? c_amber : c_grey, "SHOP %d/%d %-13s Lv%d $%s", m_shopSel + 1, SHOP_ENTRIES,
				   name, m_sim.StatLevel(stat), text);
		}
	}

	SetRow(ROW_TOAST, c_amber, "%s", m_toastTimer ? m_toast : "");
	SetRow(ROW_HELP, c_grey, "X dial O hang SEL/SQ shop%s%s START", m_sim.HasUpgrade(Upgrade::Redial) ? " TRI" : "",
		   m_sim.IsStationUnlocked(1) ? " L/R" : "");

	// the middle banner: who you are the moment you get on air, and a win held long enough to see
	if (m_bannerTimer) {
		const auto& line = m_sim.GetLine(m_bannerLine);
		bool blink = m_flashTimer & 8;
		if (line.outcome == Outcome::Won) {
			SetRow(ROW_BANNER1, blink ? c_white : c_amber, "    *** YOU WIN!  CALLER #%d ***", line.callerNumber);
			char won[12];
			line.prizeWon.Format(won);
			SetRow(ROW_BANNER2, c_amber, "          +$%s  cash $%s", won, cash);
		} else {
			SetRow(ROW_BANNER1, c_green, "  ON AIR!  You are caller #%d", line.callerNumber);
			SetRow(ROW_BANNER2, c_white, "  DJ wants #%d: %s", line.callerTarget,
				   line.callerNumber < line.callerTarget ? "too early, call again" : "missed it");
		}
	} else {
		SetRow(ROW_BANNER1, c_white, "");
		SetRow(ROW_BANNER2, c_white, "");
	}
}

void PhoneScene::TickClock(void) {
	auto& gpu = g_madnightEngine.gpu();
	uint32_t now = gpu.getFrameCount();
	m_frameAcc += now - m_lastFrame;
	m_lastFrame = now;
	uint32_t rate = gpu.getRefreshRate();
	while (m_frameAcc >= rate) {
		m_frameAcc -= rate;
		m_playSeconds++;
	}
	if (!m_intro && !m_menu && !m_sim.IsRoundOpen() && m_playSeconds >= m_nextSaveAt) {
		m_nextSaveAt = m_playSeconds + AUTOSAVE_SECONDS;
		m_save.Save(m_sim, m_playSeconds);
	}
}

void PhoneScene::UpdateMenu(void) {
	char rep[12], all[12], gain[12], text[16], price[12];
	m_sim.Reputation().Format(rep);
	m_sim.AllTimeEarned().Format(all);
	uint8_t r = ROW_INTRO;
	SetRow(r++, c_amber, "      REPUTATION AND AREA CODES");
	SetRow(r++, c_white, "Reputation %-8s area codes moved %d", rep, m_sim.AreaCodes());
	SetRow(r++, c_grey, "Played %dh%02dm   all-time $%s", m_playSeconds / 3600, m_playSeconds / 60 % 60, all);
	SetRow(r++, c_white, "");

	BigNum pending = m_sim.PendingRep();
	char cursor = m_menuSel == 0 ? '>' : ' ';
	if (pending.IsZero()) {
		m_sim.NextRepAt().Format(gain);
		SetRow(r++, m_menuSel == 0 ? c_white : c_grey, "%cNew area code: at $%s all-time", cursor, gain);
	} else {
		pending.Format(gain);
		SetRow(r++, m_menuSel == 0 ? c_amber : c_white, "%cNew area code now: +%s rep", cursor, gain);
	}

	for (uint8_t i = 0; i < uint8_t(Perk::Count); i++) {
		Perk perk = Perk(i);
		uint16_t level = m_sim.PerkLevel(perk);
		switch (perk) {
			case Perk::BigName:
				FormatFactor(text, BigNum::FromUint(2).Pow(level));
				break;
			case Perk::HeadStart:
				text[0] = '$';
				m_sim.StartCash().Format(text + 1);
				break;
			case Perk::SpeedDial:
				snprintf(text, sizeof(text), "%s", level ? "redial" : "no");
				break;
			default:
				snprintf(text, sizeof(text), "x1.%02d", (m_sim.GrowthMilli() - 1000) / 10);
				break;
		}
		BigNum cost = m_sim.PerkPrice(perk);
		bool selected = m_menuSel == i + 1;
		psyqo::Color colour = selected ? (m_sim.Reputation() >= cost && !cost.IsZero() ? c_amber : c_white) : c_grey;
		if (cost.IsZero()) {
			SetRow(r++, colour, "%c%-11sLv%-3d %-10s MAX", selected ? '>' : ' ', Sim::c_perks[i].name, level, text);
		} else {
			cost.Format(price);
			SetRow(r++, colour, "%c%-11sLv%-3d %-10s %s rep", selected ? '>' : ' ', Sim::c_perks[i].name, level,
				   text, price);
		}
	}
	SetRow(r++, c_white, "");

	static constexpr const char* c_help[MENU_ENTRIES] = {
		"Resets cash, desk, stats; keeps perks",
		"Every level doubles every prize",
		"Start each area code with more cash",
		"Start each area code with redial",
		"Shop prices grow slower per level",
	};
	if (m_menuSel == 0 && m_menuArmed) {
		SetRow(r++, (m_flashTimer & 16) ? c_red : c_amber, "Sure? X again to move, O to stay");
	} else {
		SetRow(r++, c_green, "%s", c_help[m_menuSel]);
	}

	if (m_save.Writing()) {
		SetRow(r++, c_grey, "Saving to memory card 1...");
	} else if (m_save.CardOk()) {
		if (m_save.SavesDone()) {
			SetRow(r++, c_grey, "Card 1: saved %ds ago, every 2 min", m_playSeconds - m_save.LastSavedAt());
		} else {
			SetRow(r++, c_grey, "Card 1: autosave every 2 min");
		}
	} else {
		SetRow(r++, c_grey, "No memory card in slot 1: not saving");
	}
	SetRow(r++, c_grey, "UP/DOWN pick   X buy   START back");
}

void PhoneScene::frame() {
	auto& renderer = Renderer::Instance();
	uint32_t deltaTime = renderer.Process();
	if (deltaTime == 0) {
		return;
	}

	m_flashTimer += deltaTime;
	TickClock();
	if (!m_intro && !m_menu) {
		m_sim.Tick(deltaTime);
		m_tones.Tick(deltaTime);
		m_keyFlash = m_keyFlash > deltaTime ? m_keyFlash - deltaTime : 0;
		m_toastTimer = m_toastTimer > deltaTime ? m_toastTimer - deltaTime : 0;
		m_bannerTimer = m_bannerTimer > deltaTime ? m_bannerTimer - deltaTime : 0;

		// a line that just got on air without winning gets the banner for as long as it is on air
		for (uint8_t i = 0; i < MAX_LINES; i++) {
			const auto& line = m_sim.GetLine(i);
			bool onAir = line.state == LineState::OnAir && line.outcome == Outcome::Missed;
			bool winShowing = m_bannerTimer && m_sim.GetLine(m_bannerLine).outcome == Outcome::Won;
			if (onAir && !m_wasOnAir[i] && !winShowing) {
				m_bannerTimer = 100;
				m_bannerLine = i;
			}
			m_wasOnAir[i] = onAir;
		}

		// cash counts up rather than jumping, so a win is seen landing
		const BigNum& cash = m_sim.Cash();
		if (m_shownCash > cash) {
			m_shownCash = cash;
		} else if (m_shownCash < cash) {
			BigNum diff = cash - m_shownCash;
			BigNum step = (diff * BigNum::FromMilli(62)).Floor();
			if (step.IsZero()) {
				step = BigNum::FromUint(1);
			}
			m_shownCash += step * BigNum::FromUint(deltaTime);
			if (m_shownCash > cash) {
				m_shownCash = cash;
			}
		}
	}

	UpdateDesk();
	renderer.Render(deltaTime);

	m_bandCount = 0;
	if (m_intro || m_menu) {
		// twice: one semi-transparent band still lets the desk fight the text
		DrawBand(0, 240, c_band);
		DrawBand(0, 240, c_band);
		if (m_intro) {
			UpdateIntro();
		} else {
			UpdateMenu();
		}
	} else {
		DrawBand(0, TOP_BAND_H, c_band);
		DrawBand(BOTTOM_BAND_Y, 240 - BOTTOM_BAND_Y, c_band);
		if (m_bannerTimer) {
			bool won = m_sim.GetLine(m_bannerLine).outcome == Outcome::Won;
			DrawBand(BANNER_Y, BANNER_H, won ? psyqo::Color{.r = 120, .g = 60, .b = 0} : c_band);
		}
	}

	UpdateText();
	if (m_intro || m_menu) {
		for (uint8_t i = 0; i < ROW_INTRO; i++) {
			m_rows[i]->SetDisplayText("");
		}
	}
	m_hud.Render();
}
