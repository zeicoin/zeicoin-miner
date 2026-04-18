#include "Config.h"
#include "CpuInfo.h"
#include "Log.h"
#include "version.h"

#include <fstream>
#include <sstream>
#include <cstring>
#include <thread>
#include <getopt.h>
#include <iomanip>
#include <vector>
#include <stdexcept>

#ifdef _WIN32
#   include <windows.h>
#endif

namespace zenrx {

// Safe string to int conversion with default value on error
static int safeStoi(const std::string& str, int defaultValue = 0)
{
    if (str.empty()) return defaultValue;
    try {
        return std::stoi(str);
    } catch (const std::invalid_argument&) {
        return defaultValue;
    } catch (const std::out_of_range&) {
        return defaultValue;
    }
}

// Detect TLS scheme (stratum+ssl://, ssl://, tls://, stratum+tls://) and strip it from URL.
// Returns true if the scheme indicates TLS, false otherwise.
static bool detectAndStripScheme(std::string& url)
{
    size_t schemeEnd = url.find("://");
    if (schemeEnd != std::string::npos) {
        std::string scheme = url.substr(0, schemeEnd);
        for (auto& c : scheme) c = static_cast<char>(tolower(c));
        bool tls = (scheme == "stratum+ssl" || scheme == "stratum+tls" ||
                    scheme == "ssl" || scheme == "tls");
        url = url.substr(schemeEnd + 3);
        return tls;
    }
    return false;
}

// Extract a JSON value for a given key from a JSON-like string.
// Handles both quoted string values and bare (number/boolean) values.
static std::string jsonGetValue(const std::string& content, const std::string& key)
{
    std::string search = "\"" + key + "\"";
    size_t pos = content.find(search);
    if (pos == std::string::npos) return "";

    pos = content.find(':', pos);
    if (pos == std::string::npos) return "";

    pos = content.find_first_not_of(" \t\n\r", pos + 1);
    if (pos == std::string::npos) return "";

    if (content[pos] == '"') {
        size_t end = content.find('"', pos + 1);
        return content.substr(pos + 1, end - pos - 1);
    } else {
        size_t end = content.find_first_of(",}\n", pos);
        std::string val = content.substr(pos, end - pos);
        while (!val.empty() && isspace(val.back())) val.pop_back();
        return val;
    }
}

void Config::printVersion()
{
    printf("%s\n", APP_VERSION_FULL);
    printf("  RandomX CPU miner for ZenOS\n");
    printf("  Built with GCC %d.%d.%d\n", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
}

void Config::printHelp()
{
    printVersion();
    printf("\nUsage: zenrx [OPTIONS]\n\n");
    printf("Options:\n");
    printf("  -o, --url=URL          Pool URL (e.g., stratum+tcp://pool.com:3333)\n");
    printf("  -u, --user=USER        Wallet address or username\n");
    printf("  -p, --pass=PASS        Password (default: x)\n");
    printf("  -c, --config=FILE      Load config from JSON file\n");
    printf("      --no-color         Disable colored output\n");
    printf("      --print-time=N     Hashrate print interval in seconds (default: 60)\n");
    printf("      --no-api           Disable HTTP API (enabled by default on port 16000)\n");
    printf("      --api-port=N       HTTP API port (default: 16000)\n");
    printf("      --api-host=HOST    HTTP API host (default: 127.0.0.1)\n");
    printf("      --no-autosave      Disable auto-saving config\n");
    printf("      --log-file=PATH    Write logs to file (in addition to stdout)\n");
    printf("      --debug            Enable debug logging\n");
    printf("  -h, --help             Show this help\n");
    printf("  -V, --version          Show version\n");
    printf("\nExamples:\n");
    printf("  zenrx -o stratum+tcp://pool.hashvault.pro:80 -u YOUR_WALLET\n");
    printf("  zenrx -c config.json\n");
    printf("\nAuto-detection:\n");
    printf("  ZenRX auto-detects optimal thread count based on CPU L3 cache.\n");
    printf("  MSR tweaks and huge pages are automatically configured at startup.\n");
}

bool Config::parseArgs(int argc, char** argv)
{
    static struct option long_options[] = {
        {"url",           required_argument, 0, 'o'},
        {"user",          required_argument, 0, 'u'},
        {"pass",          required_argument, 0, 'p'},
        {"config",        required_argument, 0, 'c'},
        {"no-color",      no_argument,       0, 1002},
        {"print-time",    required_argument, 0, 1003},
        {"debug",         no_argument,       0, 1005},
        {"api",           no_argument,       0, 1007},
        {"api-port",      required_argument, 0, 1008},
        {"api-host",      required_argument, 0, 1009},
        {"no-api",        no_argument,       0, 1010},
        {"no-autosave",   no_argument,       0, 1011},
        {"log-file",      required_argument, 0, 1012},
        {"help",          no_argument,       0, 'h'},
        {"version",       no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };
    
    PoolConfig cliPool;
    std::string configFile;

    // Auto-detect config file if no arguments provided
    if (argc <= 1) {
#ifdef _WIN32
        // On Windows, look for config file in same directory as executable
        char exePath[MAX_PATH];
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
            std::string path(exePath);
            size_t lastSlash = path.find_last_of("\\/");
            if (lastSlash != std::string::npos) {
                path = path.substr(0, lastSlash + 1);
            }
            
            // Try zenrx.json first, then config.json
            std::vector<std::string> configNames = {"zenrx.json", "config.json"};
            for (const auto& name : configNames) {
                std::string fullPath = path + name;
                std::ifstream test(fullPath);
                if (test.good()) {
                    configFile = fullPath;
                    break;
                }
            }
        }
#else
        // On Linux, try current directory
        std::vector<std::string> configNames = {"zenrx.json", "config.json"};
        for (const auto& name : configNames) {
            std::ifstream test(name);
            if (test.good()) {
                configFile = name;
                break;
            }
        }
#endif
    }
    
    int opt;
    int option_index = 0;
    
    while ((opt = getopt_long(argc, argv, "o:u:p:c:hV", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'o': {
                // Parse URL: stratum+tcp://host:port or host:port
                std::string url = optarg;
                cliPool.tls = detectAndStripScheme(url);

                size_t portStart = url.rfind(':');
                if (portStart != std::string::npos) {
                    cliPool.host = url.substr(0, portStart);
                    cliPool.port = safeStoi(url.substr(portStart + 1), 3333);
                } else {
                    cliPool.host = url;
                    cliPool.port = 3333;
                }
                break;
            }
            case 'u':
                cliPool.user = optarg;
                break;
            case 'p':
                cliPool.pass = optarg;
                break;
            case 'c':
                configFile = optarg;
                break;
            case 1002:
                colors = false;
                break;
            case 1003:
                printTime = safeStoi(optarg, 60);
                break;
            case 1005:
                debug = true;
                break;
            case 1007:
                // --api is now default, kept for compatibility
                apiEnabled = true;
                break;
            case 1008:
                apiPort = safeStoi(optarg, 16000);
                break;
            case 1009:
                apiHost = optarg;
                break;
            case 1010:
                apiEnabled = false;
                break;
            case 1011:
                autosave = false;
                break;
            case 1012:
                logFile = optarg;
                break;
            case 'h':
                printHelp();
                return false;
            case 'V':
                printVersion();
                return false;
            default:
                return false;
        }
    }
    
    // Load config file if specified
    if (!configFile.empty()) {
        if (!parseFile(configFile)) {
            return false;
        }
        configPath = configFile;
    }
    
    // Pool from command line overrides config file
    if (!cliPool.host.empty() && !cliPool.user.empty()) {
        pool = cliPool;
    }
    
    // Auto-detect threads if not specified
    autoDetect();
    
    return isValid();
}

void Config::autoDetect()
{
    const auto& cpuInfo = cpu();
    uint32_t maxT = cpuInfo.threads() > 1 ? cpuInfo.threads() - 1 : 1;

    static const RxAlgo ALL_ALGOS[] = {
        RxAlgo::RX_0, RxAlgo::RX_WOW, RxAlgo::RX_ARQ,
        RxAlgo::RX_XEQ, RxAlgo::RX_GRAFT, RxAlgo::RX_SFX, RxAlgo::RX_XLA
    };

    for (auto a : ALL_ALGOS) {
        auto affinity = cpuInfo.affinityForAlgo(a);
        if (algoThreads.find(a) == algoThreads.end()) {
            algoThreads[a] = static_cast<int>(affinity.size());
        }
        // Clamp to max
        if (algoThreads[a] > static_cast<int>(maxT)) {
            algoThreads[a] = static_cast<int>(maxT);
        }
        algoAffinities[a] = std::move(affinity);
        Log::debug("Algo %s: %d threads", rxAlgoName(a), algoThreads[a]);
    }
}

int Config::threadsForAlgo(RxAlgo algo) const
{
    auto it = algoThreads.find(algo);
    return (it != algoThreads.end()) ? it->second : 1;
}

int Config::maxThreads() const
{
    int max = 1;
    for (const auto& kv : algoThreads) {
        if (kv.second > max) max = kv.second;
    }
    return max;
}

const std::vector<int32_t>& Config::affinityForAlgo(RxAlgo algo) const
{
    static const std::vector<int32_t> empty;
    auto it = algoAffinities.find(algo);
    return (it != algoAffinities.end()) ? it->second : empty;
}

bool Config::parseFile(const std::string& path)
{
    std::ifstream file(path);
    if (!file.is_open()) {
        Log::error("Failed to open config file: %s", path.c_str());
        return false;
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    
    // Simple JSON parsing using the static jsonGetValue helper
    auto getValue = [&content](const std::string& key) -> std::string {
        return jsonGetValue(content, key);
    };
    
    // Parse pools array - take the first pool entry
    size_t poolsStart = content.find("\"pools\"");
    if (poolsStart != std::string::npos) {
        size_t arrayStart = content.find('[', poolsStart);
        if (arrayStart != std::string::npos) {
            // Find matching closing bracket
            int depth = 1;
            size_t arrayEnd = arrayStart + 1;
            while (arrayEnd < content.length() && depth > 0) {
                if (content[arrayEnd] == '[') depth++;
                else if (content[arrayEnd] == ']') depth--;
                arrayEnd++;
            }

            // Extract pools array content
            std::string poolsContent = content.substr(arrayStart, arrayEnd - arrayStart);

            // Parse the first pool object
            size_t pos = poolsContent.find('{');
            if (pos != std::string::npos) {
                int objDepth = 1;
                size_t objEnd = pos + 1;
                while (objEnd < poolsContent.length() && objDepth > 0) {
                    if (poolsContent[objEnd] == '{') objDepth++;
                    else if (poolsContent[objEnd] == '}') objDepth--;
                    objEnd++;
                }

                std::string poolObj = poolsContent.substr(pos, objEnd - pos);

                auto getPoolValue = [&poolObj](const std::string& key) -> std::string {
                    return jsonGetValue(poolObj, key);
                };

                std::string url = getPoolValue("url");
                std::string user = getPoolValue("user");
                std::string pass = getPoolValue("pass");
                std::string tlsStr = getPoolValue("tls");

                if (!url.empty() && !user.empty()) {
                    pool.tls = detectAndStripScheme(url);
                    if (tlsStr == "true") pool.tls = true;

                    size_t portStart = url.rfind(':');
                    if (portStart != std::string::npos) {
                        pool.host = url.substr(0, portStart);
                        pool.port = static_cast<uint16_t>(safeStoi(url.substr(portStart + 1), 3333));
                    } else {
                        pool.host = url;
                        pool.port = 3333;
                    }

                    pool.user = user;
                    if (!pass.empty()) pool.pass = pass;
                }
            }
        }
    }

    // Fallback: if no pools array found, try direct url/user/pass (simple config format)
    if (pool.host.empty()) {
        std::string url = getValue("url");
        std::string user = getValue("user");
        std::string pass = getValue("pass");

        if (!url.empty() && !user.empty()) {
            pool.tls = detectAndStripScheme(url);

            std::string tlsStr = getValue("tls");
            if (tlsStr == "true") pool.tls = true;

            size_t portStart = url.rfind(':');
            if (portStart != std::string::npos) {
                pool.host = url.substr(0, portStart);
                pool.port = safeStoi(url.substr(portStart + 1), 3333);
            } else {
                pool.host = url;
            }

            pool.user = user;
            if (!pass.empty()) pool.pass = pass;
        }
    }
    
    // Parse other settings
    std::string printTimeStr = getValue("print-time");
    if (!printTimeStr.empty()) printTime = safeStoi(printTimeStr, 60);
    
    // API/HTTP settings - try both formats
    // Format 1: "http": { "enabled": true, "host": "...", "port": ... }
    // Format 2: "api-enabled": true, "api-host": "...", "api-port": ...
    
    // Check for "http" block first
    size_t httpPos = content.find("\"http\"");
    if (httpPos != std::string::npos) {
        size_t httpStart = content.find('{', httpPos);
        size_t httpEnd = content.find('}', httpStart);
        if (httpStart != std::string::npos && httpEnd != std::string::npos) {
            std::string httpBlock = content.substr(httpStart, httpEnd - httpStart + 1);
            
            // Parse from http block
            auto getHttpValue = [&httpBlock](const std::string& key) -> std::string {
                return jsonGetValue(httpBlock, key);
            };
            
            std::string enabledStr = getHttpValue("enabled");
            if (enabledStr == "true") apiEnabled = true;
            else if (enabledStr == "false") apiEnabled = false;
            
            std::string hostStr = getHttpValue("host");
            if (!hostStr.empty()) apiHost = hostStr;
            
            std::string portStr = getHttpValue("port");
            if (!portStr.empty()) apiPort = safeStoi(portStr, 16000);
        }
    }
    
    // Fallback to flat format
    // Debug setting
    std::string debugStr = getValue("debug");
    if (debugStr == "true") debug = true;
    else if (debugStr == "false") debug = false;

    std::string colorsStr = getValue("colors");
    if (colorsStr == "true") colors = true;
    else if (colorsStr == "false") colors = false;

    std::string autosaveStr = getValue("autosave");
    if (autosaveStr == "true") autosave = true;
    else if (autosaveStr == "false") autosave = false;

    std::string logFileStr = getValue("log-file");
    if (!logFileStr.empty() && logFileStr != "null") logFile = logFileStr;

    std::string apiStr = getValue("api-enabled");
    if (apiStr == "true") apiEnabled = true;
    else if (apiStr == "false") apiEnabled = false;
    
    std::string apiHostStr = getValue("api-host");
    if (!apiHostStr.empty()) apiHost = apiHostStr;
    
    std::string apiPortStr = getValue("api-port");
    if (!apiPortStr.empty()) apiPort = safeStoi(apiPortStr, 16000);

    // Parse algorithm
    std::string algoStr = getValue("algo");
    if (!algoStr.empty() && algoStr != "null") {
        algo = algoStr;
        rxAlgo = parseRxAlgo(algoStr);
        algoSpecified = true;
    }

    // Parse bench-algo-time
    std::string benchStr = getValue("bench-algo-time");
    if (!benchStr.empty() && benchStr != "null") {
        benchAlgoTime = safeStoi(benchStr, 20);
    }

    // Parse algo-min-time
    std::string minTimeStr = getValue("algo-min-time");
    if (!minTimeStr.empty() && minTimeStr != "null") {
        algoMinTime = safeStoi(minTimeStr, 0);
    }

    // Parse "cpu" block
    size_t cpuPos = content.find("\"cpu\"");
    if (cpuPos != std::string::npos) {
        size_t cpuStart = content.find('{', cpuPos);
        size_t cpuEnd = content.find('}', cpuStart);
        if (cpuStart != std::string::npos && cpuEnd != std::string::npos) {
            std::string cpuBlock = content.substr(cpuStart, cpuEnd - cpuStart + 1);

            auto getCpuValue = [&cpuBlock](const std::string& key) -> std::string {
                return jsonGetValue(cpuBlock, key);
            };

            std::string argon2Str = getCpuValue("argon2-impl");
            if (!argon2Str.empty() && argon2Str != "null") argon2Impl = argon2Str;
        }
    }

    // Parse algo-threads object (per-algorithm thread counts)
    size_t algoThreadsPos = content.find("\"algo-threads\"");
    if (algoThreadsPos != std::string::npos) {
        size_t objStart = content.find('{', algoThreadsPos);
        if (objStart != std::string::npos) {
            int depth = 1;
            size_t objEnd = objStart + 1;
            while (objEnd < content.length() && depth > 0) {
                if (content[objEnd] == '{') depth++;
                else if (content[objEnd] == '}') depth--;
                objEnd++;
            }
            std::string threadObj = content.substr(objStart + 1, objEnd - objStart - 2);

            size_t pos = 0;
            while (pos < threadObj.length()) {
                size_t keyStart = threadObj.find('"', pos);
                if (keyStart == std::string::npos) break;
                size_t keyEnd = threadObj.find('"', keyStart + 1);
                if (keyEnd == std::string::npos) break;
                std::string key = threadObj.substr(keyStart + 1, keyEnd - keyStart - 1);

                size_t colon = threadObj.find(':', keyEnd);
                if (colon == std::string::npos) break;

                size_t valStart = threadObj.find_first_not_of(" \t\n\r", colon + 1);
                if (valStart == std::string::npos) break;
                size_t valEnd = threadObj.find_first_of(",}", valStart);
                if (valEnd == std::string::npos) valEnd = threadObj.length();
                std::string valStr = threadObj.substr(valStart, valEnd - valStart);
                while (!valStr.empty() && isspace(valStr.back())) valStr.pop_back();

                int val = safeStoi(valStr, 0);
                if (val > 0) {
                    algoThreads[parseRxAlgo(key)] = val;
                }

                pos = valEnd + 1;
            }

        }
    }

    // Parse algo-perf object
    size_t algoPerfPos = content.find("\"algo-perf\"");
    if (algoPerfPos != std::string::npos) {
        size_t objStart = content.find('{', algoPerfPos);
        if (objStart != std::string::npos) {
            int depth = 1;
            size_t objEnd = objStart + 1;
            while (objEnd < content.length() && depth > 0) {
                if (content[objEnd] == '{') depth++;
                else if (content[objEnd] == '}') depth--;
                objEnd++;
            }
            std::string perfObj = content.substr(objStart + 1, objEnd - objStart - 2);

            // Parse key:value pairs from the algo-perf object
            size_t pos = 0;
            while (pos < perfObj.length()) {
                size_t keyStart = perfObj.find('"', pos);
                if (keyStart == std::string::npos) break;
                size_t keyEnd = perfObj.find('"', keyStart + 1);
                if (keyEnd == std::string::npos) break;
                std::string key = perfObj.substr(keyStart + 1, keyEnd - keyStart - 1);

                size_t colon = perfObj.find(':', keyEnd);
                if (colon == std::string::npos) break;

                size_t valStart = perfObj.find_first_not_of(" \t\n\r", colon + 1);
                if (valStart == std::string::npos) break;
                size_t valEnd = perfObj.find_first_of(",}", valStart);
                if (valEnd == std::string::npos) valEnd = perfObj.length();
                std::string valStr = perfObj.substr(valStart, valEnd - valStart);
                while (!valStr.empty() && isspace(valStr.back())) valStr.pop_back();

                try {
                    double val = std::stod(valStr);
                    if (val > 0) {
                        algoPerf[key] = val;
                    }
                } catch (...) {}

                pos = valEnd + 1;
            }
        }
    }

    return true;
}

bool Config::saveFile(const std::string& path) const
{
    std::string savePath = path.empty() ? configPath : path;
    if (savePath.empty()) {
        savePath = "config.json";
    }

    std::ofstream file(savePath);
    if (!file.is_open()) {
        Log::error("Failed to save config to: %s", savePath.c_str());
        return false;
    }

    file << "{\n";

    // HTTP section
    file << "    \"http\": {\n";
    file << "        \"enabled\": " << (apiEnabled ? "true" : "false") << ",\n";
    file << "        \"host\": \"" << apiHost << "\",\n";
    file << "        \"port\": " << apiPort << "\n";
    file << "    },\n";

    // Pool section
    file << "    \"pools\": [\n";
    if (!pool.host.empty()) {
        file << "        {\n";
        file << "            \"url\": \"" << (pool.tls ? "stratum+ssl://" : "") << pool.host << ":" << pool.port << "\",\n";
        file << "            \"user\": \"" << pool.user << "\",\n";
        file << "            \"pass\": \"" << pool.pass << "\"\n";
        file << "        }\n";
    }
    file << "    ],\n";

    if (!argon2Impl.empty()) {
        file << "    \"argon2-impl\": \"" << argon2Impl << "\",\n";
    }

    // Per-algorithm thread counts (sorted to match algo-perf order)
    file << "    \"algo-threads\": {\n";
    {
        static const RxAlgo SAVE_ORDER[] = {
            RxAlgo::RX_0, RxAlgo::RX_ARQ, RxAlgo::RX_GRAFT,
            RxAlgo::RX_SFX, RxAlgo::RX_WOW, RxAlgo::RX_XEQ, RxAlgo::RX_XLA
        };
        size_t count = 0;
        size_t total = algoThreads.size();
        for (auto a : SAVE_ORDER) {
            auto it = algoThreads.find(a);
            if (it == algoThreads.end()) continue;
            file << "        \"" << rxAlgoName(it->first) << "\": " << it->second;
            if (++count < total) file << ",";
            file << "\n";
        }
    }
    file << "    },\n";

    if (algoSpecified) {
        file << "    \"algo\": \"" << algo << "\",\n";
    } else {
        file << "    \"algo\": null,\n";
    }
    file << "    \"colors\": " << (colors ? "true" : "false") << ",\n";
    file << "    \"autosave\": " << (autosave ? "true" : "false") << ",\n";
    file << "    \"debug\": " << (debug ? "true" : "false") << ",\n";
    if (!logFile.empty()) {
        file << "    \"log-file\": \"" << logFile << "\",\n";
    } else {
        file << "    \"log-file\": null,\n";
    }
    file << "    \"print-time\": " << printTime << ",\n";

    // Benchmark settings
    file << "    \"bench-algo-time\": " << benchAlgoTime << ",\n";
    if (algoMinTime > 0) {
        file << "    \"algo-min-time\": " << algoMinTime << ",\n";
    }

    // Algo-perf results (sorted alphabetically)
    file << "    \"algo-perf\": {";
    if (!algoPerf.empty()) {
        file << "\n";
        static const char* PERF_ORDER[] = {
            "rx/0", "rx/arq", "rx/graft", "rx/sfx", "rx/wow", "rx/xeq", "rx/xla"
        };
        size_t count = 0;
        size_t total = algoPerf.size();
        for (const char* name : PERF_ORDER) {
            auto it = algoPerf.find(name);
            if (it == algoPerf.end()) continue;
            file << "        \"" << it->first << "\": " << std::fixed << std::setprecision(1) << it->second;
            if (++count < total) file << ",";
            file << "\n";
        }
        file << "    }\n";
    } else {
        file << "}\n";
    }

    file << "}\n";

    Log::info("Config saved to: %s", savePath.c_str());
    return true;
}

bool Config::isValid() const
{
    if (pool.host.empty()) {
        Log::error("No pool configured. Use -o to specify pool URL.");
        return false;
    }

    if (pool.user.empty()) {
        Log::error("Pool user/wallet is empty. Use -u to specify.");
        return false;
    }
    
    if (algoThreads.empty()) {
        Log::error("No thread counts configured");
        return false;
    }
    
    return true;
}

} // namespace zenrx
