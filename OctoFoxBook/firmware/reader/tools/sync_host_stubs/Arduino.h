#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
class String:public std::string {
public:
 using std::string::string;
 String(const std::string &s):std::string(s){}
 String substring(size_t n)const{return substr(n);}
 int lastIndexOf(char c)const{auto n=rfind(c);return n==npos?-1:static_cast<int>(n);}
 bool startsWith(const char *s)const{return rfind(s,0)==0;}
 size_t write(uint8_t b){push_back(static_cast<char>(b));return 1;}
 size_t write(const uint8_t *b,size_t n){append(reinterpret_cast<const char*>(b),n);return n;}
};
