#include "Api.h"
#include "Miner.h"
#include "CpuInfo.h"
#include "Config.h"
#include "Log.h"
#include "Platform.h"
#include "crypto/RandomX.h"
#include "version.h"

#include <cstring>
#include <sstream>
#include <iomanip>

namespace zenrx {

static std::string jsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

Api::Api(const Config* config, Miner* miner)
    : m_config(config)
    , m_miner(miner)
{
    if (config) {
        m_host = config->apiHost;
        m_port = config->apiPort;
    }
}

Api::~Api()
{
    stop();
}

void Api::start()
{
    if (m_running) return;

    if (!initWinsock()) {
        Log::error("API: Failed to initialize Winsock");
        return;
    }
    
    // Create socket
    m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (m_serverFd == SOCKET_INVALID) {
        Log::error("API: Failed to create socket");
        return;
    }
    
    // Allow reuse
    int opt = 1;
#ifdef _WIN32
    setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif
    
    // Bind
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(m_port);
    
    if (m_host == "0.0.0.0") {
        addr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, m_host.c_str(), &addr.sin_addr);
    }
    
    if (bind(m_serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        Log::error("API: Failed to bind to %s:%d", m_host.c_str(), m_port);
        close_socket(m_serverFd);
        m_serverFd = SOCKET_INVALID;
        return;
    }
    
    // Listen
    if (listen(m_serverFd, 5) < 0) {
        Log::error("API: Failed to listen");
        close_socket(m_serverFd);
        m_serverFd = SOCKET_INVALID;
        return;
    }
    
    m_running = true;
    m_thread = std::thread(&Api::run, this);
    
    Log::info("API listening on http://%s:%d", m_host.c_str(), m_port);
}

void Api::stop()
{
    m_running = false;
    
    if (m_serverFd != SOCKET_INVALID) {
        close_socket(m_serverFd);
        m_serverFd = SOCKET_INVALID;
    }
    
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void Api::run()
{
    while (m_running) {
        struct sockaddr_in clientAddr{};
        socklen_t clientLen = sizeof(clientAddr);
        
        socket_t clientFd = accept(m_serverFd, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientFd == SOCKET_INVALID) {
            if (m_running) {
                // Timeout or interrupted, continue
            }
            continue;
        }
        
        // Set recv timeout
#ifdef _WIN32
        DWORD timeout = 1000; // milliseconds
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
#else
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
        
        handleRequest(clientFd);
        close_socket(clientFd);
    }
}

void Api::handleRequest(socket_t clientFd)
{
    char buffer[4096];
    int n = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (n <= 0) return;
    
    buffer[n] = '\0';
    
    // Parse HTTP request
    std::string request(buffer);
    std::string method, path;
    
    size_t methodEnd = request.find(' ');
    if (methodEnd != std::string::npos) {
        method = request.substr(0, methodEnd);
        size_t pathEnd = request.find(' ', methodEnd + 1);
        if (pathEnd != std::string::npos) {
            path = request.substr(methodEnd + 1, pathEnd - methodEnd - 1);
        }
    }
    
    // Generate response
    std::string json = generateResponse(path, method);
    
    // Send HTTP response
    std::stringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: application/json\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Access-Control-Allow-Methods: GET\r\n";
    response << "Access-Control-Allow-Headers: Content-Type\r\n";
    response << "Content-Length: " << json.length() << "\r\n";
    response << "Connection: close\r\n";
    response << "\r\n";
    response << json;
    
    std::string resp = response.str();
    send(clientFd, resp.c_str(), resp.length(), 0);
}

std::string Api::generateResponse(const std::string& path, const std::string& method)
{
    // Only support /api endpoint
    if (path == "/api" || path == "/") {
        return getSummaryJson();
    }
    
    // Return 404 for unknown paths
    return "{\"error\": \"not found\"}";
}

std::string Api::getSummaryJson()
{
    const auto& cpuInfo = cpu();

    std::stringstream ss;
    ss << std::fixed << std::setprecision(2);

    bool minerReady = m_miner && m_miner->isReady();
    double hashrate = minerReady ? m_miner->hashrate() : 0.0;
    uint64_t accepted = minerReady ? m_miner->accepted() : 0;
    uint64_t rejected = minerReady ? m_miner->rejected() : 0;
    uint64_t uptime = m_miner ? m_miner->uptime() : 0;
    uint64_t diff = minerReady ? m_miner->currentDiff() : 0;

    std::string algo = "rx/0";
    bool tls = false;
    bool autotuning = m_config && m_config->autotuning;

    if (autotuning && m_config->autotuneAlgo) {
        algo = m_config->autotuneAlgo;
    } else if (minerReady) {
        algo = rxAlgoName(m_miner->currentAlgo());
    }

    bool msrEnabled = m_config ? m_config->msrEnabled : false;
    bool hugePagesOk = m_config ? m_config->hugePagesEnabled : false;
    if (hugePagesOk && minerReady) {
        hugePagesOk = randomx().allHugePagesEnabled();
    }

    if (minerReady && m_miner->getClient()) {
        tls = m_miner->getClient()->currentPool().tls;
    }

    ss << "{\n";
    ss << "  \"id\": \"" << APP_ID << "\",\n";
    ss << "  \"worker_id\": \"" << APP_ID << "\",\n";
    ss << "  \"uptime\": " << uptime << ",\n";
    ss << "  \"connection\": {\n";
    ss << "    \"algo\": \"" << algo << "\",\n";
    ss << "    \"diff\": " << diff << ",\n";
    ss << "    \"accepted\": " << accepted << ",\n";
    ss << "    \"rejected\": " << rejected << ",\n";
    ss << "    \"tls\": " << (tls ? "true" : "false") << "\n";
    ss << "  },\n";
    ss << "  \"version\": \"" << APP_VERSION << "\",\n";
    ss << "  \"ua\": \"" << APP_VERSION_FULL << "\",\n";
    ss << "  \"cpu\": {\n";
    ss << "    \"brand\": \"" << cpuInfo.brand() << "\",\n";
    ss << "    \"cores\": " << cpuInfo.cores() << ",\n";
    ss << "    \"threads\": " << cpuInfo.threads() << ",\n";
    int usedThreads = 0;
    if (autotuning && m_config && m_config->autotuneAlgo) {
        usedThreads = m_config->threadsForAlgo(parseRxAlgo(m_config->autotuneAlgo));
    } else if (m_miner) {
        usedThreads = static_cast<int>(m_miner->workerCount());
    }
    ss << "    \"used_threads\": " << usedThreads << "\n";
    ss << "  },\n";
    ss << "  \"hashrate\": " << (hashrate / 1000.0) << ",\n";
    ss << "  \"msr\": " << (msrEnabled ? "true" : "false") << ",\n";
    ss << "  \"hugepages\": " << (hugePagesOk ? "true" : "false") << ",\n";
    ss << "  \"autotune\": " << (autotuning ? "true" : "false") << "\n";
    ss << "}";

    return ss.str();
}

} // namespace zenrx
