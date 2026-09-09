#pragma once
#include "kvstore.hpp"
#include "persistence.hpp"
#include <string>
#include <sstream>
#include <vector>

// Parses one line of client input and executes it against the store.
// Protocol is intentionally simple and text-based (like early Redis):
//
//   SET key value      -> +OK
//   GET key            -> value line, or (nil)
//   DEL key            -> :1 (deleted) or :0 (not found)
//   EXISTS key         -> :1 or :0
//   EXPIRE key seconds -> +OK or -ERR no such key
//   PING               -> +PONG
//
// Limitation (worth mentioning in an interview as a known trade-off):
// values can't contain spaces in this simple protocol, since we split on
// whitespace. A production protocol (like RESP) length-prefixes each
// argument instead, so arbitrary bytes -- including spaces -- are safe.
class Protocol {
public:
    Protocol(KVStore& store, Persistence& persistence)
        : store_(store), persistence_(persistence) {}

    std::string handleLine(const std::string& line) {
        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;
        for (auto& c : cmd) c = toupper(c);

        if (cmd == "SET") {
            std::string key, value;
            iss >> key >> value;
            if (key.empty() || value.empty()) return "-ERR usage: SET key value\r\n";
            store_.set(key, value);
            persistence_.logSet(key, value); // durability: log before we ack
            return "+OK\r\n";
        }
        if (cmd == "GET") {
            std::string key;
            iss >> key;
            auto val = store_.get(key);
            if (!val.has_value()) return "$-1\r\n"; // nil, Redis-style sentinel
            return "$" + val.value() + "\r\n";
        }
        if (cmd == "DEL") {
            std::string key;
            iss >> key;
            bool removed = store_.del(key);
            if (removed) persistence_.logDel(key);
            return std::string(":") + (removed ? "1" : "0") + "\r\n";
        }
        if (cmd == "EXISTS") {
            std::string key;
            iss >> key;
            return std::string(":") + (store_.exists(key) ? "1" : "0") + "\r\n";
        }
        if (cmd == "EXPIRE") {
            std::string key, secStr;
            iss >> key >> secStr;
            if (key.empty() || secStr.empty()) return "-ERR usage: EXPIRE key seconds\r\n";
            int seconds = std::stoi(secStr);
            bool ok = store_.expire(key, seconds);
            return ok ? "+OK\r\n" : "-ERR no such key\r\n";
        }
        if (cmd == "PING") {
            return "+PONG\r\n";
        }
        if (cmd == "") {
            return ""; // blank line, ignore
        }
        return "-ERR unknown command '" + cmd + "'\r\n";
    }

private:
    KVStore& store_;
    Persistence& persistence_;
};
