#pragma once
#include <string>
namespace sd {
// Repair compact identities produced by older SDK constructors without
// changing their 128-bit value. Other explicitly supplied names remain opaque.
template<class Char> std::basic_string<Char> CanonicalClientId(const std::basic_string<Char>& value) {
    const auto hex=[](Char c) { return (c>=Char('0')&&c<=Char('9')) || (c>=Char('a')&&c<=Char('f')) || (c>=Char('A')&&c<=Char('F')); };
    std::basic_string<Char> compact;
    if(value.size()==32) {
        for(Char c:value) { if(!hex(c)) return value;compact+=c; }
    } else if(value.size()==36) {
        for(size_t i=0;i<value.size();++i) {
            if(i==8 || i==13 || i==18 || i==23) { if(value[i]!=Char('-')) return value; }
            else { if(!hex(value[i])) return value;compact+=value[i]; }
        }
    } else return value;
    std::basic_string<Char> result(1,Char('{'));
    for(size_t i=0;i<compact.size();++i) {
        if(i==8 || i==12 || i==16 || i==20) result+=Char('-');
        result+=compact[i];
    }
    result+=Char('}');return result;
}
}
