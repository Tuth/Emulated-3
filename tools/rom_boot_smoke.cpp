// ROM-only boot through the same threaded engine and panel adapter used by the standalone GUI.
#include "tests/panel_rig.h"
#include "ui/panel_view.h"
int main(int argc, char **argv) {
    if(argc<2) {fprintf(stderr,"usage: rom_boot_smoke ROM_DIR [PANEL.png]\n");return 2;}
    auto cfg=eiii_test::media_config();cfg.rt.rom_dir=argv[1];cfg.rt.hd_path.clear();cfg.rt.cd_path.clear();cfg.rt.rom_only=true;
    std::string error;
    if(!eiii::runtime::check_rom_dir(cfg.rt.rom_dir,error)){fprintf(stderr,"%s\n",error.c_str());return 1;}
    eiii_test::rig t(cfg);t.pump(10);t.ui.poll();
    auto lcd=t.lcd();printf("%s\n",lcd.c_str());
    bool ok=lcd.find("EMULATOR")!=std::string::npos && lcd.find("v2.00")!=std::string::npos && lcd.find("Checking Floppy")!=std::string::npos;
    if(argc>2){eiii::ui::panel_view view;int w,h;view.preferred_size(100,w,h);view.set_size(w,h);eiii::ui::canvas frame;view.render(frame,t.ui,true);if(!frame.write_png(argv[2]))return 1;}
    printf("%s: original v2.00 firmware reaches floppy check without OS media\n",ok?"PASS":"FAIL");return ok?0:1;
}
