#pragma once

#include "embed/embed.h"
#include "object_builder.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace brocompositor::api {

namespace ev = bronze::embed;
using Value = bronze::Value;

// A host class: one constructor function + one prototype, with instances
// born on that prototype by make()/createInstance().
//
// The class objects are process-global, but what they hold is PER THREAD:
// bronze's runtime is per-thread, and a Persistent is a slot in its creating
// thread's registry, so a constructor made on the main thread means nothing
// to a Worker's realm. Every accessor reads the CALLING thread's slots and
// install() fills the calling thread's; a realm installs each class once,
// and installed() answers for the calling thread's active realm.
class HostClass {
public:
    struct Slots {
        ev::Persistent* proto = nullptr;
        ev::Persistent* ctor = nullptr;
        ev::Persistent* instanceProto = nullptr;
        ev::Persistent* realmGlobal = nullptr;
        std::string name;
        std::vector<std::string> aliases;
        bool isGlobal = true;
    };

    void install(const char* name, uint32_t arity, ev::NativeFn body,
                 const std::function<void(ObjectBuilder&)>& decorate = nullptr,
                 bool global = true);

    void init(const char* name, const std::function<void(ObjectBuilder&)>& decorate) {
        install(name, 0, nullptr, decorate);
    }

    template <typename T>
    Value createInstance(std::unique_ptr<T> ptr) const {
        return make(ptr.release(), [](void* p) { delete static_cast<T*>(p); });
    }

    void alias(const char* name) const;
    void inherit(const HostClass& base) const;
    void setInstancePrototype(Value p);

    Value make(void* data, ev::HandleDestructor dtor,
               ev::Finalize when = ev::Finalize::InSweep) const;

    void setStatic(const char* name, Value v) const;

    // The payload of a handle THIS class (or a subclass that inherit()ed from
    // it) made; nullptr for anything else, including another class's handle.
    // Allocates nothing.
    void* unwrap(Value val) const;
    bool isInstance(Value val) const;

    Value prototype() const;
    Value constructor() const;

    // Whether install() has run on the CALLING thread's active realm.
    bool installed() const;

private:
    mutable std::atomic<const HostClass*> base_{nullptr};

    Slots& slots() const;
    const Slots* slotsIfAny() const;
};

Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make);

} // namespace brocompositor::api
