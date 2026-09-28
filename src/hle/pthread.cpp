// pthreads + semaphores. Guest sync objects are small integers in guest memory;
// we key host objects by their guest address.
#include "hle_common.h"
#include <windows.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

using namespace hle;

namespace {

// ---------------------------------------------------------------------------
// Mutex (supports recursive locking; bionic encodes the type in the mutex word)
// ---------------------------------------------------------------------------
struct HMutex {
    std::mutex m;
    std::condition_variable cv;
    DWORD owner = 0;
    int count = 0;
    bool recursive = false;

    void lock() {
        DWORD me = GetCurrentThreadId();
        std::unique_lock<std::mutex> l(m);
        if (owner == me) {
            if (recursive) { count++; return; }
            LOGE("relock of non-recursive mutex (would deadlock); treating as recursive");
            count++;
            return;
        }
        cv.wait(l, [&] { return owner == 0; });
        owner = me;
        count = 1;
    }
    bool try_lock() {
        DWORD me = GetCurrentThreadId();
        std::lock_guard<std::mutex> l(m);
        if (owner == me && recursive) { count++; return true; }
        if (owner != 0) return false;
        owner = me;
        count = 1;
        return true;
    }
    void unlock() {
        std::lock_guard<std::mutex> l(m);
        if (owner != GetCurrentThreadId()) return;
        if (--count == 0) {
            owner = 0;
            cv.notify_one();
        }
    }
    int release_all() {  // for cond_wait
        std::lock_guard<std::mutex> l(m);
        int n = count;
        owner = 0;
        count = 0;
        cv.notify_one();
        return n;
    }
    void reacquire(int n) {
        lock();
        std::lock_guard<std::mutex> l(m);
        count = n;
    }
};

struct HCond {
    std::mutex m;
    std::condition_variable cv;
    u64 seq = 0;
};

struct HSem {
    std::mutex m;
    std::condition_variable cv;
    u32 value = 0;
};

std::mutex g_objs_lock;
std::unordered_map<u32, HMutex*> g_mutexes;
std::unordered_map<u32, HCond*> g_conds;
std::unordered_map<u32, HSem*> g_sems;

HMutex* get_mutex(u32 g) {
    std::lock_guard<std::mutex> l(g_objs_lock);
    auto& p = g_mutexes[g];
    if (!p) {
        p = new HMutex();
        u32 v = mem::r32(g);
        p->recursive = (v & 0xC000) == 0x4000;  // bionic MUTEX_TYPE_RECURSIVE
    }
    return p;
}
HCond* get_cond(u32 g) {
    std::lock_guard<std::mutex> l(g_objs_lock);
    auto& p = g_conds[g];
    if (!p) p = new HCond();
    return p;
}
HSem* get_sem(u32 g) {
    std::lock_guard<std::mutex> l(g_objs_lock);
    auto& p = g_sems[g];
    if (!p) p = new HSem();
    return p;
}

// ---------------------------------------------------------------------------
// Threads
// ---------------------------------------------------------------------------
struct ThreadInfo {
    u32 id;
    std::thread t;
    u32 result = 0;
    bool detached = false;
};
std::mutex g_threads_lock;
std::unordered_map<u32, ThreadInfo*> g_threads;
std::atomic<u32> g_next_tid{2};

// Thread-specific data (keys)
std::atomic<u32> g_next_key{1};
thread_local std::unordered_map<u32, u32> t_specific;

}  // namespace

// ---------------------------------------------------------------------------
// mutexes
// ---------------------------------------------------------------------------
HLE(pthread_mutexattr_init) { mem::w32(c.r(0), 0); c.ret(0); }
HLE(pthread_mutexattr_destroy) { c.ret(0); }
HLE(pthread_mutexattr_settype) { mem::w32(c.r(0), c.r(1)); c.ret(0); }  // 1 = recursive
HLE(pthread_mutex_init) {
    u32 g = c.r(0);
    bool rec = c.r(1) && mem::r32(c.r(1)) == 1;
    mem::w32(g, rec ? 0x4000 : 0);
    {
        std::lock_guard<std::mutex> l(g_objs_lock);
        auto it = g_mutexes.find(g);
        if (it != g_mutexes.end()) { delete it->second; g_mutexes.erase(it); }
    }
    get_mutex(g)->recursive = rec;
    c.ret(0);
}
HLE(pthread_mutex_destroy) {
    std::lock_guard<std::mutex> l(g_objs_lock);
    auto it = g_mutexes.find(c.r(0));
    if (it != g_mutexes.end()) { delete it->second; g_mutexes.erase(it); }
    c.ret(0);
}
HLE(pthread_mutex_lock) { get_mutex(c.r(0))->lock(); c.ret(0); }
HLE(pthread_mutex_trylock) { c.ret(get_mutex(c.r(0))->try_lock() ? 0 : 16 /* EBUSY */); }
HLE(pthread_mutex_unlock) { get_mutex(c.r(0))->unlock(); c.ret(0); }

