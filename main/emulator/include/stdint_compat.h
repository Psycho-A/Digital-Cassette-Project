// C89 <stdint.h> replacement
#ifndef STDINT_COMPAT_H
#define STDINT_COMPAT_H

typedef signed char        int8_t;
typedef unsigned char      uint8_t;
typedef signed short       int16_t;
typedef unsigned short     uint16_t;
typedef signed long        int32_t;
typedef unsigned long      uint32_t;

#ifdef _MSC_VER
    #define inline           __inline
    #define snprintf          _snprintf
    #define vsnprintf         _vsnprintf
    #define strcasecmp         stricmp
    #define strncasecmp        strnicmp
    #define log2(x)           (log(x)/log(2))
    typedef signed   __int64   int64_t;
    typedef unsigned __int64   uint64_t;
#else
    typedef signed long long   int64_t;
    typedef unsigned long long uint64_t;
#endif

#ifndef DWORD_PTR
#ifdef _WIN64
    #define DWORD_PTR  unsigned __int64
#else
    #define DWORD_PTR  unsigned long
#endif
#endif

/* Note: int32_t assumed 32-bit on all target platforms.
   int32_t = signed long > на Windows = 32 бита, на LP64 будет 64.
*/
#endif
