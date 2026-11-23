#include "save.hh"

#include "madnight.hh"

#include <psyqo/xprintf.h>
#include <stddef.h>

static constexpr uint32_t SAVE_MAGIC = 0x4e544c44; // "DLTN"
static constexpr uint16_t SAVE_VERSION = 1;
static constexpr const char* SAVE_NAME = "BASCUS-00000DIALTONE";
static constexpr const char* SAVE_TITLE = "DIALTONE";
static constexpr MC::Port SAVE_PORT = MC::Port::Port0;

// a red phone on black, 16x16 4bpp
static const MCFS::Icon& Icon(void) {
	static MCFS::Icon icon;
	static bool built = false;
	if (built) {
		return icon;
	}
	static constexpr const char* rows[16] = {
		"................", "................", "..rrrrrrrrrrrr..", ".rrwwrrrrrrwwrr.", ".rrwwrrrrrrwwrr.",
		"..rr........rr..", "....rrrrrrrr....", "...rrwwrrwwrr...", "..rrrrrrrrrrrr..", "..rrwwrrwwrrrr..",
		"..rrrrrrrrrrrr..", "..rrwwrrwwrrrr..", "..rrrrrrrrrrrr..", "..rrrrrrrrrrrr..", "................",
		"................"};
	icon.frameCount = 1;
	icon.clut[0] = 0x0000;
	icon.clut[1] = 0x0c1f; // red, BGR555
	icon.clut[2] = 0x7fff;
	for (uint8_t y = 0; y < 16; y++) {
		for (uint8_t x = 0; x < 16; x += 2) {
			auto px = [&](uint8_t c) -> uint8_t { return c == 'r' ? 1 : c == 'w' ? 2 : 0; };
			icon.pixels[0][y * 8 + x / 2] = px(rows[y][x]) | (px(rows[y][x + 1]) << 4);
		}
	}
	built = true;
	return icon;
}

uint32_t SaveGame::Checksum(const File& file) {
	auto bytes = reinterpret_cast<const uint8_t*>(&file);
	uint32_t hash = 2166136261u;
	for (uint32_t i = 0; i < offsetof(File, checksum); i++) {
		hash = (hash ^ bytes[i]) * 16777619u;
	}
	return hash;
}

SaveGame::Status SaveGame::Load(Sim& sim, uint32_t& playSeconds) {
	auto& card = g_madnightEngine.m_memoryCardHelper;
	MC::Error state = card.GetCardState(SAVE_PORT);
	if (state != MC::Error::OK) {
		m_bootStatus = state == MC::Error::NotFormatted ? Status::Unformatted : Status::NoCard;
		printf("DIALTONE: save card state %s\n", MC::errorMessage(state));
		return m_bootStatus;
	}
	m_cardOk = true;

	bool exists = false;
	MC::Error err = card.DoesSaveFileExist(SAVE_PORT, SAVE_NAME, &exists);
	if (err != MC::Error::OK || !exists) {
		m_bootStatus = err == MC::Error::OK ? Status::Fresh : Status::Error;
		printf("DIALTONE: save none on card (%s)\n", MC::errorMessage(err));
		return m_bootStatus;
	}

	uint32_t length = 0;
	err = card.ReadFile(SAVE_PORT, SAVE_NAME, &m_file, sizeof(m_file), &length);
	if (err != MC::Error::OK) {
		m_bootStatus = Status::Error;
		printf("DIALTONE: save read failed %s\n", MC::errorMessage(err));
		return m_bootStatus;
	}
	if (length < sizeof(m_file) || m_file.magic != SAVE_MAGIC || m_file.version != SAVE_VERSION ||
		m_file.size != sizeof(m_file) || m_file.checksum != Checksum(m_file)) {
		m_bootStatus = Status::Corrupt;
		printf("DIALTONE: save ignored, bad header or checksum (len=%d magic=%08x)\n", length, m_file.magic);
		return m_bootStatus;
	}

	sim.Load(m_file.sim);
	playSeconds = m_file.playSeconds;
	m_bootStatus = Status::Loaded;
	char cash[12];
	sim.Cash().Format(cash);
	printf("DIALTONE: save loaded cash=%s areacodes=%d played=%ds\n", cash, sim.AreaCodes(), playSeconds);
	return m_bootStatus;
}

void SaveGame::Save(const Sim& sim, uint32_t playSeconds) {
	if (m_inFlight) {
		return;
	}
	auto& card = g_madnightEngine.m_memoryCardHelper;
	m_inFlight = true;
	if (!m_cardOk) {
		// a card may have gone in since boot; look now, write on the next save
		card.GetCardState(SAVE_PORT, [this](MC::Error state) {
			m_inFlight = false;
			m_cardOk = state == MC::Error::OK;
		});
		return;
	}

	m_file.magic = SAVE_MAGIC;
	m_file.version = SAVE_VERSION;
	m_file.size = sizeof(m_file);
	sim.Save(m_file.sim);
	m_file.playSeconds = playSeconds;
	m_file.checksum = Checksum(m_file);
	m_startFrame = g_madnightEngine.gpu().getFrameCount();
	card.WriteFile(SAVE_PORT, SAVE_NAME, SAVE_TITLE, Icon(), &m_file, sizeof(m_file), [this](MC::Error err) {
		m_inFlight = false;
		m_lastFailed = err != MC::Error::OK;
		if (m_lastFailed) {
			// pulled out, or full: stop writing until a probe finds a card again
			m_cardOk = false;
			printf("DIALTONE: save failed %s\n", MC::errorMessage(err));
		} else {
			m_saves++;
			m_lastSavedAt = m_file.playSeconds;
			printf("DIALTONE: saved #%d played=%ds took=%d frames\n", m_saves, m_file.playSeconds,
				   g_madnightEngine.gpu().getFrameCount() - m_startFrame);
		}
	});
}
