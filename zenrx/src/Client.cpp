#include "Client.h"
#include "Log.h"
#include "Platform.h"
#include "version.h"

#include <cstring>
#include <sstream>
#include <iomanip>

#include <openssl/ssl.h>
#include <openssl/err.h>

namespace zenrx {

// Simple JSON helpers (for production, use a proper JSON library)
static std::string jsonGetString(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    
    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";
    
    // Skip whitespace
    pos++;
    while (pos < json.length() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    
    // Check if value is a string (starts with ")
    if (pos >= json.length() || json[pos] != '"') return "";
    
    size_t end = json.find('"', pos + 1);
    if (end == std::string::npos) return "";
    
    return json.substr(pos + 1, end - pos - 1);
}

// Get string from nested object like result.id
static std::string jsonGetNestedString(const std::string& json, const std::string& parent, const std::string& key)
{
    // Find the parent object
    std::string parentSearch = "\"" + parent + "\"";
    size_t parentPos = json.find(parentSearch);
    if (parentPos == std::string::npos) return "";
    
    // Find the opening brace of the parent object
    size_t bracePos = json.find('{', parentPos);
    if (bracePos == std::string::npos) return "";
    
    // Find matching closing brace (simple version - assumes no deeply nested objects)
    int depth = 1;
    size_t endBrace = bracePos + 1;
    while (endBrace < json.length() && depth > 0) {
        if (json[endBrace] == '{') depth++;
        else if (json[endBrace] == '}') depth--;
        endBrace++;
    }
    
    // Extract the parent object content
    std::string parentJson = json.substr(bracePos, endBrace - bracePos);
    
    // Now search for the key within this parent object
    return jsonGetString(parentJson, key);
}

static int64_t jsonGetInt(const std::string& json, const std::string& key)
{
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return 0;
    
    pos = json.find(':', pos);
    if (pos == std::string::npos) return 0;
    
    pos = json.find_first_not_of(" \t", pos + 1);
    if (pos == std::string::npos) return 0;
    
    // Skip if it's a string or object
    if (json[pos] == '"' || json[pos] == '{') return 0;
    
    try {
        return std::stoll(json.substr(pos));
    } catch (...) {
        return 0;
    }
}

static std::string bytesToHex(const uint8_t* data, size_t len)
{
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; i++) {
        ss << std::setw(2) << static_cast<int>(data[i]);
    }
    return ss.str();
}

Client::Client(bool silent)
    : m_silent(silent)
{
}

Client::~Client()
{
    stop();
    disconnect();
}

bool Client::tlsHandshake()
{
    Log::debug("TLS: Starting handshake with %s:%d", m_pool.host.c_str(), m_pool.port);

    m_sslCtx = SSL_CTX_new(TLS_client_method());
    if (!m_sslCtx) {
        unsigned long e = ERR_peek_last_error();
        if (!m_silent) Log::error("TLS: Failed to create SSL context (reason=%s)", ERR_reason_error_string(e));
        return false;
    }

    SSL_CTX_set_min_proto_version(m_sslCtx, TLS1_2_VERSION);
    SSL_CTX_set_verify(m_sslCtx, SSL_VERIFY_NONE, nullptr);

    m_readBio  = BIO_new(BIO_s_mem());
    m_writeBio = BIO_new(BIO_s_mem());
    Log::debug("TLS: Created memory BIOs (read=%p, write=%p)", (void*)m_readBio, (void*)m_writeBio);

    m_ssl = SSL_new(m_sslCtx);
    if (!m_ssl) {
        unsigned long e = ERR_peek_last_error();
        if (!m_silent) Log::error("TLS: Failed to create SSL object (reason=%s)", ERR_reason_error_string(e));
        BIO_free(m_readBio);
        BIO_free(m_writeBio);
        m_readBio = nullptr;
        m_writeBio = nullptr;
        SSL_CTX_free(m_sslCtx);
        m_sslCtx = nullptr;
        return false;
    }

    SSL_set_tlsext_host_name(m_ssl, m_pool.host.c_str());
    SSL_set_connect_state(m_ssl);
    SSL_set_bio(m_ssl, m_readBio, m_writeBio);  // SSL now owns the BIOs
    Log::debug("TLS: SSL object configured, starting handshake loop");

    // Blocking handshake loop using memory BIOs
    int step = 0;
    while (!SSL_is_init_finished(m_ssl)) {
        step++;
        int ret = SSL_do_handshake(m_ssl);
        int err = SSL_get_error(m_ssl, ret);
        Log::debug("TLS: Handshake step %d: ret=%d, err=%d", step, ret, err);

        // Flush any data SSL wants to send to the peer
        if (!flushTlsWrite()) {
            if (!m_silent) Log::error("TLS: Failed to send handshake data at step %d", step);
            SSL_free(m_ssl);
            m_ssl = nullptr;
            m_readBio = nullptr;
            m_writeBio = nullptr;
            SSL_CTX_free(m_sslCtx);
            m_sslCtx = nullptr;
            return false;
        }

        if (ret == 1) {
            break;  // Handshake complete
        }

        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            unsigned long e = ERR_peek_last_error();
            if (!m_silent) Log::error("TLS: Handshake failed at step %d (SSL err=%d, reason=%s)", step, err, ERR_reason_error_string(e));
            SSL_free(m_ssl);
            m_ssl = nullptr;
            m_readBio = nullptr;
            m_writeBio = nullptr;
            SSL_CTX_free(m_sslCtx);
            m_sslCtx = nullptr;
            return false;
        }

        // Read more data from the socket for the handshake
        char buf[4096];
        int n = ::recv(static_cast<int>(m_socket), buf, sizeof(buf), 0);
        Log::debug("TLS: Handshake recv returned %d bytes", n);
        if (n <= 0) {
            if (!m_silent) Log::error("TLS: Connection closed during handshake at step %d (recv=%d, errno=%d)", step, n, errno);
            SSL_free(m_ssl);
            m_ssl = nullptr;
            m_readBio = nullptr;
            m_writeBio = nullptr;
            SSL_CTX_free(m_sslCtx);
            m_sslCtx = nullptr;
            return false;
        }
        BIO_write(m_readBio, buf, n);
    }

