#pragma once
#include "sample_support.h"
#include <array>

namespace sample {
static void SaveBmp(const std::wstring& path,const sd_frame& frame) {
    if(frame.pixel_format!=SD_PIXEL_BGRA8 || !frame.pixels) throw std::runtime_error("Expected a BGRA8 SDK frame");
    BITMAPFILEHEADER file{};file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
    file.bfSize=file.bfOffBits+frame.width*frame.height*4;
    BITMAPINFOHEADER bitmap{};bitmap.biSize=sizeof(bitmap);bitmap.biWidth=LONG(frame.width);bitmap.biHeight=-LONG(frame.height);
    bitmap.biPlanes=1;bitmap.biBitCount=32;bitmap.biCompression=BI_RGB;bitmap.biSizeImage=frame.width*frame.height*4;
    sample::File output(path);output.Write(&file,sizeof(file));output.Write(&bitmap,sizeof(bitmap));
    for(uint32_t y=0;y<frame.height;++y) output.Write(frame.pixels+size_t(y)*frame.stride,frame.width*4);
}
class WaveFile {
    sample::File file_;uint32_t bytes_=0;
    void Header() {
        std::array<uint8_t,44> h{};
        const auto tag=[&](size_t offset,const char* text) { for(size_t i=0;i<4;++i) h[offset+i]=uint8_t(text[i]); };
        const auto put16=[&](size_t offset,uint16_t v) { h[offset]=uint8_t(v);h[offset+1]=uint8_t(v>>8); };
        const auto put32=[&](size_t offset,uint32_t v) { for(size_t i=0;i<4;++i) h[offset+i]=uint8_t(v>>(8*i)); };
        tag(0,"RIFF");put32(4,36+bytes_);tag(8,"WAVE");tag(12,"fmt ");put32(16,16);put16(20,1);put16(22,2);
        put32(24,48000);put32(28,192000);put16(32,4);put16(34,16);tag(36,"data");put32(40,bytes_);
        file_.Seek(0);file_.Write(h.data(),uint32_t(h.size()));
    }
public:
    explicit WaveFile(const std::wstring& path):file_(path) { Header(); }
    ~WaveFile() { try { Header(); } catch(...) {} }
    void Write(const int16_t* pcm,uint32_t frames) { file_.Write(pcm,frames*4);bytes_+=frames*4; }
    void Finish() { Header(); }
};
}
