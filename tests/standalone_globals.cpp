// Original OS MIDI Globals -> real disk writes -> standalone sidecar -> fresh machine.
#include "panel_rig.h"
#include "standalone/gui_config.h"
#include "runtime/eiii_runtime.h"
#include <filesystem>
using namespace eiii_test;
static void globals(rig &t){t.press("master");t.press("keypad_8");t.press("keypad_7");}
int main()
{
	if(env("EIII_HD").empty() || env("EIII_ROM_DIR").empty())return 77;
	auto base=std::filesystem::temp_directory_path()/"eiii-standalone-global-test";
	std::filesystem::create_directories(base);
#ifdef _WIN32
	_putenv_s("APPDATA",base.string().c_str());
#else
	setenv("XDG_CONFIG_HOME",base.string().c_str(),1);
#endif
	{
		rig t(media_config());if(!t.wait_row(0,"P00 ",25))return 1;t.pump(1);globals(t);
		t.press("cursor_down");t.press("cursor_down");t.press("yes");
		if(t.lcd().find("omni")==std::string::npos)return 2;
		t.press("enter");t.press("master");t.pump(2);
		auto state=t.e.hd_changes(1);if(state.empty() || !standalone_write(standalone_hd_state(1),state))return 3;
	}
	auto cfg=media_config();cfg.rt.hd_changes[1]=standalone_read(standalone_hd_state(1));
	{
		rig t(cfg);if(!t.wait_row(0,"P00 ",25))return 4;t.pump(1);globals(t);
		printf("Fresh standalone state: %s\n",t.lcd().c_str());if(t.lcd().find("omni")==std::string::npos)return 5;
	}
	std::filesystem::remove_all(base);
	puts("PASS original OS MIDI Globals survive a fresh standalone instance via its own disk sidecar");return 0;
}