    if (!m_silent) Log::info("TLS: Connected using %s", SSL_get_version(m_ssl));
    return true;
}

void Client::tlsClose()
{
    if (m_ssl) {
        SSL_shutdown(m_ssl);
        SSL_free(m_ssl);  // Also frees m_readBio and m_writeBio
        m_ssl = nullptr;
        m_readBio = nullptr;
        m_writeBio = nullptr;
    }
    if (m_sslCtx) {
        SSL_CTX_free(m_sslCtx);
        m_sslCtx = nullptr;
    }
    m_tls = false;
}

bool Client::flushTlsWrite()
{
    char buf[16384];
    int pending;

    while ((pending = BIO_read(m_writeBio, buf, sizeof(buf))) > 0) {
        int sent = 0;
        while (sent < pending) {
#ifdef _WIN32
            int n = ::send(m_socket, buf + sent, pending - sent, 0);
#else
            ssize_t n = ::send(m_socket, buf + sent, pending - sent, MSG_NOSIGNAL);
#endif
            if (n <= 0) {
                int err = SOCKET_ERROR_CODE;
                if (err == EAGAIN || err == EWOULDBLOCK
#ifdef _WIN32
                    || err == WSAEWOULDBLOCK
#endif
                ) {
                    std::this_thread::sleep_for(std::chrono::microseconds(1000));
                    continue;
                }
                return false;
            }
            sent += static_cast<int>(n);
        }
    }
    return true;
}

void Client::setPool(const PoolConfig& pool)
{
    m_pool = pool;
}

bool Client::connect()
{
    if (m_pool.host.empty()) {
        if (!m_silent) Log::error("No pool configured");
        return false;
    }

    m_running = true;

    while (m_running) {
        Log::debug("Connecting to %s:%d", m_pool.host.c_str(), m_pool.port);

        if (connect(m_pool)) {
            if (!m_silent) Log::info("Connected to %s:%d",
                      m_pool.host.c_str(), m_pool.port);
            return true;
        }

        Log::debug("Connection failed, retrying in 10s...");
        std::this_thread::sleep_for(std::chrono::seconds(10));
    }

    return false;
}

