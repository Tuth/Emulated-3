#pragma once
#include "gui_config.h"
#include "ui/panel_window.h"
#include "runtime/eiii_engine.h"
#include <functional>
#include <algorithm>
#include <cmath>
#ifdef _WIN32
#include <commdlg.h>
#include <mmsystem.h>
#endif

inline std::string standalone_pick(eiii::ui::panel_window &win, char const *filter, char const *title, std::string const &current)
{
#ifdef _WIN32
	char path[32768]{}; if(current.size()<sizeof path)std::copy(current.begin(),current.end(),path);
	OPENFILENAMEA of{};of.lStructSize=sizeof of;of.hwndOwner=reinterpret_cast<HWND>(win.native_handle());
	of.lpstrFile=path;of.nMaxFile=sizeof path;of.lpstrFilter=filter;of.lpstrTitle=title;
	of.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
	return GetOpenFileNameA(&of)?path:"";
#else
	(void)win;(void)filter;(void)title;(void)current;return {};
#endif
}
inline void standalone_menus(eiii::ui::panel_window &win,eiii::engine &eng,gui_options &cfg,
	std::function<void(bool,std::string)> request_io,std::function<void()> save,std::function<unsigned()> xruns={})
{
	using namespace eiii::ui;using eiii::media_slot;
	auto change=[&win,&eng](media_slot slot,std::string const &path){if(!eng.set_media(slot,path))win.view().set_menu_status("Request queue is full; retry");};
	auto choose=[&win,&cfg,change](media_slot slot){
		bool rom=slot==media_slot::rom;bool cd=slot==media_slot::cd;bool floppy=slot==media_slot::floppy;
		auto path=standalone_pick(win,floppy?"EIII floppy images\0*.img;*.IMG;*.hfe;*.HFE\0All files\0*.*\0\0":rom?"Boot ROM (*.bin)\0*.bin;*.BIN\0All files\0*.*\0\0":cd?"CD images\0*.iso;*.ISO\0All files\0*.*\0\0":"OS hard disk images\0*.img;*.hd;*.iso;*.hda\0All files\0*.*\0\0",
			floppy?"Insert EIII floppy image (write-protected)":rom?"Choose one boot ROM file":cd?"Insert EIII CD image":"Choose OS hard disk image",floppy?cfg.floppy:rom?"":cd?cfg.cd:cfg.hd);
		if(!path.empty())change(slot,rom?eiii::path_parent(path):path);
	};
	std::vector<menu> menus;
	menus.push_back({"settings",[]{return "Settings";},[&win,&eng,&cfg,choose,change,save]{std::vector<menu_item> v;
		v.push_back({"Boot ROM folder...",false,true,false,false,[choose]{choose(media_slot::rom);}});
		v.push_back({"Save standalone settings",false,true,false,false,save});
		v.push_back({"Restart (cold boot)",false,true,false,false,[change]{change(media_slot::restart,"");}});
		v.push_back(menu_item::sep());
		for(int percent:{50,75,100,125,150,200})v.push_back({"GUI "+std::to_string(percent)+"%",win.width()==1472*percent/100,true,false,false,[&win,&cfg,save,percent]{cfg.view_width=1472*percent/100;win.set_size(cfg.view_width,int(std::lround(cfg.view_width*564.0/1472)));save();}});
		v.push_back(menu_item::sep());
		for(auto model:{"v1","v2"})v.push_back({std::string("Analog model ")+model,cfg.cem_model==model,true,false,false,[change,model]{change(media_slot::cem_model,model);}});
		v.push_back(menu_item::info("Standalone settings are separate from VST3"));return v;}});
	menus.push_back({"audio",[]{return "Audio";},[&cfg,&eng,request_io,xruns]{std::vector<menu_item> v;
		v.push_back({"No audio output",cfg.audio_out.empty(),true,false,false,[request_io]{request_io(true,"");}});
		v.push_back({"System default",cfg.audio_out=="default",true,false,false,[request_io]{request_io(true,"default");}});
#ifdef _WIN32
		for(UINT i=0;i<waveOutGetNumDevs();++i){WAVEOUTCAPSA c{};if(!waveOutGetDevCapsA(i,&c,sizeof c)){auto id=std::to_string(i);v.push_back({eiii::native_display(c.szPname),cfg.audio_out==id,true,false,false,[request_io,id]{request_io(true,id);}});}}
#endif
		v.push_back(menu_item::sep());
		for(unsigned count:{2u,4u,8u}) {
			char label[96];snprintf(label,sizeof label,"%u buffers: %.1f ms output queue",count,cfg.period*count/48.0);
			v.push_back({label,cfg.periods==count,true,false,false,[&cfg,request_io,count]{cfg.periods=count;request_io(true,cfg.audio_out);}});
		}
		char info[160];auto stats=eng.stats();snprintf(info,sizeof info,"Engine queue %.1f ms; underruns %llu; output %u",eng.latency()/48.0,(unsigned long long)stats.underruns,xruns?xruns():0);
		v.push_back(menu_item::info(info));
		v.push_back(menu_item::info("Driver adds latency; fewer buffers reduce delay"));
		v.push_back(menu_item::info("WinMM output; device changes keep the machine running"));return v;}});
	menus.push_back({"midi",[]{return "MIDI In";},[&cfg,request_io]{std::vector<menu_item> v;
		v.push_back({"No MIDI input",cfg.midi_in.empty(),true,false,false,[request_io]{request_io(false,"");}});
#ifdef _WIN32
		for(UINT i=0;i<midiInGetNumDevs();++i){MIDIINCAPSA c{};if(!midiInGetDevCapsA(i,&c,sizeof c)){auto id=std::to_string(i);v.push_back({eiii::native_display(c.szPname),cfg.midi_in==id,true,false,false,[request_io,id]{request_io(false,id);}});}}
#else
		v.push_back({"ALSA virtual",cfg.midi_in=="virtual",true,false,false,[request_io]{request_io(false,"virtual");}});
#endif
		return v;}});
	menus.push_back({"d0",[&cfg]{return std::string("D0 Floppy ")+(cfg.floppy.empty()?"(empty)":eiii::native_display(eiii::path_leaf(cfg.floppy)));},[&cfg,choose,change]{return std::vector<menu_item>{menu_item::info(eiii::native_display(cfg.floppy)),{"Insert floppy image...",false,true,false,false,[choose]{choose(media_slot::floppy);}},{"Eject",false,true,false,false,[change]{change(media_slot::floppy,"");}},menu_item::info("Write protected: EIII IMG / HFE v1")};}});
	menus.push_back({"d1",[&cfg]{return std::string("D1 HD ")+(cfg.hd.empty()?"(choose OS image)":eiii::native_display(eiii::path_leaf(cfg.hd)));},[&cfg,choose,change]{return std::vector<menu_item>{menu_item::info(eiii::native_display(cfg.hd)),{"Choose OS hard disk image...",false,true,false,false,[choose]{choose(media_slot::hd);} },{"Disconnect HD (cold boot)",false,true,false,false,[change]{change(media_slot::hd,"");}},menu_item::info("OS writes persist in a standalone sidecar")};}});
	menus.push_back({"d3",[&cfg]{return std::string("D3 CD ")+(cfg.cd.empty()?"(empty)":eiii::native_display(eiii::path_leaf(cfg.cd)));},[&cfg,choose,change]{return std::vector<menu_item>{menu_item::info(eiii::native_display(cfg.cd)),{"Insert CD image...",false,true,false,false,[choose]{choose(media_slot::cd);}},{"Eject",false,true,false,false,[change]{change(media_slot::cd,"");}}};}});
	win.view().set_menus(std::move(menus));
	win.view().on_drive_click=[&win,change](bool eject,float x,float y){if(eject)change(media_slot::floppy,"");else win.view().open_menu_at(win.view().find_menu("d0"),x,y);};
}
