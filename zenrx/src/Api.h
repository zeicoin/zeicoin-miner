#pragma once

#include <cstdint>
#include <string>
#include <atomic>
#include <thread>

#include "Platform.h"

namespace zenrx {

class Miner;
class Config;

class Api {
public:
    Api(const Config* config, Miner* miner);
    ~Api();
    
    void start();
    void stop();
    
    bool isRunning() const { return m_running; }

private:
    void run();
    void handleRequest(socket_t clientFd);
    std::string generateResponse(const std::string& path, const std::string& method);
    std::string getSummaryJson();
    
    std::string m_host = "127.0.0.1";
    uint16_t m_port = 16000;
    const Config* m_config;
    Miner* m_miner;
    
    socket_t m_serverFd = SOCKET_INVALID;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
};

} // namespace zenrx
