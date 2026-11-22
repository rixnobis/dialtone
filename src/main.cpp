#include "game.hh"
#include "madnight.hh"
#include "phone_scene.hh"

#include <EASTL/vector.h>
#include <psyqo/coroutine.hh>

// the image a PCDRV build reads off the host, see tools/make_assets.sh
const char* g_gameIsoName = "dialtone.bin";

class DialtoneGame final : public MadnightEngineGame {
  public:
	psyqo::Coroutine<> InitialLoad(void) override;
};

static DialtoneGame s_game;
MadnightEngineGame& g_madnightEngineGame = s_game;

psyqo::Coroutine<> DialtoneGame::InitialLoad(void) {
	// built here, not as a global: the Camera inside needs the engine's trig tables
	static PhoneScene phoneScene;
	static constexpr const char* meshes[] = {"DESK", "PHONE",  "RADIO", "MUG",		"HANDSET", "NEEDLE", "LAMP",
											 "LAMPLIT", "KEY", "KEYSEL", "KEYLIT", "REDIAL",  "PHONE2", "AUTODIAL"};
	eastl::vector<LoadQueue> files;
	for (auto mesh : meshes) {
		LoadQueue entry;
		entry.name.sprintf("MODELS/%s.MB", mesh);
		entry.type = LoadFileType::OBJECT;
		files.push_back(entry);
	}
	co_await g_madnightEngine.HardLoadingScreen(eastl::move(files), &phoneScene);
}

int main() { return g_madnightEngine.run(); }
