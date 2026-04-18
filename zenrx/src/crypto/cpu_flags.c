/* Provides cpu_flags_has_* functions required by crypto/argon2 */

#include <cpuid.h>
#include <stdint.h>

static void cpuid(uint32_t level, uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{
    __cpuid_count(level, 0, *eax, *ebx, *ecx, *edx);
}

int cpu_flags_has_sse2(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (edx >> 26) & 1;
}

int cpu_flags_has_ssse3(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(1, &eax, &ebx, &ecx, &edx);
    return (ecx >> 9) & 1;
}

int cpu_flags_has_xop(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    return (ecx >> 11) & 1;
}

int cpu_flags_has_avx2(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, &eax, &ebx, &ecx, &edx);
    return (ebx >> 5) & 1;
}

int cpu_flags_has_avx512f(void)
{
    uint32_t eax, ebx, ecx, edx;
    cpuid(7, &eax, &ebx, &ecx, &edx);
    return (ebx >> 16) & 1;
}