// ---------------------------------------------------------------------------
// condition variables
// ---------------------------------------------------------------------------
HLE(pthread_cond_init) { get_cond(c.r(0)); c.ret(0); }
HLE(pthread_cond_destroy) {
    std::lock_guard<std::mutex> l(g_objs_lock);
    auto it = g_conds.find(c.r(0));
    if (it != g_conds.end()) { delete it->second; g_conds.erase(it); }
    c.ret(0);
}
HLE(pthread_cond_wait) {
    HCond* cv = get_cond(c.r(0));
    HMutex* m = get_mutex(c.r(1));
    u64 seq;
    { std::lock_guard<std::mutex> l(cv->m); seq = cv->seq; }
    int n = m->release_all();
    {
        std::unique_lock<std::mutex> l(cv->m);
        cv->cv.wait(l, [&] { return cv->seq != seq; });
    }
    m->reacquire(n);
    c.ret(0);
}
HLE(pthread_cond_timedwait) {
    HCond* cv = get_cond(c.r(0));
    HMutex* m = get_mutex(c.r(1));
    u32 ts = c.r(2);
    // absolute CLOCK_REALTIME deadline
    auto deadline = std::chrono::system_clock::time_point(std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::seconds(mem::r32(ts)) + std::chrono::nanoseconds(mem::r32(ts + 4))));
    u64 seq;
    { std::lock_guard<std::mutex> l(cv->m); seq = cv->seq; }
    int n = m->release_all();
    bool ok;
    {
        std::unique_lock<std::mutex> l(cv->m);
        ok = cv->cv.wait_until(l, deadline, [&] { return cv->seq != seq; });
    }
    m->reacquire(n);
    c.ret(ok ? 0 : 110 /* ETIMEDOUT */);
}
HLE(pthread_cond_signal) {
    HCond* cv = get_cond(c.r(0));
    { std::lock_guard<std::mutex> l(cv->m); cv->seq++; }
    cv->cv.notify_one();
    c.ret(0);
}
HLE(pthread_cond_broadcast) {
    HCond* cv = get_cond(c.r(0));
    { std::lock_guard<std::mutex> l(cv->m); cv->seq++; }
    cv->cv.notify_all();
    c.ret(0);
}

// ---------------------------------------------------------------------------
// semaphores
// ---------------------------------------------------------------------------
HLE(sem_init) { get_sem(c.r(0))->value = c.r(2); c.ret(0); }
HLE(sem_destroy) { c.ret(0); }
HLE(sem_post) {
    HSem* s = get_sem(c.r(0));
    { std::lock_guard<std::mutex> l(s->m); s->value++; }
    s->cv.notify_one();
    c.ret(0);
}
HLE(sem_wait) {
    HSem* s = get_sem(c.r(0));
    std::unique_lock<std::mutex> l(s->m);
    s->cv.wait(l, [&] { return s->value > 0; });
    s->value--;
    c.ret(0);
}
HLE(sem_trywait) {
    HSem* s = get_sem(c.r(0));
    std::lock_guard<std::mutex> l(s->m);
    if (!s->value) { set_errno(c, 11); c.ret((u32)-1); return; }
    s->value--;
    c.ret(0);
}

