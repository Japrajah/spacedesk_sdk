#include "text_input.h"
#include <memory>

namespace sd::viewer {
namespace {
constexpr wchar_t class_name[]=L"SpacedeskSdkTextInput";
enum : int { Edit=3101, Reset=3102, Hint=3103 };
constexpr UINT text_changed=WM_APP+0x311;
struct Panel {
    HWND window=nullptr,edit=nullptr,reset=nullptr,close=nullptr,hint=nullptr;
    TextEditSender send;
    OskHistory history;
    bool changing=false,show_hint=true,pending=false;
};
std::unique_ptr<Panel> panel;
void Layout(Panel& p) {
    RECT area{};GetClientRect(p.window,&area);
    const UINT dpi=GetDpiForWindow(p.window);const int margin=MulDiv(12,dpi?dpi:96,96);
    const int button=MulDiv(28,dpi?dpi:96,96),width=MulDiv(140,dpi?dpi:96,96);
    const int top=p.show_hint?margin+button:margin;
    MoveWindow(p.hint,margin,margin,area.right-2*margin,button,TRUE);
    MoveWindow(p.edit,margin,top,area.right-2*margin,std::max(1L,area.bottom-top-button-3*margin),TRUE);
    MoveWindow(p.reset,margin,area.bottom-button-margin,width,button,TRUE);
    MoveWindow(p.close,area.right-width-margin,area.bottom-button-margin,width,button,TRUE);
}
void ResetHistory(Panel& p) {
    p.changing=true;p.pending=false;p.history.Reset();SetWindowTextW(p.edit,L"");p.changing=false;
}
LRESULT CALLBACK Procedure(HWND window,UINT message,WPARAM wp,LPARAM lp) {
    auto* p=reinterpret_cast<Panel*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE) {
        p=static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        p->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,LONG_PTR(p));
    }
    if(!p) return DefWindowProcW(window,message,wp,lp);
    switch(message) {
    case WM_CREATE: {
        const auto instance=GetModuleHandleW(nullptr);
        p->hint=CreateWindowW(L"STATIC",L"Edits here update text at the remote cursor.",WS_CHILD|(p->show_hint?WS_VISIBLE:0),
            0,0,0,0,window,HMENU(Hint),instance,nullptr);
        p->edit=CreateWindowExW(WS_EX_CLIENTEDGE,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|
            ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN,0,0,0,0,window,HMENU(Edit),instance,nullptr);
        p->reset=CreateWindowW(L"BUTTON",L"Clear local history",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            0,0,0,0,window,HMENU(Reset),instance,nullptr);
        p->close=CreateWindowW(L"BUTTON",L"Close",WS_CHILD|WS_VISIBLE|WS_TABSTOP,
            0,0,0,0,window,HMENU(IDCANCEL),instance,nullptr);
        if(!p->hint||!p->edit||!p->reset||!p->close) return -1;
        for(const auto child:{p->hint,p->edit,p->reset,p->close})
            SendMessageW(child,WM_SETFONT,WPARAM(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
        SendMessageW(p->edit,EM_SETLIMITTEXT,4096,0);Layout(*p);return 0;
    }
    case WM_SIZE: Layout(*p);return 0;
    case WM_GETMINMAXINFO: {
        const UINT dpi=GetDpiForWindow(window);
        auto* size=reinterpret_cast<MINMAXINFO*>(lp);
        size->ptMinTrackSize={MulDiv(420,dpi?dpi:96,96),MulDiv(220,dpi?dpi:96,96)};return 0;
    }
    case WM_SETFOCUS: SetFocus(p->edit);return 0;
    case WM_INPUTLANGCHANGE:
        // T1.beginBatchEdit resets editable/history when the input method changes.
        ResetHistory(*p);break;
    case WM_COMMAND:
        if(LOWORD(wp)==IDCANCEL) { CloseTextInput();return 0; }
        if(LOWORD(wp)==Reset) { ResetHistory(*p);SetFocus(p->edit);return 0; }
        if(LOWORD(wp)==Edit && HIWORD(wp)==EN_CHANGE && !p->changing && !p->pending) {
            // An EDIT replacement emits delete and insert notifications within
            // one operation. Process its final editable snapshot as one batch.
            p->pending=PostMessageW(window,text_changed,0,0)!=0;return 0;
        }
        break;
    case text_changed:
        if(p->pending) {
            p->pending=false;
            try {
                const int count=GetWindowTextLengthW(p->edit);
                if(count>4096) return 0;
                std::wstring text(size_t(count)+1,L'\0');GetWindowTextW(p->edit,text.data(),count+1);text.resize(count);
                const auto result=p->history.Update(NormalizeEditText(text),p->send);
                if(result<0 || result==SD_DISCONNECTED) {
                    EnableWindow(p->edit,FALSE);SetWindowTextW(window,L"Text input stopped - reconnect to continue");
                }
            } catch(...) { EnableWindow(p->edit,FALSE);SetWindowTextW(window,L"Text input stopped"); }
            return 0;
        }
        return 0;
    case WM_CLOSE: CloseTextInput();return 0;
    case WM_NCDESTROY: p->window=nullptr;break;
    }
    return DefWindowProcW(window,message,wp,lp);
}
}
HWND ShowTextInput(HWND owner,TextEditSender send,bool hint,bool visible) {
    if(panel && panel->window) {
        if(visible) { ShowWindow(panel->window,SW_SHOWNORMAL);SetForegroundWindow(panel->window); }
        return panel->window;
    }
    auto next=std::make_unique<Panel>();next->send=std::move(send);next->show_hint=hint;
    WNDCLASSW cls{};cls.hInstance=GetModuleHandleW(nullptr);cls.lpfnWndProc=Procedure;
    cls.hCursor=LoadCursorW(nullptr,IDC_IBEAM);cls.hbrBackground=HBRUSH(COLOR_BTNFACE+1);cls.lpszClassName=class_name;
    if(!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    const auto window=CreateWindowExW(WS_EX_CONTROLPARENT,class_name,L"Remote text input",WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,CW_USEDEFAULT,600,280,owner,nullptr,cls.hInstance,next.get());
    if(!window) return nullptr;
    panel=std::move(next);
    if(visible) { ShowWindow(window,SW_SHOWNORMAL);SetFocus(panel->edit); }
    return window;
}
void CloseTextInput() noexcept {
    if(panel && panel->window) DestroyWindow(panel->window);
    panel.reset();
}
bool TextInputMessage(MSG* message) noexcept {
    return panel && panel->window && (message->hwnd==panel->window || IsChild(panel->window,message->hwnd)) &&
        IsDialogMessageW(panel->window,message)!=0;
}
}
