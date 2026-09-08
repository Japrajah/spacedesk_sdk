#pragma once
#include <windows.h>
#include <cstring>
#include "spacedesk_sdk.h"
namespace sd::viewer {
// GDI resource ownership belongs to the frontend. The cached bitmap contains
// source pixels; PlgBlt applies the same quarter turns as APK Q2 UV coordinates.
class GdiFrameSurface {
    HDC dc_=nullptr;HBITMAP bitmap_=nullptr;HGDIOBJ previous_=nullptr;void* pixels_=nullptr;
    uint32_t width_=0,height_=0;uint64_t sequence_=~uint64_t(0);
public:
    GdiFrameSurface()=default;
    GdiFrameSurface(const GdiFrameSurface&)=delete;
    GdiFrameSurface& operator=(const GdiFrameSurface&)=delete;
    ~GdiFrameSurface() { Reset(); }
    void Reset() noexcept {
        if(dc_ && previous_) SelectObject(dc_,previous_);
        if(bitmap_) DeleteObject(bitmap_);
        if(dc_) DeleteDC(dc_);
        dc_=nullptr;bitmap_=nullptr;previous_=nullptr;pixels_=nullptr;width_=height_=0;sequence_=~uint64_t(0);
    }
    bool Draw(HDC destination,const RECT& rect,const sd_frame& frame,uint32_t orientation) {
        if(!frame.pixels || !frame.width || !frame.height || orientation<1 || orientation>4) return false;
        if(width_!=frame.width || height_!=frame.height) {
            Reset();dc_=CreateCompatibleDC(nullptr);if(!dc_) return false;
            BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth=LONG(frame.width);info.bmiHeader.biHeight=-LONG(frame.height);
            info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
            bitmap_=CreateDIBSection(dc_,&info,DIB_RGB_COLORS,&pixels_,nullptr,0);
            if(!bitmap_) { Reset();return false; }
            previous_=SelectObject(dc_,bitmap_);
            if(!previous_ || previous_==HGDI_ERROR) { previous_=nullptr;Reset();return false; }
            width_=frame.width;height_=frame.height;
        }
        if(sequence_!=frame.sequence) {
            for(uint32_t y=0;y<height_;++y)
                std::memcpy(static_cast<uint8_t*>(pixels_)+size_t(y)*width_*4,frame.pixels+size_t(y)*frame.stride,size_t(width_)*4);
            sequence_=frame.sequence;
        }
        const POINT tl{rect.left,rect.top},tr{rect.right,rect.top},bl{rect.left,rect.bottom},br{rect.right,rect.bottom};
        POINT points[3];
        switch(orientation) {
        case 2:points[0]=tr;points[1]=br;points[2]=tl;break;
        case 3:points[0]=br;points[1]=bl;points[2]=tr;break;
        case 4:points[0]=bl;points[1]=tl;points[2]=br;break;
        default:points[0]=tl;points[1]=tr;points[2]=bl;break;
        }
        SetStretchBltMode(destination,COLORONCOLOR);
        return PlgBlt(destination,points,dc_,0,0,int(width_),int(height_),nullptr,0,0)!=FALSE;
    }
};
}
