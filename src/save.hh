#pragma once

#include "book_sim.hh"
#include "sim.hh"

#include <stdint.h>

// One save file on the memory card in slot 1: loaded once at boot (blocking), written in the
// background after that. No card, an unformatted card or a bad save never stops the game, it just
// plays unsaved. On the card: magic, version, total size, the payload padded to 4, play seconds and
// an FNV-1a checksum over everything before it.
class SaveSlot {
  public:
	enum class Status : uint8_t { NoCard, Unformatted, Fresh, Loaded, Corrupt, Error };

	SaveSlot(const char* name, uint32_t magic, uint16_t version) : m_name(name), m_magic(magic), m_version(version) {}

	// blocking; on Loaded the payload and the play time are filled in
	Status Load(void* payload, uint16_t size, uint32_t& playSeconds);
	// the same for a save that has had more than one layout: `which` is the format the file is in, and
	// the payload must have room for the biggest
	struct Format {
		uint16_t version, size;
	};
	Status Load(const Format* formats, uint8_t count, void* payload, uint32_t& playSeconds, uint8_t& which);
	// non-blocking; does nothing while a write is still running
	void Save(const void* payload, uint16_t size, uint32_t playSeconds);

	Status BootStatus(void) const { return m_bootStatus; }
	bool Writing(void) const { return m_inFlight; }
	// a real file write (the card owns the pad until it ends), not the quick probe for a card
	bool WritingFile(void) const { return m_inFlight && m_cardOk; }
	bool CardOk(void) const { return m_cardOk; }
	uint32_t SavesDone(void) const { return m_saves; }
	uint32_t LastSavedAt(void) const { return m_lastSavedAt; } // play seconds
	bool LastSaveFailed(void) const { return m_lastFailed; }

  private:
	static constexpr uint16_t MAX_FILE = 512;
	static constexpr uint16_t HEADER = 8;
	static uint16_t FileSize(uint16_t payload) { return HEADER + ((payload + 3) & ~3) + 8; }
	static uint32_t Checksum(const uint8_t* file, uint16_t length);

	const char* m_name;
	uint32_t m_magic;
	uint16_t m_version;
	alignas(4) uint8_t m_file[MAX_FILE];
	uint32_t m_writingAt = 0; // play seconds of the write in flight
	Status m_bootStatus = Status::NoCard;
	bool m_cardOk = false;
	bool m_inFlight = false;
	bool m_lastFailed = false;
	uint32_t m_saves = 0;
	uint32_t m_startFrame = 0;
	uint32_t m_lastSavedAt = 0;
};

// the radio contests save, file DIALTONE, the same bytes v1 wrote
class SaveGame final : public SaveSlot {
  public:
	SaveGame();
	Status Load(Sim& sim, uint32_t& playSeconds);
	void Save(const Sim& sim, uint32_t playSeconds);
};

// the phone books save, its own file so neither mode touches the other's
class BookSaveGame final : public SaveSlot {
  public:
	BookSaveGame();
	Status Load(BookSim& sim, uint32_t& playSeconds);
	void Save(const BookSim& sim, uint32_t playSeconds);
};
