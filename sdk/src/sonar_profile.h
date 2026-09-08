#pragma once
#include <memory>
#include <string>
#include <string_view>

namespace sd::native {
// Private profile reader: JSONObject parsing/coercion used by APK V0.g.
// This is an SDK adapter name, not an original APK class name.
class SonarProfileDocument {
    struct Data;
    std::shared_ptr<const Data> data_;
public:
    explicit SonarProfileDocument(std::string_view json);
    bool Has(std::string_view key) const;
    double Double(std::string_view key) const;
};
}
