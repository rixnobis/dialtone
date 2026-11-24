#pragma once

#include <psyqo/advancedpad.hh>
#include <psyqo/scene.hh>

// the boot screen: which game to play. Each mode is its own scene with its own save file
class ModeScene final : public psyqo::Scene {
  public:
	ModeScene(psyqo::Scene* radio, psyqo::Scene* books) : m_modes{radio, books} {}

  private:
	void start(StartReason reason) override;
	void teardown(TearDownReason reason) override;
	void frame() override;

	psyqo::Scene* m_modes[2];
	uint8_t m_sel = 0;
	int8_t m_chosen = -1;
	uint16_t m_blink = 0;
};
