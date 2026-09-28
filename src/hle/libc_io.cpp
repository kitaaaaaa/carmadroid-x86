// stdio, file descriptors, pipes, paths, stat, time, sockets (stubbed).
#include "hle_common.h"
#include "fds.h"
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utime.h>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;
using namespace hle;
using mem::ptr;
using mem::str;

// ============================================================================
// Paths
// ============================================================================
namespace hle {

Config g_config;

std::string guest_external_dir() { return "/sdcard"; }
std::string guest_files_dir() { return "/data/data/" + g_config.package + "/files"; }
std::string guest_obb_path() {
    return "/sdcard/Android/obb/" + g_config.package + "/main." + std::to_string(g_config.version_code) + "." +
           g_config.package + ".obb";
}

static bool starts_with(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }

std::string host_path(const char* gp) {
    std::string g = gp ? gp : "";
    for (auto& ch : g) if (ch == '\\') ch = '/';
    if (g == guest_obb_path()) return g_config.obb_host;
    std::string root = g_config.root;
    const std::string data = "/data/data/" + g_config.package + "/";
    if (starts_with(g, data)) return root + "/data/" + g.substr(data.size());
    for (const char* sd : {"/sdcard/", "/mnt/sdcard/", "/storage/emulated/0/", "/storage/sdcard0/"})
        if (starts_with(g, sd)) return root + "/sdcard/" + g.substr(strlen(sd));
    if (!g.empty() && g[0] == '/') return root + "/fs" + g;
    return root + "/" + g;
}

}  // namespace hle

static void make_parent_dirs(const std::string& host) {
    std::error_code ec;
    fs::create_directories(fs::path(host).parent_path(), ec);
}

// ============================================================================
// File descriptor table (files + pipes)
// ============================================================================
namespace fds {

std::mutex g_lock;
std::condition_variable g_any_data;  // notified whenever any pipe receives data
std::unordered_map<int, Fd> g_fds;
int g_next = 10;

int add(Fd f) {
    std::lock_guard<std::mutex> l(g_lock);
    int fd = g_next++;
    g_fds[fd] = std::move(f);
    return fd;
}

bool get(int fd, Fd& out) {
    std::lock_guard<std::mutex> l(g_lock);
    auto it = g_fds.find(fd);
    if (it == g_fds.end()) return false;
    out = it->second;
    return true;
}

bool readable(int fd) {
    Fd f;
    if (!get(fd, f) || !f.pipe) return false;
    std::lock_guard<std::mutex> l(f.pipe->m);
    return !f.pipe->buf.empty();
}

}  // namespace fds

static int bionic_to_host_flags(u32 f) {
    int acc = f & 3;
    int h = _O_BINARY | (acc == 0 ? _O_RDONLY : acc == 1 ? _O_WRONLY : _O_RDWR);
    if (f & 0100) h |= _O_CREAT;
    if (f & 01000) h |= _O_TRUNC;
    if (f & 02000) h |= _O_APPEND;
    if (f & 0200) h |= _O_EXCL;
    return h;
}

HLE(open) {
    std::string hp = host_path(str(c.r(0)));
    int flags = bionic_to_host_flags(c.r(1));
    if (flags & _O_CREAT) make_parent_dirs(hp);
    int h = _open(hp.c_str(), flags, _S_IREAD | _S_IWRITE);
    LOGV("open(\"%s\") -> %s : %d", str(c.r(0)), hp.c_str(), h);
    if (h < 0) { set_errno(c, 2); c.ret((u32)-1); return; }
    fds::Fd f;
    f.host = h;
    c.ret((u32)fds::add(f));
}

HLE(close) {
    int fd = (int)c.r(0);
    std::lock_guard<std::mutex> l(fds::g_lock);
    auto it = fds::g_fds.find(fd);
    if (it != fds::g_fds.end()) {
        if (it->second.host >= 0) _close(it->second.host);
        fds::g_fds.erase(it);
    }
    c.ret(0);
}

