#pragma once
#include <memory>
#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

// Large bookmark lists live in PSRAM, leaving internal RAM for radio/RTOS.
template<class T> struct ReaderStateAllocator {
    using value_type=T;
    ReaderStateAllocator()=default;
    template<class U> ReaderStateAllocator(const ReaderStateAllocator<U>&) {}
    T *allocate(size_t n) {
#ifdef ARDUINO
        if(void *p=heap_caps_malloc(n*sizeof(T),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT))return static_cast<T*>(p);
#endif
        return std::allocator<T>{}.allocate(n);
    }
    void deallocate(T *p,size_t n) {
#ifdef ARDUINO
        (void)n;heap_caps_free(p);
#else
        std::allocator<T>{}.deallocate(p,n);
#endif
    }
    template<class U> bool operator==(const ReaderStateAllocator<U>&)const{return true;}
    template<class U> bool operator!=(const ReaderStateAllocator<U>&)const{return false;}
};
