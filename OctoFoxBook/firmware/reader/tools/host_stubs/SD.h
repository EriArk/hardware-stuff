#pragma once
#include <cstdint>
#include <map>
#include <string>

constexpr const char *FILE_READ = "r";
constexpr const char *FILE_WRITE = "w";

class File {
public:
    explicit File(std::string *data = nullptr) : data_(data) {}
    explicit operator bool() const { return data_ != nullptr; }
    bool isDirectory() const { return false; }
    File openNextFile(const char *) { return File(); }
    const char *name() const { return ""; }
    size_t size() const { return data_ ? data_->size() : 0; }
    int read() { return data_ && position_ < data_->size() ? (*data_)[position_++] : -1; }
    size_t readBytes(char *out, size_t count) {
        size_t n=0; while(n<count && data_ && position_<data_->size())out[n++]=(*data_)[position_++];return n;
    }
    size_t write(uint8_t byte) { return write(&byte,1); }
    size_t write(const uint8_t *text, size_t size) {
        if (!data_) return 0;
        data_->append(reinterpret_cast<const char *>(text), size);
        return size;
    }
    void flush() {}
    void close() {}
private:
    std::string *data_;
    size_t position_ = 0;
};

class FakeSD {
public:
    std::map<std::string, std::string> files;
    std::string failingRename;
    bool exists(const char *path) const { return files.count(path) != 0; }
    bool remove(const char *path) { return files.erase(path) != 0; }
    bool rename(const char *from, const char *to) {
        if (failingRename == from || !exists(from) || exists(to)) return false;
        files[to] = files.at(from);
        files.erase(from);
        return true;
    }
    File open(const char *path, const char *mode) {
        if (std::string(mode) == FILE_WRITE) return File(&files[path]);
        return exists(path) ? File(&files.at(path)) : File();
    }
    bool mkdir(const char *) { return true; }
    bool rmdir(const char *) { return true; }
};
extern FakeSD SD;