bool Client::connect(const PoolConfig& pool)
{
    m_pool = pool;

    if (!initWinsock()) {
        if (!m_silent) Log::error("Failed to initialize Winsock");
        return false;
    }
    
    // Resolve hostname
    struct addrinfo hints{}, *result;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    
    std::string portStr = std::to_string(pool.port);
    int err = getaddrinfo(pool.host.c_str(), portStr.c_str(), &hints, &result);
    if (err != 0) {
        if (!m_silent) Log::error("Failed to resolve %s: %s", pool.host.c_str(), gai_strerror(err));
        return false;
    }
    
    // Create socket
    m_socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (m_socket == SOCKET_INVALID) {
        if (!m_silent) Log::error("Failed to create socket");
        freeaddrinfo(result);
        return false;
    }

    // Enable TCP keepalive probes to detect dead connections at the OS level
    {
        int keepalive = 1;
        setsockopt(m_socket, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepalive), sizeof(keepalive));
#ifdef _WIN32
        struct tcp_keepalive ka{};
        ka.onoff = 1;
        ka.keepalivetime = 60000;      // 60s idle before first probe
        ka.keepaliveinterval = 10000;  // 10s between probes
        DWORD bytesReturned = 0;
        WSAIoctl(m_socket, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), nullptr, 0, &bytesReturned, nullptr, nullptr);
#else
        int idle = 60;   // 60s idle before first probe
        int intvl = 10;  // 10s between probes
        int cnt = 3;     // 3 probes before declaring dead
        setsockopt(m_socket, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(m_socket, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        setsockopt(m_socket, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
    }

    // Connect
    if (::connect(m_socket, result->ai_addr, static_cast<int>(result->ai_addrlen)) < 0) {
        if (!m_silent) Log::error("Failed to connect to %s:%d", pool.host.c_str(), pool.port);
        close_socket(m_socket);
        m_socket = SOCKET_INVALID;
        freeaddrinfo(result);
        return false;
    }
    
    freeaddrinfo(result);

    // TLS handshake (while socket is still blocking)
    m_tls = pool.tls;
    if (!m_silent) Log::info("Pool %s:%d tls=%s", pool.host.c_str(), pool.port, pool.tls ? "true" : "false");
    if (m_tls) {
        if (!tlsHandshake()) {
            close_socket(m_socket);
            m_socket = SOCKET_INVALID;
            return false;
        }
    }

    // Set non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(m_socket, FIONBIO, &mode);
#else
    int flags = fcntl(m_socket, F_GETFL, 0);
    fcntl(m_socket, F_SETFL, flags | O_NONBLOCK);
#endif

    m_connected = true;
    m_lastActivity = std::chrono::steady_clock::now();
    m_lastRecvTime = std::chrono::steady_clock::now();
    m_keepaliveInFlight = false;
    m_missedKeepalives = 0;

    // Login
    if (!login()) return false;

    // Auto-detect TLS: if server closes a plain connection immediately,
    // the port likely requires TLS — retry with TLS enabled
    if (!pool.tls) {
        struct pollfd pfd;
        pfd.fd = m_socket;
        pfd.events = POLLIN;
        int pollRet = poll(&pfd, 1, 2000);

        if (pollRet > 0) {
            bool closed = false;

            if (pfd.revents & (POLLERR | POLLHUP)) {
                closed = true;
            } else if (pfd.revents & POLLIN) {
                char peek;
                int n = recv(m_socket, &peek, 1, MSG_PEEK);
                if (n == 0) {
                    closed = true;
                }
            }

            if (closed) {
                if (!m_silent) Log::info("Server closed plain connection, retrying with TLS...");
                disconnect();

                PoolConfig tlsPool = pool;
                tlsPool.tls = true;
                m_pool.tls = true;  // Update for future reconnections
                return connect(tlsPool);
            }
        }
    }

    return true;
}

void Client::disconnect()
{
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        m_rpcId.clear();  // Reset for next login (under lock to avoid race with submit())
    }
    tlsClose();
    if (m_socket != SOCKET_INVALID) {
        close_socket(m_socket);
        m_socket = SOCKET_INVALID;
    }
    m_connected = false;
    m_authenticated = false;
}

