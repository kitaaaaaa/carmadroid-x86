#pragma once
// Guest file descriptors: host files and in-process pipes (used by android_native_app_glue).
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace fds {

struct Pipe {
    std::mutex m;
    std::condition_variable cv;
    std::deque<unsigned char> buf;
};

struct Fd {
    int host = -1;
    std::shared_ptr<Pipe> pipe;
};

extern std::mutex g_lock;
extern std::condition_variable g_any_data;  // wait with g_lock held

int add(Fd f);
bool get(int fd, Fd& out);
bool readable(int fd);

}  // namespace fds
