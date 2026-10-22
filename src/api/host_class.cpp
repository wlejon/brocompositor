#include "host_class.h"
#include "object_builder.h"

#include <mutex>
#include <unordered_map>

namespace brocompositor::api {

namespace {

// One table per thread, keyed by the class object: the constructor and
// prototype a class holds are Persistents of the thread that installed it,
// so a second thread's realm gets a second, independent pair. Never freed —
// a thread's runtime lives until the thread does.
std::unordered_map<const HostClass*, HostClass::Slots>& threadSlots() {
    static thread_local std::unordered_map<const HostClass*, HostClass::Slots> t;
    return t;
}

void publishGlobal(const char* name, Value val) {
    ev::Persistent v(val);
    ev::registerGlobal(name, v.get());
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (gt.found && ev::isObject(gt.value)) {
        ev::Persistent global(gt.value);
        ev::setProperty(global.get(), name, v.get());
    }
}

// ── Brands ──────────────────────────────────────────────────────────────────
// Every payload make() hands out is registered with the class that made it,
// and unwrap() answers only for that class and subclasses that inherit() from
// it. The table is process-wide and outlives every sweep.
struct Brand {
    const HostClass* cls;
    ev::HandleDestructor dtor;
};

std::mutex& brandMutex() {
    static std::mutex m;
    return m;
}

std::unordered_map<const void*, Brand>& brands() {
    static auto* m = new std::unordered_map<const void*, Brand>();
    return *m;
}

void brandedDestroy(void* data) {
    ev::HandleDestructor dtor = nullptr;
    {
        std::lock_guard<std::mutex> lk(brandMutex());
        auto& m = brands();
        auto it = m.find(data);
        if (it != m.end()) {
            dtor = it->second.dtor;
            m.erase(it);
        }
    }
    if (dtor) dtor(data);
}

} // namespace

HostClass::Slots& HostClass::slots() const {
    return threadSlots()[this];
}

const HostClass::Slots* HostClass::slotsIfAny() const {
    auto& t = threadSlots();
    auto it = t.find(this);
    return it == t.end() ? nullptr : &it->second;
}

bool HostClass::installed() const {
    const Slots* s = slotsIfAny();
    if (!s || !s->ctor || !s->proto || !s->realmGlobal) return false;
    ev::GlobalValue gt = ev::globalValue("globalThis");
    if (!gt.found || !ev::isObject(gt.value)) return false;
    ev::Persistent gtP(gt.value);
    if (s->realmGlobal->get() != gtP.get()) return false;
    if (s->isGlobal && !s->name.empty()) {
        Value cur = ev::getProperty(gtP.get(), s->name);
        if (cur != s->ctor->get()) return false;
    }
    return true;
}

void HostClass::install(const char* name, uint32_t arity, ev::NativeFn body,
                        const std::function<void(ObjectBuilder&)>& decorate,
                        bool global) {
    ev::NativeFn ctorBody = body;
    if (!ctorBody) {
        std::string msg = std::string("TypeError: ") + (name ? name : "") + " is not a constructor";
        ctorBody = [msg](Value, std::span<const Value>) { return ev::throwTypeError(msg); };
    }

    Slots& s = slots();
    delete s.ctor;
    delete s.proto;
    delete s.instanceProto;
    delete s.realmGlobal;
    s.ctor = nullptr;
    s.proto = nullptr;
    s.instanceProto = nullptr;
    s.realmGlobal = nullptr;
    s.name = name ? name : "";
    s.aliases.clear();
    s.isGlobal = global;

    ev::Persistent ctor(ev::makeFunction(std::move(ctorBody), arity, s.name));
    s.ctor = new ev::Persistent(ctor.get());

    {
        ObjectBuilder proto(ev::getProperty(ctor.get(), "prototype"));
        proto.set("constructor", ctor.get());
        if (decorate) decorate(proto);
        s.proto = new ev::Persistent(proto.get());
    }

    {
        ev::GlobalValue gt = ev::globalValue("globalThis");
        if (gt.found && ev::isObject(gt.value)) {
            s.realmGlobal = new ev::Persistent(gt.value);
        }
    }

    if (global && !s.name.empty()) {
        publishGlobal(s.name.c_str(), s.ctor->get());
    }
}

void HostClass::alias(const char* name) const {
    if (!name || !*name) return;
    Slots& s = const_cast<HostClass*>(this)->slots();
    if (!s.ctor) return;
    s.aliases.emplace_back(name);
    publishGlobal(name, s.ctor->get());
}

void HostClass::inherit(const HostClass& base) const {
    base_.store(&base, std::memory_order_release);
    const Slots* s = slotsIfAny();
    const Slots* b = base.slotsIfAny();
    if (!s || !s->proto || !b || !b->proto) return;
    ev::GlobalValue objectCtor = ev::globalValue("Object");
    if (!objectCtor.found || !ev::isObject(objectCtor.value)) return;
    ev::Persistent objectNs(objectCtor.value);
    ev::Persistent setProto(ev::getProperty(objectNs.get(), "setPrototypeOf"));
    if (!ev::isFunction(setProto.get())) return;
    const Value args[2] = {s->proto->get(), b->proto->get()};
    ev::call(setProto.get(), ev::undefined(), std::span<const Value>(args, 2));
}

void HostClass::setInstancePrototype(Value p) {
    Slots& s = slots();
    delete s.instanceProto;
    s.instanceProto = new ev::Persistent(p);
}

bool HostClass::isInstance(Value val) const {
    return unwrap(val) != nullptr;
}

void* HostClass::unwrap(Value val) const {
    void* data = ev::handleData(val);
    if (!data) return nullptr;
    const HostClass* cls = nullptr;
    {
        std::lock_guard<std::mutex> lk(brandMutex());
        auto& m = brands();
        auto it = m.find(data);
        if (it == m.end()) return nullptr;
        cls = it->second.cls;
    }
    for (const HostClass* c = cls; c; c = c->base_.load(std::memory_order_acquire)) {
        if (c == this) return data;
    }
    return nullptr;
}

Value HostClass::make(void* data, ev::HandleDestructor dtor, ev::Finalize when) const {
    const Slots* s = slotsIfAny();
    Value p = ev::undefined();
    if (s) {
        p = s->instanceProto ? s->instanceProto->get()
                             : (s->proto ? s->proto->get() : ev::undefined());
    }
    if (!data) {
        if (ev::isUndefined(p)) return ev::makeHandle(data, dtor, when);
        return ev::makeHandle(data, dtor, when, p);
    }
    {
        std::lock_guard<std::mutex> lk(brandMutex());
        brands()[data] = Brand{this, dtor};
    }
    if (ev::isUndefined(p)) return ev::makeHandle(data, brandedDestroy, when);
    return ev::makeHandle(data, brandedDestroy, when, p);
}

void HostClass::setStatic(const char* name, Value v) const {
    const Slots* s = slotsIfAny();
    if (!s || !s->ctor) return;
    ev::Persistent valP(v);
    s->ctor->set(ev::setProperty(s->ctor->get(), name, valP.get()));
    for (const auto& a : s->aliases) {
        publishGlobal(a.c_str(), s->ctor->get());
    }
}

Value HostClass::prototype() const {
    const Slots* s = slotsIfAny();
    return (s && s->proto) ? s->proto->get() : ev::undefined();
}

Value HostClass::constructor() const {
    const Slots* s = slotsIfAny();
    return (s && s->ctor) ? s->ctor->get() : ev::undefined();
}

Value hostArrayOf(size_t count, const std::function<Value(size_t)>& make) {
    Value arr = ev::makeArray(static_cast<uint32_t>(count));
    if (!ev::isObject(arr)) {
        return ev::undefined();
    }
    ev::Persistent persistentArr(arr);
    for (size_t i = 0; i < count; ++i) {
        ev::Persistent v(make(i));
        persistentArr.set(ev::setElement(persistentArr.get(), static_cast<uint32_t>(i), v.get()));
    }
    return persistentArr.get();
}

} // namespace brocompositor::api
