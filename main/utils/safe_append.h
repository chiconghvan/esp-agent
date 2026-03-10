
#ifndef SAFE_APPEND_H
#define SAFE_APPEND_H

#define APPEND_SNPRINTF(buf, size, offset, ...) do { \
    if ((size_t)(offset) < (size)) { \
        int _n = snprintf((buf) + (offset), (size) - (offset), __VA_ARGS__); \
        if (_n > 0) { \
            (offset) += ((size_t)_n < (size) - (offset)) ? _n : ((size) - (offset) - 1); \
        } \
    } \
} while(0)

#endif // SAFE_APPEND_H
