#pragma once

#include "Config.h"
#include "Miner.h"
#include <memory>
#include <atomic>

namespace zenrx {

class Api;

class App {
public:
    App(int argc, char** argv);
    ~App();
    
    // Run the application (blocks until stopped)
    int run();
    
    // Stop the application
    void stop();
    
    // Signal handler
    static void signalHandler(int sig);

private:
    void printBanner();
    void printCpuInfo();
    void printSummary();
    
    void releaseHugePages();

    Config m_config;
    std::unique_ptr<Miner> m_miner;
    std::unique_ptr<Api> m_api;
    std::atomic<bool> m_running{false};
    std::atomic<int64_t> m_hugePagesOriginal{-1};      // Original 2MB nr_hugepages before we modified it
    std::atomic<int64_t> m_1gbHugePagesOriginal{-1};   // Original 1GB nr_hugepages before we modified it

    static App* s_instance;
    static std::atomic<bool> s_signaled;
};

} // namespace zenrx
