#pragma once
#include <map>
#include <string>
#include <vector>
#include <cstring>
namespace FakeNvs {
inline std::map<std::string,std::vector<unsigned char>> data;
inline bool failWrite = false;
}
class Preferences {
public:
    bool begin(const char *,bool) { return true; }
    void end() {}
    size_t getBytesLength(const char *key) { auto i=FakeNvs::data.find(key); return i==FakeNvs::data.end()?0:i->second.size(); }
    size_t getBytes(const char *key,void *out,size_t size) {
        auto i=FakeNvs::data.find(key);
        if(i==FakeNvs::data.end() || size<i->second.size()) return 0;
        memcpy(out,i->second.data(),i->second.size());return i->second.size();
    }
    size_t putBytes(const char *key,const void *value,size_t size) {
        if(FakeNvs::failWrite)return 0;
        const auto *p=static_cast<const unsigned char *>(value);
        FakeNvs::data[key]={p,p+size};return size;
    }
    bool isKey(const char *key) { return FakeNvs::data.count(key); }
    bool remove(const char *key) { return FakeNvs::data.erase(key); }
};
