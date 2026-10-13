// Isolated standalone defaults and binary disk-state persistence; plugin defaults must remain untouched.
#include "standalone/gui_config.h"
#include <filesystem>
#include <cstdio>

int main()
{
	auto base=std::filesystem::temp_directory_path()/"eiii-standalone-config-test";
	std::filesystem::remove_all(base);std::filesystem::create_directories(base);
#ifdef _WIN32
	_putenv_s("APPDATA",base.string().c_str());
#else
	setenv("XDG_CONFIG_HOME",base.string().c_str(),1);
#endif
	auto dir=base/"eiii";std::filesystem::create_directories(dir);
	{std::ofstream out(dir/"eiii-vst3.conf");out<<"rom_dir=PLUGIN-ROM\nhd=PLUGIN-HD\nview_width=736\n";}
	gui_options empty;load_gui_defaults(empty);
	if(!empty.rom_dir.empty() || !empty.hd.empty() || empty.view_width)return 1;
	gui_options saved;saved.rom_dir="Standalone ROM";saved.hd="own.img";saved.cd="bank.iso";saved.midi_in="3";saved.audio_out="2";saved.view_width=2208;saved.cem_model="v2";
	if(!save_gui_defaults(saved))return 2;
	gui_options loaded;load_gui_defaults(loaded);
	if(loaded.rom_dir!=saved.rom_dir || loaded.hd!=saved.hd || loaded.cd!=saved.cd || loaded.midi_in!=saved.midi_in || loaded.audio_out!=saved.audio_out || loaded.view_width!=2208 || loaded.cem_model!="v2")return 3;
	std::string blob("disk\0changes\xff",13);
	if(!standalone_write(standalone_hd_state(1),blob) || standalone_read(standalone_hd_state(1))!=blob)return 4;
	if(standalone_read("eiii-vst3.conf")!="rom_dir=PLUGIN-ROM\nhd=PLUGIN-HD\nview_width=736\n")return 5;
	{std::ofstream out(dir/"eiii-standalone.conf");out<<"view_width=999999\ncem_model=invalid\naudio_out=\n";}
	gui_options invalid;load_gui_defaults(invalid);if(invalid.view_width || invalid.cem_model!="v1" || !invalid.audio_out.empty())return 6;
	std::filesystem::remove_all(base);
	puts("PASS standalone defaults, device choices and binary disk state persist independently of VST3");return 0;
}
