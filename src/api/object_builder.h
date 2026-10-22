#pragma once

#include "embed/embed.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>

namespace brocompositor::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

/// Helper to build objects and namespaces property by property using bronze::embed.
/// Handles moving GC by rooting the target in an ev::Persistent and rooting
/// allocated values before setting properties.
struct ObjectBuilder {
    ev::Persistent obj;

    ObjectBuilder() : obj(ev::createObject()) {}
    explicit ObjectBuilder(Value existing) : obj(existing) {}

    void set(std::string_view name, Value v) {
        ev::Persistent valP(v);
        obj.set(ev::setProperty(obj.get(), name, valP.get()));
    }

    void set(std::string_view name, double d) {
        set(name, ev::fromDouble(d));
    }

    void set(std::string_view name, int32_t i) {
        set(name, ev::fromDouble(static_cast<double>(i)));
    }

    void set(std::string_view name, uint32_t u) {
        set(name, ev::fromDouble(static_cast<double>(u)));
    }

    void set(std::string_view name, uint16_t u) {
        set(name, ev::fromDouble(static_cast<double>(u)));
    }

    void set(std::string_view name, int64_t i) {
        set(name, ev::fromDouble(static_cast<double>(i)));
    }

    void set(std::string_view name, uint64_t u) {
        set(name, ev::fromDouble(static_cast<double>(u)));
    }

    void set(std::string_view name, bool b) {
        set(name, ev::fromBool(b));
    }

    void set(std::string_view name, const std::string& s) {
        set(name, ev::fromUtf8(s));
    }

    void set(std::string_view name, const char* s) {
        set(name, ev::fromUtf8(s ? s : ""));
    }

    void set(std::string_view name, std::string_view s) {
        set(name, ev::fromUtf8(s));
    }

    void setNull(std::string_view name) {
        set(name, ev::null());
    }

    void setUndefined(std::string_view name) {
        set(name, ev::undefined());
    }

    void def(std::string_view name, uint32_t arity, ev::NativeFn fn) {
        ev::Persistent f(ev::makeFunction(std::move(fn), arity, name));
        obj.set(ev::setProperty(obj.get(), name, f.get()));
    }

    void accessor(std::string_view name, ev::NativeFn getter, ev::NativeFn setter = nullptr) {
        const std::string getName = "get " + std::string(name);
        const std::string setName = "set " + std::string(name);
        ev::Persistent g(ev::makeFunction(std::move(getter), 0, getName));
        ev::Persistent s(setter ? ev::makeFunction(std::move(setter), 1, setName)
                                : ev::undefined());
        obj.set(ev::defineAccessor(obj.get(), name, g.get(), s.get(), /*enumerable=*/true));
    }

    Value get() const { return obj.get(); }
    Value build() const { return obj.get(); }
};

/// Helper to build JavaScript arrays item by item using bronze::embed.
struct ArrayBuilder {
    ev::Persistent arr;
    uint32_t length = 0;

    ArrayBuilder() : arr(ev::makeArray(0)), length(0) {}
    explicit ArrayBuilder(size_t count)
        : arr(ev::makeArray(static_cast<uint32_t>(count))),
          length(static_cast<uint32_t>(count)) {}

    void set(uint32_t index, Value v) {
        ev::Persistent valP(v);
        arr.set(ev::setElement(arr.get(), index, valP.get()));
        if (index >= length) length = index + 1;
    }

    void set(uint32_t index, const std::string& s) {
        set(index, ev::fromUtf8(s));
    }

    void set(uint32_t index, const char* s) {
        set(index, ev::fromUtf8(s ? s : ""));
    }

    void set(uint32_t index, std::string_view s) {
        set(index, ev::fromUtf8(s));
    }

    void set(uint32_t index, double d) {
        set(index, ev::fromDouble(d));
    }

    void set(uint32_t index, int32_t i) {
        set(index, ev::fromDouble(static_cast<double>(i)));
    }

    void set(uint32_t index, uint32_t u) {
        set(index, ev::fromDouble(static_cast<double>(u)));
    }

    void set(uint32_t index, bool b) {
        set(index, ev::fromBool(b));
    }

    void push(Value v) {
        set(length, v);
    }

    Value get() const { return arr.get(); }
    Value build() const { return arr.get(); }
};

} // namespace brocompositor::api