bool Client::login()
{
    // Send login request
    std::stringstream ss;
    ss << "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"login\",\"params\":{";
    ss << "\"login\":\"" << m_pool.user << "\",";
    ss << "\"pass\":\"" << m_pool.pass << "\",";
    ss << "\"agent\":\"" << APP_VERSION_FULL << "\"";

    // MoneroOcean algo switching: send supported algorithms
    ss << ",\"algo\":[\"rx/0\",\"rx/wow\",\"rx/arq\",\"rx/xeq\",\"rx/graft\",\"rx/sfx\",\"rx/xla\"]";

    // Send algo-perf results if available
    if (!m_algoPerf.empty()) {
        ss << ",\"algo-perf\":{";
        bool first = true;
        for (const auto& kv : m_algoPerf) {
            if (!first) ss << ",";
            ss << "\"" << kv.first << "\":" << std::fixed << std::setprecision(1) << kv.second;
            first = false;
        }
        ss << "}";
    }

    // Send algo-min-time if set
    if (m_algoMinTime > 0) {
        ss << ",\"algo-min-time\":" << m_algoMinTime;
    }

    ss << "}}\n";

    return send(ss.str());
}

bool Client::send(const std::string& message)
{
    std::lock_guard<std::mutex> lock(m_sendMutex);

    if (m_socket == SOCKET_INVALID) return false;

    if (m_tls && m_ssl) {
        int ret = SSL_write(m_ssl, message.c_str(), static_cast<int>(message.length()));
        if (ret <= 0) {
            if (!m_silent) Log::error("TLS: Failed to encrypt message");
            return false;
        }
        if (!flushTlsWrite()) {
            if (!m_silent) Log::error("TLS: Failed to send encrypted data");
            return false;
        }
    } else {
        const char* data = message.c_str();
        size_t remaining = message.length();
        while (remaining > 0) {
#ifdef _WIN32
            int sent = ::send(m_socket, data, static_cast<int>(remaining), 0);
#else
            ssize_t sent = ::send(m_socket, data, remaining, MSG_NOSIGNAL);
#endif
            if (sent <= 0) {
                if (!m_silent) Log::error("Failed to send message");
                return false;
            }
            data += sent;
            remaining -= static_cast<size_t>(sent);
        }
    }
    
    // Update activity timestamp
    m_lastActivity = std::chrono::steady_clock::now();
    
    Log::debug("Sent: %s", message.c_str());
    return true;
}

static std::string nonceToHex(uint32_t nonce)
{
    // Convert nonce to little-endian hex string
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(2) << ((nonce >> 0) & 0xFF);
    ss << std::setw(2) << ((nonce >> 8) & 0xFF);
    ss << std::setw(2) << ((nonce >> 16) & 0xFF);
    ss << std::setw(2) << ((nonce >> 24) & 0xFF);
    return ss.str();
}

bool Client::submit(const JobResult& result)
{
    // Snapshot m_rpcId under m_sendMutex to avoid data race with disconnect()
    std::string rpcId;
    {
        std::lock_guard<std::mutex> lock(m_sendMutex);
        if (!m_authenticated || m_rpcId.empty()) {
            if (!m_silent) Log::warn("Cannot submit: not authenticated");
            return false;
        }
        rpcId = m_rpcId;
    }

    // Check for duplicate submission
    {
        std::lock_guard<std::mutex> lock(m_submitHistoryMutex);

        for (const auto& entry : m_submitHistory) {
            if (entry.first == result.jobId && entry.second == result.nonce) {
                Log::debug("Duplicate share detected, skipping (job: %s, nonce: %08x)",
                          result.jobId.c_str(), result.nonce);
                return false;
            }
        }

        // Add to history
        m_submitHistory.emplace_back(result.jobId, result.nonce);

        // Trim history if too large
        if (m_submitHistory.size() > MAX_SUBMIT_HISTORY) {
            m_submitHistory.erase(m_submitHistory.begin(),
                                  m_submitHistory.begin() + (m_submitHistory.size() - MAX_SUBMIT_HISTORY / 2));
        }
    }

    std::string nonceHex = nonceToHex(result.nonce);
    std::string hashHex = bytesToHex(result.hash.data(), 32);

    std::stringstream ss;
    ss << "{\"id\":" << m_submitId++ << ",\"jsonrpc\":\"2.0\",\"method\":\"submit\",\"params\":{";
    ss << "\"id\":\"" << rpcId << "\",";
    ss << "\"job_id\":\"" << result.jobId << "\",";
    ss << "\"nonce\":\"" << nonceHex << "\",";
    ss << "\"result\":\"" << hashHex << "\"";
    ss << "}}\n";

    Log::debug("Submit: job=%s nonce=%s", result.jobId.c_str(), nonceHex.c_str());
    return send(ss.str());
}

