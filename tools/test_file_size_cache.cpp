// A held scanner proves requests/invalidation never wait for filesystem work.
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include "game_file_size.h"

static bool fail_create;
static int create_thread(pthread_t *thread, const pthread_attr_t *attr, void *(*run)(void *), void *arg) {
    return fail_create ? EAGAIN : pthread_create(thread, attr, run, arg);
}
#define pthread_create create_thread
#include "../runtime/ui/file_size_cache.mm"
#undef pthread_create

static std::mutex lock;
static std::condition_variable changed;
static unsigned entered, released;
static bool incomplete;
static bool ignore_cancel;
static std::string scanned_root;
extern "C" RtGameFilesSize rt_game_files_size_scan(const char *root, bool (*cancelled)(void *), void *user) {
    std::unique_lock<std::mutex> guard(lock);
    unsigned call = ++entered;
    scanned_root = root;
    changed.notify_all();
    while (released < call && (ignore_cancel || !cancelled(user))) changed.wait_for(guard, std::chrono::milliseconds(1));
    if (!ignore_cancel && cancelled(user)) return {0, false, true};
    return {1000 + call, !incomplete, false};
}
static void wait_entered(unsigned n) {
    std::unique_lock<std::mutex> guard(lock);
    assert(changed.wait_for(guard, std::chrono::seconds(3), [=]{ return entered >= n; }));
}
static void release(unsigned n) {
    std::lock_guard<std::mutex> guard(lock); released = n; changed.notify_all();
}
static FileSizeCache::Value wait_ready(FileSizeCache &cache) {
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < until) {
        auto value = cache.get();
        if (value.state != FileSizeCache::State::Pending) return value;
        std::this_thread::yield();
    }
    assert(false); return {};
}
int main() {
    FileSizeCache cache;
    cache.reset("first");
    assert(cache.get().state == FileSizeCache::State::Pending);
    wait_entered(1);
    for (int i=0; i<1000; ++i) assert(cache.get().state == FileSizeCache::State::Pending);
    { std::lock_guard<std::mutex> guard(lock); assert(entered == 1); }
    release(1);
    auto value = wait_ready(cache);
    assert(value.state == FileSizeCache::State::Ready && value.bytes == 1001);
    assert(cache.get().bytes == value.bytes);  // the other page shares the same result

    { std::lock_guard<std::mutex> guard(lock); ignore_cancel = true; }
    cache.invalidate(); cache.get(); wait_entered(2);
    cache.reset("second"); cache.get(); release(2); wait_entered(3);
    { std::lock_guard<std::mutex> guard(lock); assert(scanned_root == "second"); }
    assert(cache.get().state == FileSizeCache::State::Pending);
    { std::lock_guard<std::mutex> guard(lock); ignore_cancel = false; }
    release(3); value = wait_ready(cache); assert(value.bytes == 1003);

    cache.set_suspended(true);
    for (int i=0; i<1000; ++i) assert(cache.get().state == FileSizeCache::State::Pending);
    { std::lock_guard<std::mutex> guard(lock); assert(entered == 3); }
    cache.set_suspended(false); cache.get(); wait_entered(4);
    { std::lock_guard<std::mutex> guard(lock); incomplete = true; }
    release(4); value = wait_ready(cache); assert(value.state == FileSizeCache::State::Unavailable);
    { std::lock_guard<std::mutex> guard(lock); incomplete = false; }

    cache.invalidate(); cache.get(); wait_entered(5);
    cache.stop();  // cancellation releases a scanner even though the test hasn't released it
    cache.reset("third"); cache.get(); wait_entered(6); release(6);
    assert(wait_ready(cache).bytes == 1006);
    cache.stop();
    fail_create = true; cache.reset("failure");
    assert(cache.get().state == FileSizeCache::State::Unavailable);
    for (int i=0; i<100; ++i) assert(cache.get().state == FileSizeCache::State::Unavailable);
    cache.stop(); fail_create = false;
    printf("file-size cache: held scan, reuse, invalidation, root switch, suspension, error, cancellation and restart passed\n");
}
