#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string>
inline uint32_t fakeMillis = 0;
inline uint32_t millis() { return fakeMillis; }
class String : public std::string {
public: using std::string::string;
};
