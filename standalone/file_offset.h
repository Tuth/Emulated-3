// Disk images can exceed LONG_MAX on Windows (LLP64).
#pragma once
#include <cstdio>
#include <cstdint>
#ifdef _WIN32
inline int eiii_fseek(FILE *f, int64_t offset, int origin) { return _fseeki64(f, offset, origin); }
inline int64_t eiii_ftell(FILE *f) { return _ftelli64(f); }
#else
inline int eiii_fseek(FILE *f, int64_t offset, int origin) { return fseeko(f, off_t(offset), origin); }
inline int64_t eiii_ftell(FILE *f) { return ftello(f); }
#endif
