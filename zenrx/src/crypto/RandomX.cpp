#include "crypto/RandomX.h"
#include "Log.h"

#include "crypto/randomx/randomx.h"

#include <cpuid.h>

namespace zenrx {

static RandomX s_randomx;

RandomX& randomx()
{
    return s_randomx;
}

RandomX::RandomX()
{
}

RandomX::~RandomX()
{
    release();
}

bool RandomX::hasAES()
{
    uint32_t eax, ebx, ecx, edx;
    __cpuid(1, eax, ebx, ecx, edx);
    return (ecx >> 25) & 1;
}

bool RandomX::hasAVX2()
{
    uint32_t eax, ebx, ecx, edx;
    __cpuid_count(7, 0, eax, ebx, ecx, edx);
    return (ebx >> 5) & 1;
}

RxInstance* RandomX::resolve(RxInstanceId id) const
{
    switch (id) {
    case RxInstanceId::User: return m_userInstance.get();
    case RxInstanceId::Dev:  return m_devInstance.get();
    }
    return nullptr;
}

bool RandomX::init(RxInstanceId id, const std::string& seedHash, int threads,
                   RxAlgo algo, bool hugePages, bool hardwareAES, int initThreads,
                   bool oneGbPages, const std::vector<int32_t>& numaNodes)
{
    std::lock_guard<std::shared_mutex> lock(m_mutex);

    if (!resolve(id)) {
        switch (id) {
        case RxInstanceId::User: m_userInstance = std::make_unique<RxInstance>(); break;
        case RxInstanceId::Dev:  m_devInstance  = std::make_unique<RxInstance>(); break;
        }
    }

    RxInstance* instance = resolve(id);
    if (!instance) {
        return false;
    }

    return instance->init(seedHash, threads, algo, hugePages, hardwareAES, initThreads, oneGbPages, numaNodes);
}

bool RandomX::init(const std::string& seedHash, int threads,
                   RxAlgo algo, bool hugePages, bool hardwareAES, int initThreads,
                   bool oneGbPages, const std::vector<int32_t>& numaNodes)
{
    return init(RxInstanceId::User, seedHash, threads, algo, hugePages, hardwareAES, initThreads, oneGbPages, numaNodes);
}

bool RandomX::reinit(RxInstanceId id, const std::string& seedHash, int threads,
                     bool hardwareAES, int initThreads,
                     const std::vector<int32_t>& numaNodes)
{
    std::lock_guard<std::shared_mutex> lock(m_mutex);

    RxInstance* instance = resolve(id);
    if (!instance) {
        return false;
    }

    return instance->reinit(seedHash, threads, hardwareAES, initThreads, numaNodes);
}

bool RandomX::isSeedValid(const std::string& seedHash) const
{
    return isSeedValid(RxInstanceId::User, seedHash);
}

bool RandomX::isSeedValid(RxInstanceId id, const std::string& seedHash) const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    const RxInstance* instance = resolve(id);
    if (!instance) {
        return false;
    }

    return instance->isValidForSeed(seedHash);
}

randomx_vm* RandomX::getVM(int index)
{
    return getVM(RxInstanceId::User, index);
}

randomx_vm* RandomX::getVM(RxInstanceId id, int index)
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    RxInstance* instance = resolve(id);
    if (!instance) {
        return nullptr;
    }

    return instance->getVM(index);
}

RxInstance* RandomX::getInstance(RxInstanceId id)
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);
    return resolve(id);
}

void RandomX::release()
{
    std::lock_guard<std::shared_mutex> lock(m_mutex);

    if (m_userInstance) {
        m_userInstance->release();
        m_userInstance.reset();
    }

    if (m_devInstance) {
        m_devInstance->release();
        m_devInstance.reset();
    }
}

void RandomX::release(RxInstanceId id)
{
    std::lock_guard<std::shared_mutex> lock(m_mutex);

    auto releaseOne = [](std::unique_ptr<RxInstance>& inst) {
        if (inst) {
            inst->release();
            inst.reset();
        }
    };

    switch (id) {
    case RxInstanceId::User: releaseOne(m_userInstance); break;
    case RxInstanceId::Dev:  releaseOne(m_devInstance);  break;
    }
}

bool RandomX::allHugePagesEnabled() const
{
    std::shared_lock<std::shared_mutex> lock(m_mutex);

    bool userOk = m_userInstance && m_userInstance->isInitialized() && m_userInstance->hasHugePages();
    bool devOk = !m_devInstance || !m_devInstance->isInitialized() || m_devInstance->hasHugePages();

    return userOk && devOk;
}

} // namespace zenrx
