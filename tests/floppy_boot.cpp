// Boot the unmodified EIII ROM/OS from D0 only, then hot eject/reinsert.
#include "panel_rig.h"
#include "runtime/eiii_runtime.h"
#include <fstream>
using namespace eiii_test;
int main()
{
	auto floppy=env("EIII_FLOPPY");if(floppy.empty() || env("EIII_ROM_DIR").empty())return 77;
	auto cfg=media_config();cfg.rt.hd_path.clear();cfg.rt.cd_path.clear();cfg.rt.floppy_path=floppy;
	FILE *log=nullptr;if(auto p=env("EIII_FLOPPY_LOG");!p.empty())log=fopen(p.c_str(),"w");cfg.rt.log=log;
	int result=0;
	{
		rig t(cfg);
		if(!t.wait_row(0,"P00 ",90)){fprintf(stderr,"D0 boot failed at %.3f: %s\n",t.now(),t.lcd().c_str());result=1;}
		else {
			printf("D0 OS boot at %.3f: %s\n",t.now(),t.lcd().c_str());
			t.pump(2);t.press("master");
			if(!t.wait([&]{return t.lcd().find("Wait...")==std::string::npos;},30))result=2;
			t.press("keypad_8");t.pump(1);t.press("keypad_7");
			if(!t.wait_text("MIDI",30))result=2;
			printf("After Globals buttons: %s\n",t.lcd().c_str());
			t.e.set_media(eiii::media_slot::floppy,"");t.pump(.1);
			eiii::media_result r;bool ejected=false;while(t.e.poll_media_result(r))if(r.slot==eiii::media_slot::floppy)ejected=r.ok && !r.restarted;
			if(result==0 && (!ejected || !t.e.media_path(eiii::media_slot::floppy).empty()))result=3;
			t.e.set_media(eiii::media_slot::floppy,floppy);t.pump(.1);bool inserted=false;
			while(t.e.poll_media_result(r))if(r.slot==eiii::media_slot::floppy)inserted=r.ok && !r.restarted;
			if(result==0 && (!inserted || t.e.media_path(eiii::media_slot::floppy)!=floppy))result=4;
			if(auto hd=env("EIII_HD");!hd.empty()) {
				t.e.set_media(eiii::media_slot::hd,hd);t.pump(.1);bool attached=false;
				while(t.e.poll_media_result(r))if(r.slot==eiii::media_slot::scsi && r.scsi_id==1)attached=r.ok && r.restarted;
				if(!attached || !t.wait_row(0,"P00 ",40))result=5;
				t.e.set_media(eiii::media_slot::hd,"");t.pump(.1);bool disconnected=false;
				while(t.e.poll_media_result(r))if(r.slot==eiii::media_slot::scsi && r.scsi_id==1)disconnected=r.ok && r.restarted;
				if(!disconnected || !t.e.media_path(eiii::media_slot::hd).empty() || !t.wait_row(0,"P00 ",40))result=6;
				printf("D1 disconnect/reboot: %s\n",t.lcd().c_str());
			}

		}
	}
	if(log)fclose(log);
	if(!result)puts("PASS original OS boots from write-protected D0; MIDI menu works; hot eject/reinsert preserves runtime");
	return result;
}
