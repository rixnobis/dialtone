#pragma once

#include "sim.hh"

#include <stdint.h>

// The save on the memory card in slot 1: loaded once at boot (blocking), written in the background
// after that. No card, an unformatted card or a bad save never stops the game, it just plays unsaved.
class SaveGame final {
  public:
	enum class Status : uint8_t { NoCard, Unformatted, Fresh, Loaded, Corrupt, Error };

	// blocking; on Loaded the sim and the play time are restored
	Status Load(Sim& sim, uint32_t& playSeconds);
	// non-blocking; does nothing while a write is still running
	void Save(const Sim& sim, uint32_t playSeconds);

	Status BootStatus(void) const { return m_bootStatus; }
	bool Writing(void) const { return m_inFlight; }
	bool CardOk(void) const { return m_cardOk; }
	uint32_t SavesDone(void) const { return m_saves; }
	uint32_t LastSavedAt(void) const { return m_lastSavedAt; } // play seconds
	bool LastSaveFailed(void) const { return m_lastFailed; }

  private:
	struct File {
		uint32_t magic;
		uint16_t version;
		uint16_t size;
		SimSave sim;
		uint32_t playSeconds;
		uint32_t checksum; // FNV-1a over everything above
	};
	static uint32_t Checksum(const File& file);

	File m_file;
	Status m_bootStatus = Status::NoCard;
	bool m_cardOk = false;
	bool m_inFlight = false;
	bool m_lastFailed = false;
	uint32_t m_saves = 0;
	uint32_t m_startFrame = 0;
	uint32_t m_lastSavedAt = 0;
};
