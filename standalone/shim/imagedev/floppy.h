// EIII read-only 3.5-inch DD drive adapter for the unmodified MAME WD1772.
// Supplies rotating flux, index, head stepping and active-low drive signals.
#pragma once
#include "emu.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

class floppy_image_device : public device_t
{
public:
	struct line_cb {
		std::function<void(floppy_image_device *, int)> f;
		line_cb() = default;
		template<class C> line_cb(void (C::*fn)(floppy_image_device *,int), C *p) : f([p,fn](auto d,int s){(p->*fn)(d,s);}) {}
		void operator()(floppy_image_device *d,int s) { if(f)f(d,s); }
	};
	using index_pulse_cb = line_cb;
	using ready_cb = line_cb;
	inline static const device_type_impl type_id{"eiii_floppy", "EIII read-only DD drive"};
	floppy_image_device(machine_config const &c) : device_t(c,type_id,"floppy",nullptr,0) {}
	void setup_index_pulse_cb(index_pulse_cb c) { index_cb=std::move(c); }
	void setup_ready_cb(ready_cb c) { rdy_cb=std::move(c); }
	int ready_r() const { return !loaded || !motor; }
	int trk00_r() const { return cylinder != 0; }
	int wpt_r() const { return loaded; } // all images deliberately write protected
	int idx_r() const { return index; }
	void dir_w(int s) { direction=s; }
	void stp_w(int s) { if(step && !s)cylinder=std::clamp(cylinder+(direction?-1:1),0,83); step=s; }
	void ss_w(int s) { side=s?1:0; }
	void mon_w(int s) {
		bool on=!s; if(on==motor)return;
		int prev=ready_r();motor=on;
		if(index){index=false;index_cb(this,0);}
		if(index_timer) {
			if(motor && loaded){origin=machine().time();index_timer->adjust(attotime::zero);}
			else index_timer->adjust(attotime::never);
		}
		if(prev!=ready_r())rdy_cb(this,ready_r());
	}
	attotime time_next_index() const {
		if(!motor || !loaded)return attotime::never;
		auto now=machine().time();auto phase=(now-origin).m_seconds;
		return origin+attotime((std::floor(phase/revolution)+1)*revolution);
	}
	attotime get_next_transition(attotime const &after) const {
		if(!motor || !loaded || cylinder>=80)return attotime::never;
		auto const &v=tracks[cylinder*2+side];if(v.empty())return attotime::never;
		long double dt=(after-origin).m_seconds, rev=std::floor(dt/revolution), phase=dt-rev*revolution;
		auto it=std::upper_bound(v.begin(),v.end(),phase);
		if(it==v.end()){++rev;it=v.begin();}
		return origin+attotime(rev*revolution+*it);
	}
	// The WD1772 observes WPT before starting a write; no writable flux is exposed.
	void write_start(attotime const &) {}
	void write_end(attotime const &) {}
	void write_flux_change(attotime const &) {}
	void write_flush(attotime const &) {}
	void eject() {
		int prev=ready_r();loaded=false;tracks.clear();
		if(index_timer)index_timer->adjust(attotime::never);
		if(index){index=false;index_cb(this,0);}
		if(prev!=ready_r())rdy_cb(this,ready_r());
	}
	bool load(std::string const &path,std::string &error) {
		std::ifstream in(path,std::ios::binary);if(!in){error="cannot open floppy image";return false;}
		in.seekg(0,std::ios::end);auto size=in.tellg();if(size<=0 || size>4*1024*1024){error="invalid floppy image size";return false;}in.seekg(0);
		std::vector<u8>b(static_cast<size_t>(size));in.read(reinterpret_cast<char*>(b.data()),b.size());if(!in){error="cannot read floppy image";return false;}
		std::vector<std::vector<long double>> candidate(160);
		bool hfe=b.size()>=512 && !memcmp(b.data(),"HXCPICFE",8);
		if(hfe) {
			if(b[8]!=0 || b[9]!=80 || b[10]!=2 || b[11]!=0 || (b[12]|(b[13]<<8))!=250 || (b[22]==0 && b[23]!=0) || (b[24]==0 && b[25]!=0)){error="supported HFE: v1, 80 tracks, 2 sides, 250 kbit/s MFM";return false;}
			unsigned table=(b[18]|(b[19]<<8))*512;if(table+320>b.size()){error="truncated HFE track table";return false;}
			for(unsigned t=0;t<80;++t) {
				unsigned p=table+t*4,off=(b[p]|(b[p+1]<<8))*512,n=b[p+2]|(b[p+3]<<8),blocks=(n+511)/512;
				if(n<20000 || n>30000 || off+blocks*512>b.size()){error="invalid HFE track extent";return false;}
				for(unsigned s=0;s<2;++s) {
					std::vector<u8> bits;
					for(unsigned j=0;j<n/2;++j){unsigned q=off+(j/256)*512+s*256+j%256;for(int k=0;k<8;++k)bits.push_back((b[q]>>k)&1);}
					flux(bits,candidate[t*2+s]);
				}
			}
		} else {
			if(b.size()!=819200){error="raw EIII floppy images must be 819200 bytes (80 x 2 x 10 x 512); E3O is not a disk image";return false;}
			for(unsigned t=0;t<80;++t)for(unsigned s=0;s<2;++s) {
				std::vector<u8> bits;bool previous=false;
				auto raw=[&](u16 w){for(int k=15;k>=0;--k)bits.push_back((w>>k)&1);previous=w&1;};
				auto byte=[&](u8 d){u16 w=0;for(int k=7;k>=0;--k){bool bit=(d>>k)&1;w=(w<<2)|((!previous && !bit)?2:0)|bit;previous=bit;}raw(w);};
				auto repeat=[&](u8 d,int n){while(n--)byte(d);};
				auto field=[&](std::vector<u8> const &data){repeat(0,12);raw(0x4489);raw(0x4489);raw(0x4489);u16 c=0xffff;for(u8 x:{u8(0xa1),u8(0xa1),u8(0xa1)})c=crc(c,x);for(u8 x:data){byte(x);c=crc(c,x);}byte(c>>8);byte(c);};
				repeat(0x4e,80);
				for(unsigned r=1;r<=10;++r) {
					field({0xfe,u8(t),u8(s),u8(r),2});repeat(0x4e,22);
					std::vector<u8>d{0xfb};auto p=b.begin()+((t*2+s)*10+r-1)*512;d.insert(d.end(),p,p+512);field(d);repeat(0x4e,24);
				}
				while(bits.size()<100000)byte(0x4e);
				flux(bits,candidate[t*2+s]);
			}
		}
		int prev=ready_r();tracks=std::move(candidate);loaded=true;
		if(index_timer && motor){if(index){index=false;index_cb(this,0);}origin=machine().time();index_timer->adjust(attotime::zero);}
		if(prev!=ready_r())rdy_cb(this,ready_r());
		return true;
	}
protected:
	void device_start() override { index_timer=timer_alloc(FUNC(floppy_image_device::index_tick),this); }
	void device_reset() override { cylinder=side=direction=0;step=true;motor=false;index=false;index_timer->adjust(attotime::never); }
private:
	static constexpr long double revolution=0.2L;
	bool loaded=false,motor=false,index=false,step=true;
	int cylinder=0,side=0,direction=0;
	attotime origin;
	emu_timer *index_timer=nullptr;
	index_pulse_cb index_cb;ready_cb rdy_cb;
	std::vector<std::vector<long double>> tracks;
	static u16 crc(u16 c,u8 x){c^=u16(x)<<8;for(int k=0;k<8;++k)c=(c&0x8000)?(c<<1)^0x1021:c<<1;return c;}
	static void flux(std::vector<u8> const &bits,std::vector<long double> &out){for(size_t k=0;k<bits.size();++k)if(bits[k])out.push_back((k+0.5L)*revolution/bits.size());}
	TIMER_CALLBACK_MEMBER(index_tick) {
		if(!motor || !loaded)return;
		index=!index;index_cb(this,index);
		index_timer->adjust(index?attotime::from_msec(2):attotime::from_msec(198));
	}
};
