#include "file_size_cache.h"
#include "game_file_size.h"
#include <stdio.h>

FileSizeCache::~FileSizeCache() {
    stop();
    pthread_cond_destroy(&changed_);
    pthread_mutex_destroy(&lock_);
}

void FileSizeCache::invalidate_locked() {
    ++generation_;
    value_ = {};
    wanted_ = queued_ = false;
}

void FileSizeCache::reset(const std::string &root) {
    pthread_mutex_lock(&lock_);
    root_ = root;
    suspended_ = false;
    invalidate_locked();
    pthread_mutex_unlock(&lock_);
}

void FileSizeCache::invalidate() {
    pthread_mutex_lock(&lock_);
    invalidate_locked();
    pthread_mutex_unlock(&lock_);
}

void FileSizeCache::set_suspended(bool suspended) {
    pthread_mutex_lock(&lock_);
    if (suspended != suspended_) { suspended_ = suspended; invalidate_locked(); }
    pthread_mutex_unlock(&lock_);
}

FileSizeCache::Value FileSizeCache::get() {
    pthread_mutex_lock(&lock_);
    if (!suspended_ && !wanted_) {
        wanted_ = true;
        if (!started_) {
            int error = pthread_create(&thread_, NULL, run, this);
            if (error) {
                value_.state = State::Unavailable;
                fprintf(stderr, "[launcher] size worker could not start (%d)\n", error);
            } else started_ = true;
        }
        if (started_) { queued_ = true; pthread_cond_signal(&changed_); }
    }
    Value value = value_;
    pthread_mutex_unlock(&lock_);
    return value;
}

bool FileSizeCache::cancelled(void *data) {
    Attempt *attempt = static_cast<Attempt *>(data);
    return attempt->cache->generation_.load() != attempt->generation;
}

void *FileSizeCache::run(void *data) {
    FileSizeCache *cache = static_cast<FileSizeCache *>(data);
    pthread_mutex_lock(&cache->lock_);
    while (!cache->stopping_) {
        while (!cache->queued_ && !cache->stopping_) pthread_cond_wait(&cache->changed_, &cache->lock_);
        if (cache->stopping_) break;
        std::string root = cache->root_;
        Attempt attempt{cache, cache->generation_.load()};
        cache->queued_ = false;
        pthread_mutex_unlock(&cache->lock_);
        RtGameFilesSize result = rt_game_files_size_scan(root.c_str(), cancelled, &attempt);
        pthread_mutex_lock(&cache->lock_);
        if (!cancelled(&attempt) && !cache->stopping_) {
            cache->value_ = {result.complete && !result.cancelled ? State::Ready : State::Unavailable, result.bytes};
            if (!result.complete && !result.cancelled) fprintf(stderr, "[launcher] game-file size is incomplete; no total shown\n");
        }
    }
    pthread_mutex_unlock(&cache->lock_);
    return NULL;
}

void FileSizeCache::stop() {
    pthread_mutex_lock(&lock_);
    stopping_ = true;
    invalidate_locked();
    pthread_cond_signal(&changed_);
    pthread_mutex_unlock(&lock_);
    if (started_) pthread_join(thread_, NULL);
    started_ = stopping_ = false;
}