void Client::run()
{
    m_running = true;
    char buffer[4096];
    std::string lineBuffer;
    
    while (m_running) {
        if (!m_connected) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            reconnect();
            continue;
        }
        
        auto now = std::chrono::steady_clock::now();

        if (m_authenticated) {
            // Step 1: Check if an in-flight keepalive timed out
            if (m_keepaliveInFlight) {
                auto waitTime = std::chrono::duration_cast<std::chrono::seconds>(
                    now - m_keepaliveSentTime).count();

                if (waitTime >= KEEPALIVE_RESPONSE_TIMEOUT) {
                    m_missedKeepalives++;
                    m_keepaliveInFlight = false;

                    if (!m_silent) Log::warn("Keepalive timeout (%d/%d missed)",
                                              m_missedKeepalives, MAX_MISSED_KEEPALIVES);

                    if (m_missedKeepalives >= MAX_MISSED_KEEPALIVES) {
                        auto silenceTime = std::chrono::duration_cast<std::chrono::seconds>(
                            now - m_lastRecvTime).count();

                        if (silenceTime >= SILENCE_TIMEOUT) {
                            if (!m_silent) Log::warn("Connection dead: no data for %llds",
                                                      static_cast<long long>(silenceTime));
                            disconnect();
                            if (m_onDisconnected) m_onDisconnected();
                            continue;
                        }
                        // Pool alive (sending jobs) but doesn't respond to keepalive — reset and continue
                        if (!m_silent) Log::info("Pool does not support keepalive, using silence timeout (%llds remaining)",
                                                  static_cast<long long>(SILENCE_TIMEOUT - silenceTime));
                        m_missedKeepalives = 0;
                    }
                    m_lastActivity = now;  // reset idle timer for next keepalive
                }
            }

            // Step 2: If idle and no keepalive in flight, send one
            if (!m_keepaliveInFlight) {
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    now - m_lastActivity).count();
                if (elapsed >= KEEPALIVE_INTERVAL) {
                    sendKeepalive();
                }
            }
        }

#ifdef _WIN32
        WSAPOLLFD pfd;
        pfd.fd = m_socket;
        pfd.events = POLLIN;
        
        int ret = WSAPoll(&pfd, 1, 1000);
#else
        struct pollfd pfd;
        pfd.fd = m_socket;
        pfd.events = POLLIN;
        
        int ret = poll(&pfd, 1, 1000);
#endif
        if (ret < 0) {
            if (!m_silent) Log::error("Poll error");
            disconnect();
            continue;
        }
        
        if (ret == 0) continue; // Timeout, check keepalive on next iteration
        
        if (pfd.revents & (POLLERR | POLLHUP)) {
            if (!m_silent) Log::error("Connection lost");
            disconnect();
            if (m_onDisconnected) m_onDisconnected();
            continue;
        }
        
        if (pfd.revents & POLLIN) {
            // Always recv raw bytes from socket first
            int n = recv(m_socket, buffer, sizeof(buffer) - 1, 0);
            if (n < 0) {
                int err = SOCKET_ERROR_CODE;
                if (err == EAGAIN || err == EWOULDBLOCK
#ifdef _WIN32
                    || err == WSAEWOULDBLOCK
#endif
                ) {
                    continue;  // Spurious wakeup, retry poll
                }
                if (!m_silent) Log::error("Connection error (%d)", err);
                disconnect();
                if (m_onDisconnected) m_onDisconnected();
                continue;
            }
            if (n == 0) {
                if (!m_silent) Log::error("Connection closed by peer");
                disconnect();
                if (m_onDisconnected) m_onDisconnected();
                continue;
            }

            // Any data received proves connection is alive
            m_lastActivity = std::chrono::steady_clock::now();
            m_lastRecvTime = std::chrono::steady_clock::now();
            m_keepaliveInFlight = false;
            m_missedKeepalives = 0;

            if (m_tls && m_ssl) {
                // Feed raw bytes into the read BIO, then decrypt via SSL_read
                BIO_write(m_readBio, buffer, n);

                char plaintext[16384];
                int bytes_read;
                while ((bytes_read = SSL_read(m_ssl, plaintext, sizeof(plaintext) - 1)) > 0) {
                    plaintext[bytes_read] = '\0';
                    lineBuffer += plaintext;
                }

                // After SSL_read loop: WANT_READ is normal (need more data), anything else is fatal
                if (bytes_read <= 0) {
                    int err = SSL_get_error(m_ssl, bytes_read);
                    if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_ZERO_RETURN) {
                        unsigned long e = ERR_peek_last_error();
                        if (!m_silent) Log::error("TLS: Read error (SSL err=%d, reason=%s)", err, ERR_reason_error_string(e));
                        disconnect();
                        if (m_onDisconnected) m_onDisconnected();
                        continue;
                    }
                }

                // Flush any data SSL may want to send (e.g. renegotiation, alerts)
                flushTlsWrite();
            } else {
                buffer[n] = '\0';
                lineBuffer += buffer;
            }

            // Guard against unbounded buffer growth (e.g. malicious pool or MITM)
            if (lineBuffer.size() > 1024 * 1024) {
                if (!m_silent) Log::error("Message too large, disconnecting");
                disconnect();
                if (m_onDisconnected) m_onDisconnected();
                lineBuffer.clear();
                continue;
            }

            // Process complete lines
            size_t pos;
            while ((pos = lineBuffer.find('\n')) != std::string::npos) {
                std::string line = lineBuffer.substr(0, pos);
                lineBuffer.erase(0, pos + 1);

                if (!line.empty()) {
                    handleMessage(line);
                }
            }
        }
    }
}