HLE(read) {
    int fd = (int)c.r(0);
    u32 buf = c.r(1), n = c.r(2);
    fds::Fd f;
    if (!fds::get(fd, f)) { set_errno(c, 9); c.ret((u32)-1); return; }
    if (f.pipe) {
        std::unique_lock<std::mutex> l(f.pipe->m);
        f.pipe->cv.wait(l, [&] { return !f.pipe->buf.empty(); });
        u32 k = 0;
        while (k < n && !f.pipe->buf.empty()) { mem::w8(buf + k++, f.pipe->buf.front()); f.pipe->buf.pop_front(); }
        c.ret(k);
        return;
    }
    c.ret((u32)_read(f.host, ptr(buf), n));
}

static s32 do_write(int fd, const u8* data, u32 n) {
    if (fd == 1 || fd == 2) { fwrite(data, 1, n, stdout); return (s32)n; }
    fds::Fd f;
    if (!fds::get(fd, f)) return -1;
    if (f.pipe) {
        {
            std::lock_guard<std::mutex> l(f.pipe->m);
            f.pipe->buf.insert(f.pipe->buf.end(), data, data + n);
        }
        f.pipe->cv.notify_all();
        { std::lock_guard<std::mutex> l(fds::g_lock); }
        fds::g_any_data.notify_all();
        return (s32)n;
    }
    return _write(f.host, data, n);
}

HLE(write) { c.ret((u32)do_write((int)c.r(0), ptr(c.r(1)), c.r(2))); }
HLE(writev) {
    u32 iov = c.r(1), cnt = c.r(2);
    s32 total = 0;
    for (u32 i = 0; i < cnt; i++) {
        s32 r = do_write((int)c.r(0), ptr(mem::r32(iov + i * 8)), mem::r32(iov + i * 8 + 4));
        if (r < 0) { c.ret((u32)-1); return; }
        total += r;
    }
    c.ret((u32)total);
}
HLE(lseek) {
    fds::Fd f;
    if (!fds::get((int)c.r(0), f) || f.host < 0) { c.ret((u32)-1); return; }
    c.ret((u32)_lseek(f.host, (long)(s32)c.r(1), (int)c.r(2)));
}
HLE(pipe) {
    auto p = std::make_shared<fds::Pipe>();
    fds::Fd rd, wr;
    rd.pipe = p;
    wr.pipe = p;
    mem::w32(c.r(0), (u32)fds::add(rd));
    mem::w32(c.r(0) + 4, (u32)fds::add(wr));
    c.ret(0);
}
HLE(fcntl) { c.ret(0); }
HLE(ioctl) { c.ret((u32)-1); }

// ---------------------------------------------------------------------------
// stat (bionic 32-bit ARM layout, 104 bytes)
// ---------------------------------------------------------------------------
static void fill_stat(u32 st, const struct _stat64& h) {
    memset(ptr(st), 0, 104);
    u32 mode = (h.st_mode & _S_IFDIR) ? 0040000 : 0100000;
    mode |= 0666 | ((h.st_mode & _S_IFDIR) ? 0111 : 0);
    mem::w32(st + 16, mode);
    mem::w32(st + 20, 1);
    u64 size = (u64)h.st_size;
    memcpy(ptr(st + 48), &size, 8);
    mem::w32(st + 56, 4096);
    u64 blocks = (size + 511) / 512;
    memcpy(ptr(st + 64), &blocks, 8);
    mem::w32(st + 72, (u32)h.st_atime);
    mem::w32(st + 80, (u32)h.st_mtime);
    mem::w32(st + 88, (u32)h.st_ctime);
}

HLE(stat) {
    std::string hp = host_path(str(c.r(0)));
    struct _stat64 h;
    if (_stat64(hp.c_str(), &h) != 0) { set_errno(c, 2); c.ret((u32)-1); return; }
    fill_stat(c.r(1), h);
    c.ret(0);
}
HLE(lstat) { hle_fn_stat(c); }
HLE(fstat) {
    fds::Fd f;
    struct _stat64 h;
    if (!fds::get((int)c.r(0), f) || f.host < 0 || _fstat64(f.host, &h) != 0) { set_errno(c, 9); c.ret((u32)-1); return; }
    fill_stat(c.r(1), h);
    c.ret(0);
}
HLE(access) {
    std::string hp = host_path(str(c.r(0)));
    c.ret(fs::exists(hp) ? 0 : (u32)-1);
}
HLE(mkdir) {
    std::error_code ec;
    fs::create_directories(host_path(str(c.r(0))), ec);
    c.ret(0);
}
HLE(unlink) { c.ret(_unlink(host_path(str(c.r(0))).c_str()) == 0 ? 0 : (u32)-1); }
HLE(remove) { c.ret(::remove(host_path(str(c.r(0))).c_str()) == 0 ? 0 : (u32)-1); }
HLE(rename) {
    std::string a = host_path(str(c.r(0))), b = host_path(str(c.r(1)));
    MoveFileExA(a.c_str(), b.c_str(), MOVEFILE_REPLACE_EXISTING) ? c.ret(0) : c.ret((u32)-1);
}
HLE(chmod) { c.ret(0); }
HLE(chown) { c.ret(0); }
HLE(utime) { c.ret(0); }

