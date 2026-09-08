#include "spacedesk_settings.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

int main(int argc,char** argv) {
    try {
        sd::SASettings settings;
        if(argc==1) {
            for(uint32_t i=0;i<sd_settings_count();++i) {
                sd_setting_info info{};info.struct_size=sizeof(info);
                if(sd_settings_describe(i,&info)!=SD_OK) return 1;
                std::cout<<info.key<<" = "<<(info.type==SD_SETTING_TEXT?settings.GetText(info.key):std::to_string(settings.GetInteger(info.key)))
                         <<" ["<<(info.application&SD_SETTING_PENDING?"pending":info.application&SD_SETTING_FRONTEND?"frontend":"engine")<<"]\n";
            }
            return 0;
        }
        if(argc!=3) { std::cerr<<"Usage: sd_settings.exe [--export file | --validate file]\n";return 2; }
        if(std::string(argv[1])=="--export") {
            std::ofstream file(argv[2],std::ios::binary);file<<settings.Serialize();
            if(!file) return 1;
        } else if(std::string(argv[1])=="--validate") {
            std::ifstream file(argv[2],std::ios::binary);
            if(!file) return 1;
            const std::string data{std::istreambuf_iterator<char>(file),{}};
            settings.Deserialize(data);
            sd_config config{};sd_config_init(&config);sd_client* client=nullptr;
            if(sd_client_create(&config,&client)!=SD_OK) return 1;
            const auto result=settings.Apply(client);
            if(result!=SD_OK) std::cerr<<sd_client_last_error(client)<<'\n';
            sd_client_destroy(client);
            if(result!=SD_OK) return 1;
            std::cout<<"Valid settings; no connection opened. Frontend preferences require consumer support.\n";
        } else return 2;
        return 0;
    } catch(const sd::SettingsError& e) { std::cerr<<e.what()<<": "<<e.result<<'\n';return 1; }
}
