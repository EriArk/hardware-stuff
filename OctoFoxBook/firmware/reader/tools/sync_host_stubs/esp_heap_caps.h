#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_SPIRAM=0,MALLOC_CAP_8BIT=0;
inline void *heap_caps_malloc(size_t n,int){return malloc(n);}
inline void *heap_caps_realloc(void *p,size_t n,int){return realloc(p,n);}
inline void heap_caps_free(void *p){free(p);}
