#include "kvstore.hpp"
#include "persistence.hpp"
#include "threadpool.hpp"
#include "server.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <csignal>
#include <atomic>

std::atomic<bool> g_running{true};
Server* g_serverPtr = nullptr;

void handleSignal(int) {
    g_running = false;
    if (g_serverPtr) g_serverPtr->stop();
}

int main(int argc, char* argv[]) {
    int port = 6380; // deliberately not 6379 (real Redis's port), to avoid clashes
    if (argc > 1) port = std::atoi(argv[1]);

    KVStore store;
    Persistence persistence("./data");

    std::cout << "[startup] loading data from disk...\n";
    persistence.loadInto(store);
    std::cout << "[startup] loaded " << store.size() << " keys\n";

    ThreadPool pool(/*numThreads=*/8);
    Server server(port, store, persistence, pool);
    g_serverPtr = &server;

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);

    // Background maintenance thread: reap expired keys every second, and
    // compact the WAL into a snapshot every 30 seconds. Runs independently
    // of client connections so maintenance never blocks/queues behind them.
    std::thread maintenance([&store, &persistence] {
        int tickCount = 0;
        while (g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            store.reapExpired();
            tickCount++;
            if (tickCount % 30 == 0) {
                persistence.compact(store);
            }
        }
    });

    server.run(); // blocks until server.stop() is called (e.g. Ctrl+C)

    g_running = false;
    maintenance.join();

    std::cout << "[shutdown] compacting before exit...\n";
    persistence.compact(store);
    std::cout << "[shutdown] done\n";
    return 0;
}
