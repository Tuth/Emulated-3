// Real MAME WD1772 register/PLL regression over a synthetic EIII disk.
#include "emu.h"
#include "machine/wd_fdc.h"
#include "imagedev/floppy.h"
#include <filesystem>

int main()
{
	shim_scheduler sched;auto previous=g_shim_sched;g_shim_sched=&sched;
	int failures=0;
	auto check=[&](bool ok,char const *what){printf("%s %s\n",ok?"PASS":"FAIL",what);if(!ok)++failures;};
	auto path=std::filesystem::temp_directory_path()/"eiii-floppy-controller.img";
	std::vector<u8> image(819200);for(size_t i=0;i<image.size();++i)image[i]=u8((i/512)*37+i);
	{std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(image.data()),image.size());}
	{
		machine_config cfg;floppy_image_device disk(cfg);wd1772_device fdc(cfg,"fdc",nullptr,8'000'000);
		bool drq=false,irq=false;fdc.drq_wr_callback().set([&](int s){drq=s;});fdc.intrq_wr_callback().set([&](int s){irq=s;});
		fdc.set_disable_motor_control(true);disk.start();fdc.start();disk.reset();fdc.reset();fdc.set_floppy(&disk);fdc.dden_w(0);
		check(fdc.status_r()==4,"empty drive starts on track zero");
		std::string error;check(disk.load(path.string(),error),"mount synthetic 800 KiB floppy");disk.mon_w(0);
		auto run=[&](double seconds,std::vector<u8> *data=nullptr){
			auto end=sched.time()+attotime::from_double(seconds);size_t events=0;
			while(sched.next_expiry()<=end && events++<1000000){sched.fire_due(sched.next_expiry());if(data && drq)data->push_back(fdc.data_r());if(irq)break;}
			if(!irq)sched.fire_due(end);
		};
		fdc.cmd_w(0xd0);run(.001);check(!irq && !(fdc.status_r()&1),"force interrupt D0 clears busy without asserting IRQ");
		for(int track:{0,79}) {
			fdc.data_w(track);fdc.cmd_w(0x18);run(2);check(irq && fdc.track_r()==track,"seek completes with IRQ and track register update");fdc.status_r();
			for(int side=0;side<2;++side)for(int sector:{1,10}) {
				disk.ss_w(side);fdc.sector_w(sector);fdc.cmd_w(0x88);std::vector<u8> data;run(2,&data);
				unsigned status=fdc.status_r();auto start=image.begin()+((track*2+side)*10+sector-1)*512;
				check(irq==false && data.size()==512 && std::equal(data.begin(),data.end(),start) && !(status&0x1c),"sector bytes and CRC correct at both sides and disk boundaries; status read acknowledges IRQ");
			}
		}
		fdc.cmd_w(0xa8);run(2);check(irq && (fdc.status_r()&0x40) && !drq,"write sector is refused by write protection");
		check(!disk.load(path.string()+".missing",error),"invalid mount rejected");
		fdc.sector_w(1);fdc.cmd_w(0x88);std::vector<u8> data;run(2,&data);fdc.status_r();check(data.size()==512,"failed replacement retains the mounted medium");
		disk.eject();check(disk.ready_r()==1,"eject deasserts ready");
	}
	std::filesystem::remove(path);g_shim_sched=previous;
	return failures?1:0;
}
