#pragma once
#include "eiii_gui.h"
#include "native_path.h"
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>

inline std::string standalone_storage_path(std::string const &name, bool create = false)
{
	std::string dir;
#ifdef _WIN32
	if (auto p = std::getenv("APPDATA")) dir = p;
#else
	if (auto p = std::getenv("XDG_CONFIG_HOME")) dir = p;
	else if (auto p = std::getenv("HOME")) dir = std::string(p) + "/.config";
#endif
	if (dir.empty()) return {};
	dir += "/eiii";
	if (create && !eiii::create_native_directory(dir)) return {};
	return dir + "/" + name;
}
inline bool standalone_write(std::string const &name, std::string const &bytes)
{
	auto p = standalone_storage_path(name, true); if (p.empty()) return false;
	std::ofstream out(p + ".tmp", std::ios::binary); out.write(bytes.data(), bytes.size()); out.close();
	if (!out) return false;
#ifdef _WIN32
	return MoveFileExA((p + ".tmp").c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
	return std::rename((p + ".tmp").c_str(), p.c_str()) == 0;
#endif
}
inline void load_gui_defaults(gui_options &o)
{
#ifdef _WIN32
	o.audio_out = "default";
#endif
	std::ifstream in(standalone_storage_path("eiii-standalone.conf"));
	for (std::string line; std::getline(in, line);)
	{
		if (!line.empty() && line.back() == '\r') line.pop_back();
		auto pos = line.find('='); if (pos == std::string::npos) continue;
		auto k = line.substr(0,pos), v = line.substr(pos+1);
		if (k == "rom_dir") o.rom_dir = v;
		else if (k == "hd") o.hd = v;
		else if (k == "cd") o.cd = v;
		else if (k == "cem_model" && (v == "v1" || v == "v2")) o.cem_model = v;
		else if (k == "midi_in") o.midi_in = v;
		else if (k == "audio_out") o.audio_out = v;
		else if (k == "view_width") { int n=0; auto r=std::from_chars(v.data(),v.data()+v.size(),n); if(r.ec==std::errc{} && r.ptr==v.data()+v.size() && n>=736 && n<=2944)o.view_width=n; }
	}
}
inline bool save_gui_defaults(gui_options const &o)
{
	return standalone_write("eiii-standalone.conf", "rom_dir="+o.rom_dir+"\nhd="+o.hd+"\ncd="+o.cd+"\ncem_model="+o.cem_model+"\nmidi_in="+o.midi_in+"\naudio_out="+o.audio_out+"\nview_width="+std::to_string(o.view_width)+"\n");
}
inline std::string standalone_hd_state(unsigned id) { return "eiii-standalone-hd"+std::to_string(id)+".state"; }
inline std::string standalone_read(std::string const &name)
{
	std::ifstream in(standalone_storage_path(name),std::ios::binary);
	in.seekg(0,std::ios::end);auto size=in.tellg();if(size<0 || size>256*1024*1024)return {};in.seekg(0);
	return std::string(std::istreambuf_iterator<char>(in),{});
}
