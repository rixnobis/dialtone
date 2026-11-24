#include "save.hh"

#include "madnight.hh"

#include <EASTL/algorithm.h>
#include <psyqo/xprintf.h>
#include <stddef.h>

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

uint32_t SaveSlot::Checksum(const uint8_t* file, uint16_t length) {
	uint32_t hash = 2166136261u;
	for (uint16_t i = 0; i < length; i++) {
		hash = (hash ^ file[i]) * 16777619u;
	}
	return hash;
}

SaveSlot::Status SaveSlot::Load(void* payload, uint16_t size, uint32_t& playSeconds) {
	Format format = {.version = m_version, .size = size};
	uint8_t which;
	return Load(&format, 1, payload, playSeconds, which);
}

SaveSlot::Status SaveSlot::Load(const Format* formats, uint8_t count, void* payload, uint32_t& playSeconds,
								uint8_t& which) {
	auto& card = g_madnightEngine.m_memoryCardHelper;
	MC::Error state = card.GetCardState(SAVE_PORT);
	if (state != MC::Error::OK) {
		m_bootStatus = state == MC::Error::NotFormatted ? Status::Unformatted : Status::NoCard;
		printf("DIALTONE: save card state %s\n", MC::errorMessage(state));
		return m_bootStatus;
	}
	m_cardOk = true;

	bool exists = false;
	MC::Error err = card.DoesSaveFileExist(SAVE_PORT, m_name, &exists);
	if (err != MC::Error::OK || !exists) {
		m_bootStatus = err == MC::Error::OK ? Status::Fresh : Status::Error;
		printf("DIALTONE: save none on card (%s)\n", MC::errorMessage(err));
		return m_bootStatus;
	}

	// read as much as the biggest version needs, then go by the version in the header
	uint16_t readSize = 0;
	for (uint8_t i = 0; i < count; i++) {
		readSize = eastl::max(readSize, FileSize(formats[i].size));
	}
	uint32_t length = 0;
	err = card.ReadFile(SAVE_PORT, m_name, m_file, readSize, &length);
	if (err != MC::Error::OK) {
		m_bootStatus = Status::Error;
		printf("DIALTONE: save read failed %s\n", MC::errorMessage(err));
		return m_bootStatus;
	}
	uint32_t magic, sum = 0;
	uint16_t version, stored;
	__builtin_memcpy(&magic, m_file, 4);
	__builtin_memcpy(&version, m_file + 4, 2);
	__builtin_memcpy(&stored, m_file + 6, 2);
	which = count;
	for (uint8_t i = 0; i < count; i++) {
		if (formats[i].version == version) {
			which = i;
		}
	}
	uint16_t size = which < count ? formats[which].size : 0;
	uint16_t fileSize = FileSize(size);
	if (which < count) {
		__builtin_memcpy(&sum, m_file + fileSize - 4, 4);
	}
	if (which == count || length < fileSize || magic != m_magic || stored != fileSize ||
		sum != Checksum(m_file, fileSize - 4)) {
		m_bootStatus = Status::Corrupt;
		printf("DIALTONE: save ignored, bad header or checksum (len=%d magic=%08x)\n", length, magic);
		return m_bootStatus;
	}

	__builtin_memcpy(payload, m_file + HEADER, size);
	__builtin_memcpy(&playSeconds, m_file + fileSize - 8, 4);
	m_bootStatus = Status::Loaded;
	return m_bootStatus;
}

void SaveSlot::Save(const void* payload, uint16_t size, uint32_t playSeconds) {
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

	uint16_t fileSize = FileSize(size);
	__builtin_memset(m_file, 0, fileSize);
	__builtin_memcpy(m_file, &m_magic, 4);
	__builtin_memcpy(m_file + 4, &m_version, 2);
	__builtin_memcpy(m_file + 6, &fileSize, 2);
	__builtin_memcpy(m_file + HEADER, payload, size);
	__builtin_memcpy(m_file + fileSize - 8, &playSeconds, 4);
	uint32_t sum = Checksum(m_file, fileSize - 4);
	__builtin_memcpy(m_file + fileSize - 4, &sum, 4);
	m_writingAt = playSeconds;
	m_startFrame = g_madnightEngine.gpu().getFrameCount();
	card.WriteFile(SAVE_PORT, m_name, SAVE_TITLE, Icon(), m_file, fileSize, [this](MC::Error err) {
		m_inFlight = false;
		m_lastFailed = err != MC::Error::OK;
		if (m_lastFailed) {
			// pulled out, or full: stop writing until a probe finds a card again
			m_cardOk = false;
			printf("DIALTONE: save failed %s\n", MC::errorMessage(err));
		} else {
			m_saves++;
			m_lastSavedAt = m_writingAt;
			printf("DIALTONE: saved #%d played=%ds took=%d frames\n", m_saves, m_writingAt,
				   g_madnightEngine.gpu().getFrameCount() - m_startFrame);
		}
	});
}

SaveGame::SaveGame() : SaveSlot("BASCUS-00000DIALTONE", 0x4e544c44 /* "DLTN" */, 1) {}

SaveSlot::Status SaveGame::Load(Sim& sim, uint32_t& playSeconds) {
	SimSave data;
	Status status = SaveSlot::Load(&data, sizeof(data), playSeconds);
	if (status != Status::Loaded) {
		return status;
	}
	sim.Load(data);
	char cash[12];
	sim.Cash().Format(cash);
	printf("DIALTONE: save loaded cash=%s areacodes=%d played=%ds\n", cash, sim.AreaCodes(), playSeconds);
	return status;
}

void SaveGame::Save(const Sim& sim, uint32_t playSeconds) {
	if (Writing()) {
		return;
	}
	SimSave data;
	sim.Save(data);
	SaveSlot::Save(&data, sizeof(data), playSeconds);
}

// version 2: bought books, the multi-line autodialer, promotions. Version 1 (phase 1) saves are
// read and migrated, and the next save writes them back as version 2
BookSaveGame::BookSaveGame() : SaveSlot("BASCUS-00000DIALBOOK", 0x4b424c44 /* "DLBK" */, 2) {}

SaveSlot::Status BookSaveGame::Load(BookSim& sim, uint32_t& playSeconds) {
	static constexpr Format formats[] = {{.version = 2, .size = sizeof(BookSave)},
										 {.version = 1, .size = sizeof(BookSaveV1)}};
	union {
		BookSave v2;
		BookSaveV1 v1;
	} data;
	uint8_t which = 0;
	Status status = SaveSlot::Load(formats, 2, &data, playSeconds, which);
	if (status != Status::Loaded) {
		return status;
	}
	if (which == 1) {
		BookSaveV1 old = data.v1;
		BookSim::Migrate(old, data.v2);
	}
	sim.Load(data.v2);
	return status;
}

void BookSaveGame::Save(const BookSim& sim, uint32_t playSeconds) {
	if (Writing()) {
		return;
	}
	BookSave data;
	sim.Save(data);
	SaveSlot::Save(&data, sizeof(data), playSeconds);
}
