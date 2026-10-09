#pragma once
#include <stdint.h>
#include <stddef.h>
inline uint32_t esp_crc32_le(uint32_t crc,const uint8_t *data,size_t length) {
    crc=~crc;
    for(size_t i=0;i<length;++i) {
        crc^=data[i];
        for(int bit=0;bit<8;++bit)crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return ~crc;
}