// ============================================================================
// stdio: guest FILE* is an opaque guest allocation mapped to a host FILE*
// ============================================================================
static std::mutex g_file_lock;
static std::unordered_map<u32, FILE*> g_files;
static u32 g_sF = 0;  // __sF[3]: stdin, stdout, stderr (84 bytes each in bionic)

namespace hle {
void init_stdio_data() {
    g_sF = mem::calloc(3, 84);
    g_files[g_sF] = stdin;
    g_files[g_sF + 84] = stdout;
    g_files[g_sF + 168] = stderr;
    register_data("__sF", g_sF);
}
}  // namespace hle

static FILE* host_file(u32 g) {
    std::lock_guard<std::mutex> l(g_file_lock);
    auto it = g_files.find(g);
    return it == g_files.end() ? nullptr : it->second;
}

HLE(fopen) {
    const char* gp = str(c.r(0));
    std::string mode = str(c.r(1));
    if (mode.find('b') == std::string::npos) mode += 'b';
    std::string hp = host_path(gp);
    if (mode[0] == 'w' || mode[0] == 'a') make_parent_dirs(hp);
    FILE* f = fopen(hp.c_str(), mode.c_str());
    LOGV("fopen(\"%s\", \"%s\") -> %s : %s", gp, str(c.r(1)), hp.c_str(), f ? "ok" : "FAILED");
    if (!f) { set_errno(c, 2); c.ret(0); return; }
    u32 g = mem::calloc(1, 84);
    std::lock_guard<std::mutex> l(g_file_lock);
    g_files[g] = f;
    c.ret(g);
}
HLE(fdopen) {
    LOGI("fdopen(%d) not supported", (int)c.r(0));
    c.ret(0);
}
HLE(fclose) {
    FILE* f = host_file(c.r(0));
    if (!f) { c.ret((u32)-1); return; }
    if (f != stdout && f != stderr && f != stdin) {
        fclose(f);
        std::lock_guard<std::mutex> l(g_file_lock);
        g_files.erase(c.r(0));
        mem::free(c.r(0));
    }
    c.ret(0);
}
HLE(fread) {
    FILE* f = host_file(c.r(3));
    c.ret(f ? (u32)fread(ptr(c.r(0)), 1, (size_t)c.r(1) * c.r(2), f) / std::max<u32>(c.r(1), 1) : 0);
}
HLE(fwrite) {
    FILE* f = host_file(c.r(3));
    c.ret(f ? (u32)fwrite(ptr(c.r(0)), 1, (size_t)c.r(1) * c.r(2), f) / std::max<u32>(c.r(1), 1) : 0);
}
HLE(fseek) {
    FILE* f = host_file(c.r(0));
    c.ret(f ? (u32)_fseeki64(f, (s32)c.r(1), (int)c.r(2)) : (u32)-1);
}
HLE(fseeko) { hle_fn_fseek(c); }
HLE(ftell) {
    FILE* f = host_file(c.r(0));
    c.ret(f ? (u32)_ftelli64(f) : (u32)-1);
}
HLE(ftello) { hle_fn_ftell(c); }
HLE(rewind) { if (FILE* f = host_file(c.r(0))) rewind(f); }
HLE(feof) { FILE* f = host_file(c.r(0)); c.ret(f && feof(f) ? 1 : 0); }
HLE(ferror) { FILE* f = host_file(c.r(0)); c.ret(f && ferror(f) ? 1 : 0); }
HLE(clearerr) { if (FILE* f = host_file(c.r(0))) clearerr(f); }
HLE(fflush) { if (FILE* f = host_file(c.r(0))) fflush(f); else fflush(stdout); c.ret(0); }
HLE(setvbuf) { c.ret(0); }
HLE(fgets) {
    FILE* f = host_file(c.r(2));
    c.ret(f && fgets(ptr<char>(c.r(0)), (int)c.r(1), f) ? c.r(0) : 0);
}
HLE(getc) { FILE* f = host_file(c.r(0)); c.ret(f ? (u32)getc(f) : (u32)-1); }
HLE(fgetc) { hle_fn_getc(c); }
HLE(ungetc) { FILE* f = host_file(c.r(1)); c.ret(f ? (u32)ungetc((int)c.r(0), f) : (u32)-1); }
HLE(putc) { FILE* f = host_file(c.r(1)); c.ret(f ? (u32)putc((int)c.r(0), f) : (u32)-1); }
HLE(fputc) { hle_fn_putc(c); }
HLE(fputs) { FILE* f = host_file(c.r(1)); c.ret(f ? (u32)fputs(str(c.r(0)), f) : (u32)-1); }
HLE(puts) { puts(str(c.r(0))); c.ret(1); }
HLE(putchar) { putchar((int)c.r(0)); c.ret(c.r(0)); }
HLE(fprintf) {
    RegArgs a(c, 2);
    std::string s = format(str(c.r(1)), a);
    FILE* f = host_file(c.r(0));
    if (f) fputs(s.c_str(), f);
    c.ret((u32)s.size());
}
HLE(vfprintf) {
    VaArgs a(c.r(2));
    std::string s = format(str(c.r(1)), a);
    FILE* f = host_file(c.r(0));
    if (f) fputs(s.c_str(), f);
    c.ret((u32)s.size());
}
// Wide stream I/O: read/write one UTF-32 unit as a byte (C locale)
HLE(getwc) { FILE* f = host_file(c.r(0)); c.ret(f ? (u32)getc(f) : (u32)-1); }
HLE(ungetwc) { FILE* f = host_file(c.r(1)); c.ret(f ? (u32)ungetc((int)c.r(0), f) : (u32)-1); }
HLE(putwc) { FILE* f = host_file(c.r(1)); c.ret(f ? (u32)putc((int)c.r(0), f) : (u32)-1); }

