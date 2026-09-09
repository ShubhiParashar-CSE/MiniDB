#pragma once
#include "threadpool.hpp"
#include "kvstore.hpp"
#include "persistence.hpp"
#include "protocol.hpp"

#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <iostream>
#include <atomic>
#include <thread>

// Listens for TCP connections and hands each accepted connection off to the
// thread pool. The accept loop itself runs on ONE dedicated thread so it's
// never blocked doing client I/O -- it just accepts and dispatches, fast.
class Server {
public:
    Server(int port, KVStore& store, Persistence& persistence, ThreadPool& pool)
        : port_(port), store_(store), persistence_(persistence), pool_(pool) {}

    void run() {
        listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) { perror("socket"); return; }

        // Allows immediate restart of the server on the same port without
        // waiting out the OS's TIME_WAIT period from the previous run.
        int opt = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port_);

        if (bind(listenFd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            perror("bind");
            return;
        }
        if (listen(listenFd_, /*backlog=*/128) < 0) {
            perror("listen");
            return;
        }

        std::cout << "[server] listening on port " << port_ << "\n";
        running_ = true;

        while (running_) {
            sockaddr_in clientAddr{};
            socklen_t clientLen = sizeof(clientAddr);
            int clientFd = accept(listenFd_, (sockaddr*)&clientAddr, &clientLen);
            if (clientFd < 0) {
                if (running_) perror("accept");
                continue;
            }
            // Hand off to a worker thread -- the accept loop moves on
            // immediately and can accept the next connection right away.
            pool_.submit([this, clientFd] { handleClient(clientFd); });
        }
    }

    void stop() {
        running_ = false;
        if (listenFd_ >= 0) close(listenFd_);
    }

private:
    void handleClient(int clientFd) {
        Protocol protocol(store_, persistence_);
        char buf[4096];
        std::string pending; // holds a partial line across recv() calls

        while (true) {
            ssize_t n = recv(clientFd, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break; // client closed connection or error

            buf[n] = '\0';
            pending += buf;

            // TCP is a byte stream, not a message stream -- one recv() might
            // contain zero, one, or several lines. We split on '\n' and keep
            // any trailing partial line buffered for next time.
            size_t pos;
            while ((pos = pending.find('\n')) != std::string::npos) {
                std::string line = pending.substr(0, pos);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                pending.erase(0, pos + 1);

                std::string response = protocol.handleLine(line);
                if (!response.empty()) {
                    send(clientFd, response.c_str(), response.size(), 0);
                }
            }
        }
        close(clientFd);
    }

    int port_;
    int listenFd_ = -1;
    std::atomic<bool> running_{false};
    KVStore& store_;
    Persistence& persistence_;
    ThreadPool& pool_;
};
