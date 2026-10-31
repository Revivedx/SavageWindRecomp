// One lazy size calculation shared by the launcher's pages. Public methods run on the UI thread.
#pragma once
#include <atomic>
#include <pthread.h>
#include <string>

class FileSizeCache {
public:
    enum class State { Pending, Ready, Unavailable };
    struct Value { State state = State::Pending; unsigned long long bytes = 0; };
    ~FileSizeCache();
    void reset(const std::string &root);
    void invalidate();
    void set_suspended(bool suspended);
    Value get();
    void stop();
    FileSizeCache() = default;
    FileSizeCache(const FileSizeCache &) = delete;
    FileSizeCache &operator=(const FileSizeCache &) = delete;
private:
    pthread_mutex_t lock_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_cond_t changed_ = PTHREAD_COND_INITIALIZER;
    pthread_t thread_{};
    std::atomic<unsigned long long> generation_{0};
    std::string root_;
    Value value_;
    bool started_ = false, stopping_ = false, suspended_ = false, wanted_ = false, queued_ = false;
    void invalidate_locked();
    static void *run(void *);
    struct Attempt { FileSizeCache *cache; unsigned long long generation; };
    static bool cancelled(void *);
};
