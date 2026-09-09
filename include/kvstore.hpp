#pragma once
#include <string>
#include <unordered_map>
#include <shared_mutex>
#include <mutex>
#include <optional>
#include <chrono>
#include <vector>

// The in-memory storage engine.
//
// Concurrency design: we use a shared_mutex (readers-writer lock) instead of
// a plain mutex. GET is far more common than SET in most workloads, and
// shared_mutex lets many GETs happen in parallel (shared/read lock) while
// still giving SET/DEL exclusive access (unique/write lock) when needed.
// This is a real trade-off worth explaining in an interview: plain mutex is
// simpler and faster under low contention, shared_mutex wins when reads
// dominate and you have multiple cores actually contending.
class KVStore {
public:
    struct Entry {
        std::string value;
        // No expiry = epoch{} i.e. time_point's default (treated as "never").
        std::chrono::steady_clock::time_point expiresAt{};
        bool hasExpiry = false;
    };

    void set(const std::string& key, const std::string& value) {
        std::unique_lock lock(mutex_);
        data_[key] = Entry{value, {}, false};
    }

    // Returns std::nullopt if key doesn't exist or has expired.
    std::optional<std::string> get(const std::string& key) {
        {
            std::shared_lock lock(mutex_);
            auto it = data_.find(key);
            if (it == data_.end()) return std::nullopt;
            if (isExpiredLocked(it->second)) {
                // fall through to remove it under a write lock below
            } else {
                return it->second.value;
            }
        }
        // Lazy expiry: key was expired, remove it now (needs write lock).
        std::unique_lock lock(mutex_);
        data_.erase(key);
        return std::nullopt;
    }

    bool del(const std::string& key) {
        std::unique_lock lock(mutex_);
        return data_.erase(key) > 0;
    }

    bool exists(const std::string& key) {
        std::shared_lock lock(mutex_);
        auto it = data_.find(key);
        if (it == data_.end()) return false;
        return !isExpiredLocked(it->second);
    }

    // Set a TTL (in seconds) on an existing key. Returns false if key missing.
    bool expire(const std::string& key, int seconds) {
        std::unique_lock lock(mutex_);
        auto it = data_.find(key);
        if (it == data_.end()) return false;
        it->second.hasExpiry = true;
        it->second.expiresAt = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        return true;
    }

    size_t size() {
        std::shared_lock lock(mutex_);
        return data_.size();
    }

    // Used by the persistence layer to snapshot everything to disk.
    // Returns a copy to avoid holding the lock during file I/O.
    std::unordered_map<std::string, Entry> snapshotAll() {
        std::shared_lock lock(mutex_);
        return data_; // copy
    }

    // Used when replaying the WAL / loading a snapshot at startup.
    void loadRaw(const std::string& key, const Entry& entry) {
        std::unique_lock lock(mutex_);
        data_[key] = entry;
    }

    // Background reaper calls this periodically to proactively sweep
    // expired keys, instead of relying purely on lazy expiry from GET.
    // Without this, keys that are set-and-forgotten (never GET'd again)
    // would sit in memory forever even after "expiring" logically.
    void reapExpired() {
        std::unique_lock lock(mutex_);
        auto now = std::chrono::steady_clock::now();
        for (auto it = data_.begin(); it != data_.end(); ) {
            if (it->second.hasExpiry && it->second.expiresAt <= now) {
                it = data_.erase(it);
            } else {
                ++it;
            }
        }
    }

private:
    bool isExpiredLocked(const Entry& e) const {
        if (!e.hasExpiry) return false;
        return std::chrono::steady_clock::now() >= e.expiresAt;
    }

    std::unordered_map<std::string, Entry> data_;
    mutable std::shared_mutex mutex_;
};