// ---------------------------------------------------------------------------
// thread attributes (bionic pthread_attr_t: flags, stack_base, stack_size, guard_size, policy, priority)
// ---------------------------------------------------------------------------
HLE(pthread_attr_init) {
    u32 a = c.r(0);
    memset(mem::ptr(a), 0, 24);
    mem::w32(a + 8, 1024 * 1024);
    c.ret(0);
}
HLE(pthread_attr_destroy) { c.ret(0); }
HLE(pthread_attr_setdetachstate) { mem::w32(c.r(0), c.r(1) ? 1 : 0); c.ret(0); }
HLE(pthread_attr_setstacksize) { mem::w32(c.r(0) + 8, c.r(1)); c.ret(0); }
HLE(pthread_attr_getschedparam) { mem::w32(c.r(1), mem::r32(c.r(0) + 20)); c.ret(0); }
HLE(pthread_attr_setschedparam) { mem::w32(c.r(0) + 20, mem::r32(c.r(1))); c.ret(0); }
HLE(pthread_setschedparam) { c.ret(0); }
HLE(pthread_getschedparam) { mem::w32(c.r(1), 0); mem::w32(c.r(2), 0); c.ret(0); }

// ---------------------------------------------------------------------------
// threads
// ---------------------------------------------------------------------------
HLE(pthread_create) {
    u32 out = c.r(0), attr = c.r(1), fn = c.r(2), arg = c.r(3);
    u32 stack = attr ? mem::r32(attr + 8) : 1024 * 1024;
    if (stack < 256 * 1024) stack = 256 * 1024;
    bool detached = attr && (mem::r32(attr) & 1);
    auto* ti = new ThreadInfo();
    ti->id = g_next_tid++;
    ti->detached = detached;
    {
        std::lock_guard<std::mutex> l(g_threads_lock);
        g_threads[ti->id] = ti;
    }
    mem::w32(out, ti->id);
    LOGI("pthread_create: thread %u -> %s", ti->id, symbolize(fn).c_str());
    ti->t = std::thread([ti, fn, arg, stack] {
        Cpu cpu(ti->id, stack);
        Cpu::current = &cpu;
        u32 r = cpu.call(fn, {arg});
        ti->result = cpu.exiting ? cpu.exit_value : r;
        LOGI("thread %u finished", ti->id);
    });
    if (detached) ti->t.detach();
    c.ret(0);
}
HLE(pthread_join) {
    ThreadInfo* ti;
    {
        std::lock_guard<std::mutex> l(g_threads_lock);
        auto it = g_threads.find(c.r(0));
        if (it == g_threads.end()) { c.ret(3 /* ESRCH */); return; }
        ti = it->second;
    }
    if (ti->t.joinable()) ti->t.join();
    if (c.r(1)) mem::w32(c.r(1), ti->result);
    c.ret(0);
}
HLE(pthread_detach) {
    std::lock_guard<std::mutex> l(g_threads_lock);
    auto it = g_threads.find(c.r(0));
    if (it != g_threads.end() && it->second->t.joinable()) it->second->t.detach();
    c.ret(0);
}
HLE(pthread_exit) {
    c.request_exit(c.r(0));
}
HLE(pthread_self) { c.ret(c.thread_id); }
HLE(pthread_equal) { c.ret(c.r(0) == c.r(1) ? 1 : 0); }
HLE(gettid) { c.ret(c.thread_id); }
HLE(getpid) { c.ret(1234); }

HLE(pthread_once) {
    // bionic pthread_once_t: 0 = not run. We use 1 = running, 2 = done.
    static std::mutex m;
    static std::condition_variable cv;
    u32 ctl = c.r(0), fn = c.r(1);
    {
        std::unique_lock<std::mutex> l(m);
        u32 v = mem::r32(ctl);
        if (v == 2) { c.ret(0); return; }
        if (v == 1) { cv.wait(l, [&] { return mem::r32(ctl) == 2; }); c.ret(0); return; }
        mem::w32(ctl, 1);
    }
    c.call(fn, {});
    {
        std::lock_guard<std::mutex> l(m);
        mem::w32(ctl, 2);
    }
    cv.notify_all();
    c.ret(0);
}

HLE(pthread_key_create) {
    mem::w32(c.r(0), g_next_key++);
    c.ret(0);
}
HLE(pthread_key_delete) { c.ret(0); }
HLE(pthread_getspecific) {
    auto it = t_specific.find(c.r(0));
    c.ret(it == t_specific.end() ? 0 : it->second);
}
HLE(pthread_setspecific) {
    t_specific[c.r(0)] = c.r(1);
    c.ret(0);
}
HLE(sched_yield) { std::this_thread::yield(); c.ret(0); }