void Client::sendKeepalive()
{
    // Send keepalive using JSON-RPC keepalive method
    std::stringstream ss;
    ss << "{\"id\":0,\"jsonrpc\":\"2.0\",\"method\":\"keepalived\",\"params\":{\"id\":\"" << m_rpcId << "\"}}\n";
    
    if (send(ss.str())) {
        m_keepaliveSentTime = std::chrono::steady_clock::now();
        m_keepaliveInFlight = true;
        Log::debug("Sent keepalive (missed so far: %d)", m_missedKeepalives);
    }
}

void Client::stop()
{
    m_running = false;
}

void Client::handleMessage(const std::string& message)
{
    Log::debug("Received: %s", message.c_str());
    
    // Check for error — whitespace-tolerant check for "error": null
    bool hasRealError = false;
    size_t errorPos = message.find("\"error\"");
    if (errorPos != std::string::npos) {
        size_t colonPos = message.find(':', errorPos + 7);
        if (colonPos != std::string::npos) {
            size_t valPos = message.find_first_not_of(" \t", colonPos + 1);
            if (valPos != std::string::npos && message.substr(valPos, 4) != "null") {
                hasRealError = true;
            }
        }
    }

    if (hasRealError) {
        std::string error = jsonGetString(message, "message");
        if (error.empty()) {
            error = "Unknown error";
        }
        
        int64_t id = jsonGetInt(message, "id");
        if (id > 1) {
            m_rejected++;
            if (!m_silent) {
                Log::rejected(m_accepted.load(), m_rejected.load(), error.c_str());
            } else {
                Log::debug("Dev share rejected [%lu/%lu] %s", m_accepted.load(), m_rejected.load(), error.c_str());
            }
        } else {
            if (!m_silent) Log::error("Pool error: %s", error.c_str());
        }
        return;
    }
    
    // Check for login response
    if (message.find("\"result\"") != std::string::npos && m_rpcId.empty()) {
        // RPC ID is in result.id, not top-level id
        m_rpcId = jsonGetNestedString(message, "result", "id");
        if (!m_rpcId.empty()) {
            m_authenticated = true;
            m_lastActivity = std::chrono::steady_clock::now();
            Log::debug("Authenticated with pool");
            if (m_onConnected) m_onConnected();
        }
        
        // Handle initial job
        if (message.find("\"job\"") != std::string::npos) {
            handleJob(message);
        }
        return;
    }
    
    // Check for job notification
    if (message.find("\"method\":\"job\"") != std::string::npos) {
        handleJob(message);
        return;
    }
    
    // Check for submit response
    if (message.find("\"result\"") != std::string::npos) {
        std::string status = jsonGetString(message, "status");
        if (status == "OK" || message.find("\"result\":true") != std::string::npos ||
            message.find("\"result\":{\"status\":\"OK\"}") != std::string::npos) {
            m_accepted++;
            if (!m_silent) {
                Log::accepted(m_accepted.load(), m_rejected.load());
            } else {
                Log::debug("Dev share accepted [%lu/%lu]", m_accepted.load(), m_rejected.load());
            }
        } else {
            Log::debug("Unrecognized submit response: %s", message.c_str());
        }
    }
}

