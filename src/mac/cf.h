// CoreFoundation helpers for the macOS backend's C++ translation units.
#pragma once

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace brocompositor::mac {

// Owning reference to a CF object (released on destruction).
template <class T>
class CFRef {
public:
    CFRef() = default;
    explicit CFRef(T ref) : ref_(ref) {}  // adopts a +1 reference
    CFRef(const CFRef& o) : ref_(o.ref_) {
        if (ref_) CFRetain(ref_);
    }
    CFRef(CFRef&& o) noexcept : ref_(std::exchange(o.ref_, nullptr)) {}
    CFRef& operator=(CFRef o) noexcept {
        std::swap(ref_, o.ref_);
        return *this;
    }
    ~CFRef() {
        if (ref_) CFRelease(ref_);
    }
    static CFRef retain(T ref) {
        if (ref) CFRetain(ref);
        return CFRef(ref);
    }
    T get() const { return ref_; }
    explicit operator bool() const { return ref_ != nullptr; }
    // For out-parameters of Copy/Create functions.
    T* out() {
        *this = CFRef();
        return &ref_;
    }
    CFTypeRef* out_type() { return reinterpret_cast<CFTypeRef*>(out()); }

private:
    T ref_ = nullptr;
};

inline std::string to_string(CFStringRef s) {
    if (!s) return {};
    if (const char* p = CFStringGetCStringPtr(s, kCFStringEncodingUTF8)) return p;
    CFIndex len = CFStringGetLength(s);
    CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
    std::string out(size_t(max), '\0');
    if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return {};
    out.resize(std::char_traits<char>::length(out.c_str()));
    return out;
}

inline CFRef<CFStringRef> make_string(const std::string& s) {
    return CFRef<CFStringRef>(CFStringCreateWithCString(nullptr, s.c_str(), kCFStringEncodingUTF8));
}

inline std::optional<int64_t> dict_int(CFDictionaryRef d, CFStringRef key) {
    auto v = static_cast<CFNumberRef>(CFDictionaryGetValue(d, key));
    int64_t out = 0;
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID() || !CFNumberGetValue(v, kCFNumberSInt64Type, &out))
        return std::nullopt;
    return out;
}

inline std::optional<double> dict_double(CFDictionaryRef d, CFStringRef key) {
    auto v = static_cast<CFNumberRef>(CFDictionaryGetValue(d, key));
    double out = 0;
    if (!v || CFGetTypeID(v) != CFNumberGetTypeID() || !CFNumberGetValue(v, kCFNumberDoubleType, &out))
        return std::nullopt;
    return out;
}

inline bool dict_bool(CFDictionaryRef d, CFStringRef key) {
    CFTypeRef v = CFDictionaryGetValue(d, key);
    if (!v) return false;
    if (CFGetTypeID(v) == CFBooleanGetTypeID()) return CFBooleanGetValue(static_cast<CFBooleanRef>(v));
    if (CFGetTypeID(v) == CFNumberGetTypeID()) {
        int64_t n = 0;
        CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberSInt64Type, &n);
        return n != 0;
    }
    return false;
}

inline std::string dict_string(CFDictionaryRef d, CFStringRef key) {
    auto v = static_cast<CFStringRef>(CFDictionaryGetValue(d, key));
    return v && CFGetTypeID(v) == CFStringGetTypeID() ? to_string(v) : std::string();
}

}  // namespace brocompositor::mac
