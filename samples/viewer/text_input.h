#pragma once
#include <windows.h>
#include "spacedesk_sdk.h"
#include <algorithm>
#include <functional>
#include <string>
#include <utility>

namespace sd::viewer {
using TextEditSender = std::function<sd_result(const std::u16string&, uint32_t)>;
// UI history from APK T1.PrivateProcessOskInput. The Android BaseInputConnection
// itself is platform UI and is not invented as a C++ SDK base class.
class OskHistory {
    std::u16string previous_;
public:
    void Reset() { previous_.clear(); }
    const std::u16string& Previous() const noexcept { return previous_; }
    sd_result Update(std::u16string current, const TextEditSender& send) {
        if(current==previous_) return SD_IGNORED;
        size_t prefix=0;
        while(prefix<std::min(current.size(),previous_.size()) && current[prefix]==previous_[prefix]) ++prefix;
        const auto result=send(current.substr(prefix),uint32_t(previous_.size()-prefix));
        if(result>=0) previous_=std::move(current);
        return result;
    }
};
inline std::u16string NormalizeEditText(const std::wstring& value) {
    std::u16string result;result.reserve(value.size());
    for(size_t i=0;i<value.size();++i) {
        if(value[i]=='\r' && i+1<value.size() && value[i+1]=='\n') continue;
        result.push_back(char16_t(value[i]));
    }
    return result;
}
// visible=false is used by the hidden-window integration harness.
HWND ShowTextInput(HWND owner, TextEditSender send, bool show_hint=true, bool visible=true);
void CloseTextInput() noexcept;
bool TextInputMessage(MSG* message) noexcept;
}