void Client::handleJob(const std::string& json)
{
    // Try direct extraction first (for job notifications via params)
    std::string jobId = jsonGetString(json, "job_id");
    std::string blob = jsonGetString(json, "blob");
    std::string target = jsonGetString(json, "target");
    std::string seedHash = jsonGetString(json, "seed_hash");
    int64_t height = jsonGetInt(json, "height");
    
    // If not found, try nested in "job" object (login response: result.job)
    if (jobId.empty() && json.find("\"job\"") != std::string::npos) {
        jobId = jsonGetNestedString(json, "job", "job_id");
        blob = jsonGetNestedString(json, "job", "blob");
        target = jsonGetNestedString(json, "job", "target");
        seedHash = jsonGetNestedString(json, "job", "seed_hash");
        
        // For height, extract job object and parse
        size_t jobStart = json.find("\"job\"");
        if (jobStart != std::string::npos) {
            size_t braceStart = json.find('{', jobStart);
            if (braceStart != std::string::npos) {
                std::string jobJson = json.substr(braceStart);
                height = jsonGetInt(jobJson, "height");
            }
        }
    }
    
    // Try nested in "params" object (some pools use this for notifications)
    if (jobId.empty() && json.find("\"params\"") != std::string::npos) {
        jobId = jsonGetNestedString(json, "params", "job_id");
        blob = jsonGetNestedString(json, "params", "blob");
        target = jsonGetNestedString(json, "params", "target");
        seedHash = jsonGetNestedString(json, "params", "seed_hash");
        
        size_t paramsStart = json.find("\"params\"");
        if (paramsStart != std::string::npos) {
            size_t braceStart = json.find('{', paramsStart);
            if (braceStart != std::string::npos) {
                std::string paramsJson = json.substr(braceStart);
                height = jsonGetInt(paramsJson, "height");
            }
        }
    }
    
    // Parse algorithm from stratum job
    std::string algo;
    // Try direct
    algo = jsonGetString(json, "algo");
    // Try nested in "job"
    if (algo.empty() && json.find("\"job\"") != std::string::npos) {
        algo = jsonGetNestedString(json, "job", "algo");
    }
    // Try nested in "params"
    if (algo.empty() && json.find("\"params\"") != std::string::npos) {
        algo = jsonGetNestedString(json, "params", "algo");
    }

    if (jobId.empty() || blob.empty() || target.empty()) {
        if (!m_silent) Log::warn("Invalid job received");
        return;
    }

    Log::debug("Job parsed - target: %s, seed: %s, algo: %s", target.c_str(), seedHash.c_str(), algo.c_str());

    Job job(jobId, blob, target, height, seedHash);
    if (!algo.empty()) {
        job.setAlgo(algo);
    }
    job.setSequence(++m_sequence);
    
    // Log job only if not in silent mode
    if (!m_silent) {
        uint64_t diff = job.diff();
        if (diff >= 1000000) {
            Log::info("New job | diff: %.1fM | height: %lu", 
                     static_cast<double>(diff) / 1000000.0, job.height());
        } else if (diff >= 1000) {
            Log::info("New job | diff: %.1fK | height: %lu", 
                     static_cast<double>(diff) / 1000.0, job.height());
        } else {
            Log::info("New job | diff: %lu | height: %lu", diff, job.height());
        }
    }
    Log::debug("Job target64: %016lx", job.target());
    
    // Clear submit history for new job (prevent false duplicate detection across jobs)
    {
        std::lock_guard<std::mutex> lock(m_submitHistoryMutex);
        m_submitHistory.clear();
    }
    
    if (m_onJob) {
        m_onJob(job);
    }
}

void Client::reconnect()
{
    if (m_connected) return;

    if (!m_silent) Log::info("Reconnecting to %s:%d...", m_pool.host.c_str(), m_pool.port);
    if (connect(m_pool)) {
        if (!m_silent) Log::info("Reconnected successfully");
    }
}

} // namespace zenrx
