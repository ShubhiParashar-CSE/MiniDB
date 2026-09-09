#pragma once
#include "kvstore.hpp"
#include <fstream>
#include <sstream>
#include <iostream>
#include <mutex>
#include <filesystem>

// Persistence strategy: Write-Ahead Log (WAL) + periodic snapshots.
//
// Why a WAL? If we only kept data in memory, a crash or restart loses
// everything. The simplest fix -- write the whole dataset to disk after
// every command -- is far too slow (O(n) disk write per SET).
//
// Instead: every mutating command (SET/DEL/EXPIRE) is appended as a single
// line to a log file BEFORE we consider it "done". Appending is O(1) and
// sequential writes are fast even on spinning disks. On startup, we replay
// the log from the beginning to rebuild the exact in-memory state.
//
// Problem: the log grows forever. Fix: SNAPSHOT. Periodically, dump the
// full current state to a snapshot file and truncate the WAL to empty.
// On startup we load the snapshot first, THEN replay only the (small) WAL
// written since that snapshot. This is the same idea real databases
// (Redis RDB+AOF, Postgres checkpoints+WAL) use.
class Persistence {
public:
    Persistence(const std::string& dataDir) : dataDir_(dataDir) {
        std::filesystem::create_directories(dataDir_);
        walPath_ = dataDir_ + "/wal.log";
        snapshotPath_ = dataDir_ + "/snapshot.db";
    }

    // Call once at startup to rebuild state before serving any clients.
    void loadInto(KVStore& store) {
        loadSnapshot(store);
        replayWal(store);
    }

    // Append one mutating command to the WAL. Called on every SET/DEL/EXPIRE.
    // fsync-ing on every write is the "safe but slow" choice -- we do it so
    // that a crash right after we ack a write can't silently lose it. This
    // is a deliberate durability-vs-throughput trade-off worth discussing.
    void logSet(const std::string& key, const std::string& value) {
        appendLine("SET " + escape(key) + " " + escape(value));
    }
    void logDel(const std::string& key) {
        appendLine("DEL " + escape(key));
    }
    void logExpire(const std::string& key, int seconds) {
        appendLine("EXPIRE " + escape(key) + " " + std::to_string(seconds));
    }

    // Dump full state to snapshot.db, then truncate the WAL to zero bytes.
    // This is "compaction": the WAL only ever needs to hold writes that
    // happened SINCE the last snapshot.
    void compact(KVStore& store) {
        std::lock_guard<std::mutex> lock(fileMutex_);

        auto data = store.snapshotAll();
        std::string tmpPath = snapshotPath_ + ".tmp";
        {
            std::ofstream out(tmpPath, std::ios::trunc);
            for (auto& [key, entry] : data) {
                long long ttlRemaining = -1;
                if (entry.hasExpiry) {
                    auto now = std::chrono::steady_clock::now();
                    ttlRemaining = std::chrono::duration_cast<std::chrono::seconds>(
                        entry.expiresAt - now).count();
                    if (ttlRemaining < 0) continue; // already expired, don't persist
                }
                out << escape(key) << " " << escape(entry.value) << " " << ttlRemaining << "\n";
            }
        }
        // Atomic rename: readers never see a half-written snapshot file.
        std::filesystem::rename(tmpPath, snapshotPath_);

        // Truncate WAL now that everything in it is captured in the snapshot.
        std::ofstream(walPath_, std::ios::trunc).close();

        std::cout << "[persistence] compacted " << data.size() << " keys into snapshot\n";
    }

private:
    void appendLine(const std::string& line) {
        std::lock_guard<std::mutex> lock(fileMutex_);
        std::ofstream out(walPath_, std::ios::app);
        out << line << "\n";
        out.flush(); // ensure it hits the OS buffer immediately
    }

    void loadSnapshot(KVStore& store) {
        std::ifstream in(snapshotPath_);
        if (!in.is_open()) return; // no snapshot yet, that's fine on first run

        std::string line;
        while (std::getline(in, line)) {
            std::istringstream iss(line);
            std::string key, value, ttlStr;
            iss >> key >> value >> ttlStr;
            key = unescape(key);
            value = unescape(value);
            long long ttl = std::stoll(ttlStr);

            KVStore::Entry entry;
            entry.value = value;
            if (ttl >= 0) {
                entry.hasExpiry = true;
                entry.expiresAt = std::chrono::steady_clock::now() + std::chrono::seconds(ttl);
            }
            store.loadRaw(key, entry);
        }
    }

    void replayWal(KVStore& store) {
        std::ifstream in(walPath_);
        if (!in.is_open()) return;

        std::string line;
        while (std::getline(in, line)) {
            std::istringstream iss(line);
            std::string cmd;
            iss >> cmd;
            if (cmd == "SET") {
                std::string key, value;
                iss >> key >> value;
                store.set(unescape(key), unescape(value));
            } else if (cmd == "DEL") {
                std::string key;
                iss >> key;
                store.del(unescape(key));
            } else if (cmd == "EXPIRE") {
                std::string key, secStr;
                iss >> key >> secStr;
                store.expire(unescape(key), std::stoi(secStr));
            }
            // unknown/corrupt lines are silently skipped -- a real system
            // would log a warning and could checksum each line instead.
        }
    }

    // Our WAL format is space-delimited, so keys/values can't contain raw
    // spaces or newlines. We escape them so any string value is safe to store.
    static std::string escape(const std::string& s) {
        std::string out;
        for (char c : s) {
            if (c == ' ') out += "\\s";
            else if (c == '\n') out += "\\n";
            else if (c == '\\') out += "\\\\";
            else out += c;
        }
        if (out.empty()) out = "\\e"; // marker for empty string
        return out;
    }
    static std::string unescape(const std::string& s) {
        if (s == "\\e") return "";
        std::string out;
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\\' && i + 1 < s.size()) {
                char next = s[i + 1];
                if (next == 's') { out += ' '; ++i; continue; }
                if (next == 'n') { out += '\n'; ++i; continue; }
                if (next == '\\') { out += '\\'; ++i; continue; }
            }
            out += s[i];
        }
        return out;
    }

    std::string dataDir_;
    std::string walPath_;
    std::string snapshotPath_;
    std::mutex fileMutex_;
};
