// Render the actual standalone menus without an OS window; real engine/firmware snapshot.
#include "tests/panel_rig.h"
#include "standalone/gui_menu.h"
#include <cstdio>
int main(int argc,char **argv)
{
	using namespace eiii_test;
	rig t(media_config());if(!t.wait_row(0,"P00 ",25))return 1;
	gui_options cfg;cfg.rom_dir=env("EIII_ROM_DIR");cfg.hd=env("EIII_HD");
	eiii::ui::panel_window win(t.ui);
	standalone_menus(win,t.e,cfg,[](bool,std::string){},[]{});
	for(auto id:{"settings","audio","midi","d1","d3"})if(win.view().find_menu(id)<0)return 2;
	win.view().set_size(1472,564);eiii::ui::canvas frame;win.view().render(frame,t.ui,true);
	win.view().open_menu(win.view().find_menu("settings"));win.view().render(frame,t.ui,true);
	if(argc>1 && !frame.write_png(argv[1]))return 3;
	puts("PASS standalone menus render above the original OS panel");return 0;
}
