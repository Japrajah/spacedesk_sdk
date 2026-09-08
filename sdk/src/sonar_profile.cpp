/*
 * Parsing portions derived from Android JSONTokener/JSONObject/JSON.
 * Copyright (C) 2010 The Android Open Source Project
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy at http://www.apache.org/licenses/LICENSE-2.0
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and limitations.
 * Modified: private C++ reader/coercion for SonarPen profiles, without the
 * general JSONObject mutation/serialization API. See third_party/android-json.
 */
#include "sonar_profile.h"
#include <charconv>
#include <cmath>
#include <limits>
#include <locale.h>
#include <map>
#include <regex>
#include <stdexcept>
#include <vector>

namespace sd::native {
namespace {
struct ProfileError:std::runtime_error { ProfileError():std::runtime_error("Invalid SonarPen JSON profile") {} };
enum class Kind { Null,Boolean,Number,String,Object,Array };
struct Value {
    Kind kind=Kind::Null;
    double number=0;
    std::string string;
    std::map<std::string,Value,std::less<>> object;
    std::vector<Value> array;
};
bool equal_ascii(std::string_view a,std::string_view b) {
    if(a.size()!=b.size()) return false;
    for(size_t i=0;i<a.size();++i) { const char c=a[i]>='A'&&a[i]<='Z'?char(a[i]+32):a[i];if(c!=b[i]) return false; }
    return true;
}
double java_double(std::string_view text) {
    while(!text.empty() && uint8_t(text.front())<=32) text.remove_prefix(1);
    while(!text.empty() && uint8_t(text.back())<=32) text.remove_suffix(1);
    std::string_view magnitude=text;
    const bool negative=!magnitude.empty() && magnitude.front()=='-';
    if(!magnitude.empty() && (magnitude.front()=='-' || magnitude.front()=='+')) magnitude.remove_prefix(1);
    if(magnitude=="NaN") return std::numeric_limits<double>::quiet_NaN();
    if(magnitude=="Infinity") return negative?-std::numeric_limits<double>::infinity():std::numeric_limits<double>::infinity();
    static const std::regex decimal(R"([+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?[fFdD]?)");
    static const std::regex hexadecimal(R"([+-]?0[xX](?:[0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?|\.[0-9a-fA-F]+)[pP][+-]?[0-9]+[fFdD]?)");
    const std::string input(text);
    if(!std::regex_match(input,decimal) && !std::regex_match(input,hexadecimal)) throw ProfileError();
    std::string numeric=input;
    if(numeric.back()=='f' || numeric.back()=='F' || numeric.back()=='d' || numeric.back()=='D') numeric.pop_back();
    struct Locale {
        _locale_t value=_create_locale(LC_NUMERIC,"C");
        Locale() { if(!value) throw std::bad_alloc(); }
        ~Locale() { _free_locale(value); }
    };
    static const Locale locale;
    char* end=nullptr;
    const double value=_strtod_l(numeric.c_str(),&end,locale.value);
    if(end!=numeric.c_str()+numeric.size()) throw ProfileError();
    return value;
}
void utf8(std::string& out,uint32_t c) {
    if(c<0x80) out+=char(c);
    else if(c<0x800) { out+=char(0xc0|(c>>6));out+=char(0x80|(c&63)); }
    else if(c<0x10000) { out+=char(0xe0|(c>>12));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63)); }
    else { out+=char(0xf0|(c>>18));out+=char(0x80|((c>>12)&63));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63)); }
}
class Reader {
    std::string_view input_;
    size_t position_=0;
    void LineEnd() { while(position_<input_.size()) { const char c=input_[position_++];if(c=='\n'||c=='\r') break; } }
    int Clean() {
        while(position_<input_.size()) {
            const auto c=uint8_t(input_[position_++]);
            if(c==' ' || c=='\t' || c=='\n' || c=='\r') continue;
            if(c=='#') { LineEnd();continue; }
            if(c=='/' && position_<input_.size()) {
                if(input_[position_]=='/') { ++position_;LineEnd();continue; }
                if(input_[position_]=='*') {
                    const auto end=input_.find("*/",position_+1);
                    if(end==std::string_view::npos) throw ProfileError();
                    position_=end+2;continue;
                }
            }
            return c;
        }
        return -1;
    }
    uint32_t Hex4() {
        if(input_.size()-position_<4) throw ProfileError();
        uint32_t value=0;
        for(int i=0;i<4;++i) {
            const char c=input_[position_++];
            const int digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
            if(digit<0) throw ProfileError();value=value*16+uint32_t(digit);
        }
        return value;
    }
    Value String(char quote) {
        Value out;out.kind=Kind::String;
        while(position_<input_.size()) {
            char c=input_[position_++];if(c==quote) return out;
            if(c!='\\') { out.string+=c;continue; }
            if(position_==input_.size()) throw ProfileError();
            c=input_[position_++];
            switch(c) {
            case 'b':out.string+='\b';break;
            case 'f':out.string+='\f';break;
            case 'n':out.string+='\n';break;
            case 'r':out.string+='\r';break;
            case 't':out.string+='\t';break;
            case 'u': {
                uint32_t code=Hex4();
                if(code>=0xd800 && code<=0xdbff && input_.substr(position_,2)=="\\u") {
                    const size_t before=position_;position_+=2;const auto low=Hex4();
                    if(low>=0xdc00 && low<=0xdfff) code=0x10000+((code-0xd800)<<10)+(low-0xdc00);
                    else position_=before;
                }
                utf8(out.string,code);break;
            }
            default:out.string+=c;break; // Android accepts an unknown escaped character.
            }
        }
        throw ProfileError();
    }
    Value Literal() {
        const size_t first=position_;
        while(position_<input_.size() && std::string_view("{}[]/\\:,=;# \t\f\r\n").find(input_[position_])==std::string_view::npos) ++position_;
        const auto literal=input_.substr(first,position_-first);
        if(literal.empty()) throw ProfileError();
        Value out;
        if(equal_ascii(literal,"null")) return out;
        if(equal_ascii(literal,"true") || equal_ascii(literal,"false")) { out.kind=Kind::Boolean;return out; }
        if(literal.find('.')==std::string_view::npos) {
            auto number=literal;int base=10;
            if(number.substr(0,2)=="0x" || number.substr(0,2)=="0X") { number.remove_prefix(2);base=16; }
            else if(number.size()>1 && number.front()=='0') { number.remove_prefix(1);base=8; }
            if(!number.empty() && number.front()=='+') number.remove_prefix(1);
            int64_t integer=0;
            const auto parsed=std::from_chars(number.data(),number.data()+number.size(),integer,base);
            if(!number.empty() && parsed.ec==std::errc{} && parsed.ptr==number.data()+number.size()) {
                out.kind=Kind::Number;out.number=double(integer);return out;
            }
        }
        try { out.number=java_double(literal);out.kind=Kind::Number;return out; } catch(const ProfileError&) {}
        out.kind=Kind::String;out.string=literal;return out;
    }
    Value Object() {
        Value out;out.kind=Kind::Object;
        const int first=Clean();if(first=='}') return out;if(first!=-1) --position_;
        for(;;) {
            auto key=Next();if(key.kind!=Kind::String) throw ProfileError();
            const int separator=Clean();if(separator!=':' && separator!='=') throw ProfileError();
            if(position_<input_.size() && input_[position_]=='>') ++position_;
            auto value=Next();
            if(value.kind==Kind::Number && !std::isfinite(value.number)) throw ProfileError();
            out.object[std::move(key.string)]=std::move(value);
            const int end=Clean();if(end=='}') return out;if(end!=',' && end!=';') throw ProfileError();
        }
    }
    Value Array() {
        Value out;out.kind=Kind::Array;bool trailing=false;
        for(;;) {
            const int first=Clean();if(first==-1) throw ProfileError();
            if(first==']') { if(trailing) out.array.emplace_back();return out; }
            if(first==',' || first==';') { out.array.emplace_back();trailing=true;continue; }
            --position_;out.array.push_back(Next());
            const int end=Clean();if(end==']') return out;
            if(end!=',' && end!=';') throw ProfileError();trailing=true;
        }
    }
public:
    explicit Reader(std::string_view input):input_(input) { if(input_.substr(0,3)=="\xef\xbb\xbf") position_=3; }
    Value Next() {
        switch(const int c=Clean()) {
        case -1:throw ProfileError();
        case '{':return Object();case '[':return Array();case '\'':return String('\'');case '"':return String('"');
        default:--position_;return Literal();
        }
    }
};
}
struct SonarProfileDocument::Data { Value value; };
SonarProfileDocument::SonarProfileDocument(std::string_view json) {
    auto data=std::make_shared<Data>();data->value=Reader(json).Next();
    if(data->value.kind!=Kind::Object) throw ProfileError();
    data_=std::move(data); // JSONObject does not require EOF after the first object.
}
bool SonarProfileDocument::Has(std::string_view key) const { return data_->value.object.find(key)!=data_->value.object.end(); }
double SonarProfileDocument::Double(std::string_view key) const {
    const auto found=data_->value.object.find(key);if(found==data_->value.object.end()) throw ProfileError();
    const auto& value=found->second;
    if(value.kind==Kind::Number) return value.number;
    if(value.kind==Kind::String) return java_double(value.string);
    throw ProfileError();
}
}
