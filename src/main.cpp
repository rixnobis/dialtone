#include "game.hh"
#include "madnight.hh"
#include "book_scene.hh"
#include "mode_scene.hh"
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
	static BookScene bookScene;
	static ModeScene modeScene(&phoneScene, &bookScene);
	static constexpr const char* meshes[] = {"DESK",	 "PHONE",  "RADIO",	   "MUG",	   "HANDSET",  "NEEDLE",
											 "LAMP",	 "LAMPLIT", "KEY",	   "KEYSEL",   "KEYLIT",   "REDIAL",
											 "PHONE2",	 "AUTODIAL", "LINEBOARD", "BOOK",	   "BOOKDONE"};
	eastl::vector<LoadQueue> files;
	for (auto mesh : meshes) {
		LoadQueue entry;
		entry.name.sprintf("MODELS/%s.MB", mesh);
		entry.type = LoadFileType::OBJECT;
		files.push_back(entry);
	}
	co_await g_madnightEngine.HardLoadingScreen(eastl::move(files), &modeScene);
}

int main() { return g_madnightEngine.run(); }
