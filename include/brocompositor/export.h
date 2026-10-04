#pragma once

#if defined(BROCOMPOSITOR_STATIC_DEFINE)
    #define BROCOMPOSITOR_API
#elif defined(_WIN32) || defined(__CYGWIN__)
    #if defined(BROCOMPOSITOR_EXPORTS)
        #define BROCOMPOSITOR_API __declspec(dllexport)
    #else
        #define BROCOMPOSITOR_API
    #endif
#else
    #if defined(__GNUC__) && __GNUC__ >= 4
        #define BROCOMPOSITOR_API __attribute__((visibility("default")))
    #else
        #define BROCOMPOSITOR_API
    #endif
#endif