// ============================================================================
// Time
// ============================================================================
static u64 now_ns_monotonic() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (u64)((double)t.QuadPart * 1e9 / (double)freq.QuadPart);
}
static u64 now_ns_realtime() {
    using namespace std::chrono;
    return (u64)duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
}

HLE(clock_gettime) {
    u64 ns = c.r(0) == 0 ? now_ns_realtime() : now_ns_monotonic();
    mem::w32(c.r(1), (u32)(ns / 1000000000ull));
    mem::w32(c.r(1) + 4, (u32)(ns % 1000000000ull));
    c.ret(0);
}
HLE(gettimeofday) {
    u64 ns = now_ns_realtime();
    if (c.r(0)) {
        mem::w32(c.r(0), (u32)(ns / 1000000000ull));
        mem::w32(c.r(0) + 4, (u32)((ns / 1000) % 1000000ull));
    }
    c.ret(0);
}
HLE(time) {
    u32 t = (u32)(now_ns_realtime() / 1000000000ull);
    if (c.r(0)) mem::w32(c.r(0), t);
    c.ret(t);
}
HLE(clock) { c.ret((u32)(now_ns_monotonic() / 1000)); }  // CLOCKS_PER_SEC = 1000000 on bionic
HLE(usleep) {
    // Worker threads poll with usleep(100); a bare yield would make them spin at full speed.
    // (Windows sleeps have ~1 ms granularity; main() raises the timer resolution to 1 ms.)
    u32 us = c.r(0);
    if (us == 0) std::this_thread::yield();
    else std::this_thread::sleep_for(std::chrono::microseconds(us));
    c.ret(0);
}
HLE(nanosleep) {
    u32 sec = mem::r32(c.r(0)), ns = mem::r32(c.r(0) + 4);
    std::this_thread::sleep_for(std::chrono::seconds(sec) + std::chrono::nanoseconds(ns));
    c.ret(0);
}
HLE(sleep) { std::this_thread::sleep_for(std::chrono::seconds(c.r(0))); c.ret(0); }

