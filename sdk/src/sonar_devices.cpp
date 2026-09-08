#include "sonar_wasapi.h"
#include <cstring>

namespace {
template<size_t N> void utf8(char (&destination)[N],const std::wstring& text) {
    const int required=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    if((required==0 && !text.empty()) || required>=int(N)) throw sd::Failure(SD_IO_ERROR,"Audio endpoint metadata exceeds the SDK field");
    if(required) WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),int(text.size()),destination,required,nullptr,nullptr);
}
}
extern "C" sd_result SD_CALL sd_sonar_enumerate_audio_devices(sd_sonar_audio_device* output,uint32_t capacity,uint32_t* count) {
    if(!count || (!output && capacity) || capacity>4096) return SD_INVALID_ARGUMENT;
    for(uint32_t i=0;output && i<capacity;++i) if(output[i].struct_size!=sizeof(*output)) return SD_INVALID_ARGUMENT;
    const auto com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    if(FAILED(com) && com!=RPC_E_CHANGED_MODE) return SD_IO_ERROR;
    struct Uninitialize { bool active;~Uninitialize(){if(active) CoUninitialize();} } uninitialize{SUCCEEDED(com)};
    try {
        const auto endpoints=sd::native::SonarEndpoints();
        std::vector<sd_sonar_audio_device> rows;rows.reserve(endpoints.size());
        for(const auto& endpoint:endpoints) {
            sd_sonar_audio_device row{};row.struct_size=sizeof(row);row.flow=endpoint.flow;row.is_default=endpoint.is_default;
            row.form_factor=endpoint.form_factor;row.source_type_hint=endpoint.source_type;utf8(row.endpoint_id,endpoint.id);utf8(row.name,endpoint.name);
            rows.push_back(row);
        }
        *count=uint32_t(rows.size());
        if(!output) return SD_OK;
        if(capacity<rows.size()) return SD_PENDING;
        if(!rows.empty()) std::memcpy(output,rows.data(),rows.size()*sizeof(rows[0]));
        return SD_OK;
    } catch(const sd::Failure& error) { return error.status; }
      catch(const std::bad_alloc&) { return SD_OUT_OF_MEMORY; }
      catch(...) { return SD_INTERNAL_ERROR; }
}
