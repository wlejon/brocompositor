#pragma once

#include "embed/embed.h"

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace brocompositor::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

class ArgReader {
public:
    explicit ArgReader(std::span<const Value> args) {
        pinned_.reserve(args.size());
        for (const auto& a : args) {
            pinned_.emplace_back(a);
        }
    }

    size_t count() const noexcept {
        return pinned_.size();
    }

    bool has(size_t i) const {
        return i < pinned_.size() && !ev::isUndefined(pinned_[i].get());
    }

    Value get(size_t i) const {
        return i < pinned_.size() ? pinned_[i].get() : ev::undefined();
    }

    bool isObject(size_t i) const {
        return has(i) && ev::isObject(pinned_[i].get());
    }

    bool isString(size_t i) const {
        return has(i) && ev::isString(pinned_[i].get());
    }

    bool isNumber(size_t i) const {
        return has(i) && ev::isNumber(pinned_[i].get());
    }

    bool isBool(size_t i) const {
        return has(i) && ev::isBool(pinned_[i].get());
    }

    bool isFunction(size_t i) const {
        return has(i) && ev::isFunction(pinned_[i].get());
    }

    double getDouble(size_t i, double def = 0.0) const {
        if (!has(i)) return def;
        Value v = pinned_[i].get();
        if (ev::isObject(v)) return def;
        double d = ev::toDouble(v);
        return std::isnan(d) ? def : d;
    }

    int32_t getInt(size_t i, int32_t def = 0) const {
        if (!has(i)) return def;
        double d = getDouble(i, static_cast<double>(def));
        if (d >= 2147483647.0) return INT32_MAX;
        if (d <= -2147483648.0) return INT32_MIN;
        return static_cast<int32_t>(d);
    }

    uint32_t getUint(size_t i, uint32_t def = 0) const {
        if (!has(i)) return def;
        double d = getDouble(i, static_cast<double>(def));
        if (d >= 4294967295.0) return UINT32_MAX;
        if (!(d > 0.0)) return 0;
        return static_cast<uint32_t>(d);
    }

    uint64_t getUint64(size_t i, uint64_t def = 0) const {
        if (!has(i)) return def;
        double d = getDouble(i, static_cast<double>(def));
        if (d <= 0.0) return 0;
        return static_cast<uint64_t>(d);
    }

    bool getBool(size_t i, bool def = false) const {
        if (!has(i)) return def;
        return ev::toBool(pinned_[i].get());
    }

    std::string getString(size_t i, const std::string& def = "") const {
        if (!has(i)) return def;
        Value v = pinned_[i].get();
        if (ev::isUndefined(v) || ev::isNull(v) || ev::isSymbol(v)) return def;
        return ev::toUtf8(v);
    }

    // Property helper functions (strictly GC-safe via ev::Persistent)
    static bool hasProp(Value obj, std::string_view prop) {
        if (!ev::isObject(obj)) return false;
        ev::Persistent p(ev::getProperty(obj, prop));
        return !ev::isUndefined(p.get());
    }

    static Value getProp(Value obj, std::string_view prop) {
        if (!ev::isObject(obj)) return ev::undefined();
        ev::Persistent p(ev::getProperty(obj, prop));
        return p.get();
    }

    static std::string getPropString(Value obj, std::string_view prop, const std::string& def = "") {
        if (!ev::isObject(obj)) return def;
        ev::Persistent p(ev::getProperty(obj, prop));
        if (ev::isString(p.get())) return ev::toUtf8(p.get());
        return def;
    }

    static double getPropDouble(Value obj, std::string_view prop, double def = 0.0) {
        if (!ev::isObject(obj)) return def;
        ev::Persistent p(ev::getProperty(obj, prop));
        if (ev::isNumber(p.get())) {
            double d = ev::toDouble(p.get());
            return std::isnan(d) ? def : d;
        }
        return def;
    }

    static int32_t getPropInt(Value obj, std::string_view prop, int32_t def = 0) {
        double d = getPropDouble(obj, prop, static_cast<double>(def));
        if (d >= 2147483647.0) return INT32_MAX;
        if (d <= -2147483648.0) return INT32_MIN;
        return static_cast<int32_t>(d);
    }

    static uint32_t getPropUint(Value obj, std::string_view prop, uint32_t def = 0) {
        double d = getPropDouble(obj, prop, static_cast<double>(def));
        if (d >= 4294967295.0) return UINT32_MAX;
        if (!(d > 0.0)) return 0;
        return static_cast<uint32_t>(d);
    }

    static uint64_t getPropUint64(Value obj, std::string_view prop, uint64_t def = 0) {
        double d = getPropDouble(obj, prop, static_cast<double>(def));
        if (d <= 0.0) return 0;
        return static_cast<uint64_t>(d);
    }

    static bool getPropBool(Value obj, std::string_view prop, bool def = false) {
        if (!ev::isObject(obj)) return def;
        ev::Persistent p(ev::getProperty(obj, prop));
        if (ev::isBool(p.get())) return ev::toBool(p.get());
        return def;
    }

private:
    std::vector<ev::Persistent> pinned_;
};

} // namespace brocompositor::api