// struct tm: 9 ints, long tm_gmtoff, const char* tm_zone (44 bytes)
static void tm_to_guest(u32 g, const struct tm& t) {
    const int v[9] = {t.tm_sec, t.tm_min, t.tm_hour, t.tm_mday, t.tm_mon, t.tm_year, t.tm_wday, t.tm_yday, t.tm_isdst};
    for (int i = 0; i < 9; i++) mem::w32(g + i * 4, (u32)v[i]);
    mem::w32(g + 36, 0);
    static u32 zone = mem::strdup("UTC");
    mem::w32(g + 40, zone);
}
static struct tm tm_from_guest(u32 g) {
    struct tm t = {};
    int* f[9] = {&t.tm_sec, &t.tm_min, &t.tm_hour, &t.tm_mday, &t.tm_mon, &t.tm_year, &t.tm_wday, &t.tm_yday, &t.tm_isdst};
    for (int i = 0; i < 9; i++) *f[i] = (int)mem::r32(g + i * 4);
    return t;
}
static thread_local u32 t_tm_buf;
static u32 tm_buf() { if (!t_tm_buf) t_tm_buf = mem::calloc(1, 48); return t_tm_buf; }

HLE(localtime) {
    __time64_t t = (s32)mem::r32(c.r(0));
    struct tm h;
    _localtime64_s(&h, &t);
    tm_to_guest(tm_buf(), h);
    c.ret(tm_buf());
}
HLE(localtime_r) {
    __time64_t t = (s32)mem::r32(c.r(0));
    struct tm h;
    _localtime64_s(&h, &t);
    tm_to_guest(c.r(1), h);
    c.ret(c.r(1));
}
HLE(gmtime) {
    __time64_t t = (s32)mem::r32(c.r(0));
    struct tm h;
    _gmtime64_s(&h, &t);
    tm_to_guest(tm_buf(), h);
    c.ret(tm_buf());
}
HLE(mktime) {
    struct tm t = tm_from_guest(c.r(0));
    c.ret((u32)_mktime64(&t));
}
HLE(ctime) {
    static thread_local u32 buf = 0;
    if (!buf) buf = mem::malloc(32);
    __time64_t t = (s32)mem::r32(c.r(0));
    char tmp[32];
    _ctime64_s(tmp, sizeof tmp, &t);
    strcpy(ptr<char>(buf), tmp);
    c.ret(buf);
}
HLE(strftime) {
    struct tm t = tm_from_guest(c.r(3));
    char tmp[512];
    size_t n = strftime(tmp, sizeof tmp, str(c.r(2)), &t);
    if (n >= c.r(1)) { c.ret(0); return; }
    memcpy(ptr(c.r(0)), tmp, n + 1);
    c.ret((u32)n);
}
HLE(wcsftime) {
    struct tm t = tm_from_guest(c.r(3));
    std::string f = mem::wstr_utf8(c.r(2));
    char tmp[512];
    size_t n = strftime(tmp, sizeof tmp, f.c_str(), &t);
    if (n >= c.r(1)) { c.ret(0); return; }
    for (size_t i = 0; i <= n; i++) mem::w32(c.r(0) + (u32)i * 4, (u8)tmp[i]);
    c.ret((u32)n);
}

// ============================================================================
// Networking: not supported (the game's online features are long dead)
// ============================================================================
#define NET_FAIL(n) HLE(n) { set_errno(c, 100 /* ENETDOWN */); c.ret((u32)-1); }
NET_FAIL(socket)
NET_FAIL(connect)
NET_FAIL(bind)
NET_FAIL(listen)
NET_FAIL(accept)
NET_FAIL(send)
NET_FAIL(recv)
NET_FAIL(setsockopt)
HLE(gethostbyname) { c.ret(0); }
HLE(inet_addr) { c.ret(0xffffffff); }
HLE(select) {
    // Only used for sleeping/network waits: honour the timeout and report nothing ready.
    u32 tv = c.arg(4);
    if (tv) {
        u32 us = mem::r32(tv) * 1000000 + mem::r32(tv + 4);
        std::this_thread::sleep_for(std::chrono::microseconds(std::min<u32>(us, 100000)));
    }
    c.ret(0);
}
HLE(poll) {
    s32 timeout = (s32)c.r(2);
    if (timeout > 0) std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout, 100)));
    c.ret(0);
}
