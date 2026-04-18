/* XMRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2017-2018 XMR-Stak    <https://github.com/fireice-uk>, <https://github.com/psychocrypt>
 * Copyright 2018      Lee Clagett <https://github.com/vtnerd>
 * Copyright 2018-2019 SChernykh   <https://github.com/SChernykh>
 * Copyright 2018-2019 tevador     <tevador@gmail.com>
 * Copyright 2016-2019 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */


#include <stdlib.h>
#include <sys/mman.h>
#include <cstdio>
#include <cstdint>

#ifdef __linux__
#   include <sys/syscall.h>
#   include <unistd.h>
    // mbind constants (avoids libnuma-dev dependency)
#   ifndef MPOL_INTERLEAVE
#       define MPOL_INTERLEAVE 3
#   endif
#   ifndef MPOL_BIND
#       define MPOL_BIND 2
#   endif
#   ifndef MPOL_MF_MOVE
#       define MPOL_MF_MOVE (1 << 1)
#   endif
#   ifndef __NR_mbind
#       define __NR_mbind 237
#   endif
    static long zenrx_mbind(void *addr, unsigned long len, int mode,
                            const unsigned long *nodemask, unsigned long maxnode,
                            unsigned flags)
    {
        return syscall(__NR_mbind, addr, len, mode, nodemask, maxnode, flags);
    }
#endif


#include "crypto/mm_malloc.h"
#include "crypto/VirtualMemory.h"


#if defined(__APPLE__)
#   include <mach/vm_statistics.h>
#endif

#if defined(__linux__) && !defined(MAP_HUGE_SHIFT)
#	include <asm-generic/mman-common.h>
#endif


int zenrx::VirtualMemory::m_globalFlags = 0;


zenrx::VirtualMemory::VirtualMemory(size_t size, bool hugePages, size_t align) :
    m_size(size)
{
    if (hugePages) {
        // Huge pages require 2MB-aligned size for mmap MAP_HUGETLB
        size_t alignedSize = VirtualMemory::align(size);
        m_scratchpad = static_cast<uint8_t*>(allocateLargePagesMemory(alignedSize));
        if (m_scratchpad) {
            m_size = alignedSize;
            m_flags |= HUGEPAGES;

            madvise(m_scratchpad, size, MADV_RANDOM | MADV_WILLNEED);

            if (mlock(m_scratchpad, m_size) == 0) {
                m_flags |= LOCK;
            }

            return;
        }
    }

    // Non-hugepage fallback: use actual size (no 2MB rounding waste)
    m_scratchpad = static_cast<uint8_t*>(_mm_malloc(m_size, align));
}


zenrx::VirtualMemory::~VirtualMemory()
{
    if (!m_scratchpad) {
        return;
    }

    if (isHugePages()) {
        if (m_flags & LOCK) {
            munlock(m_scratchpad, m_size);
        }

        freeLargePagesMemory(m_scratchpad, m_size);
    }
    else {
        _mm_free(m_scratchpad);
    }
}



void *zenrx::VirtualMemory::allocateExecutableMemory(size_t size)
{
#   if defined(__APPLE__)
    void *mem = mmap(0, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON, -1, 0);
#   else
    void *mem = mmap(0, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#   endif

    return mem == MAP_FAILED ? nullptr : mem;
}

void *zenrx::VirtualMemory::allocateLargePagesMemory(size_t size, size_t page_size)
{
#   if defined(__APPLE__)
    void *mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, VM_FLAGS_SUPERPAGE_SIZE_2MB, 0);
#   elif defined(__FreeBSD__)
    void *mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_ALIGNED_SUPER | MAP_PREFAULT_READ, -1, 0);
#   else
#ifndef MAP_HUGE_2MB
#define MAP_HUGE_2MB (21 << MAP_HUGE_SHIFT)
#endif
#ifndef MAP_HUGE_1GB
#define MAP_HUGE_1GB (30 << MAP_HUGE_SHIFT)
#endif

	int page_size_flags = 0;
	if(page_size == 2u)
		page_size_flags |= MAP_HUGE_2MB;
	else if(page_size == 1024u)
		page_size_flags |= MAP_HUGE_1GB;

    void *mem = mmap(0, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB | MAP_POPULATE | page_size_flags, 0, 0);
#   endif

    return mem == MAP_FAILED ? nullptr : mem;
}


void zenrx::VirtualMemory::flushInstructionCache(void *p, size_t size)
{
#   ifdef HAVE_BUILTIN_CLEAR_CACHE
    __builtin___clear_cache(reinterpret_cast<char*>(p), reinterpret_cast<char*>(p) + size);
#   endif
}


void zenrx::VirtualMemory::freeLargePagesMemory(void *p, size_t size)
{
    if(munmap(p, size) != 0)
	{
		fprintf(stderr, "munmap failed %llu\n", (unsigned long long)size);
		size_t page3gib = 3llu*1024*1024*1024;
		fprintf(stderr, "try to unmap %llu\n", (unsigned long long)page3gib);
		if(munmap(p, page3gib) != 0)
		{
			fprintf(stderr, "munmap failed %llu\n", (unsigned long long)page3gib);
		}
	}
}


void zenrx::VirtualMemory::init(bool hugePages)
{
    if (hugePages) {
        m_globalFlags = HUGEPAGES | HUGEPAGES_AVAILABLE;
    }
}


void zenrx::VirtualMemory::protectExecutableMemory(void *p, size_t size)
{
    mprotect(p, size, PROT_READ | PROT_EXEC);
}


void zenrx::VirtualMemory::unprotectExecutableMemory(void *p, size_t size)
{
    mprotect(p, size, PROT_WRITE | PROT_EXEC);
}


void zenrx::VirtualMemory::protectRW(void *p, size_t size)
{
    mprotect(p, size, PROT_READ | PROT_WRITE);
}


void zenrx::VirtualMemory::protectRX(void *p, size_t size)
{
    mprotect(p, size, PROT_READ | PROT_EXEC);
}


void zenrx::VirtualMemory::bindInterleave(void *p, size_t size)
{
#ifdef __linux__
    unsigned long maxNode = 64;
    unsigned long nodemask[1] = { ~0UL };
    zenrx_mbind(p, size, MPOL_INTERLEAVE, nodemask, maxNode, MPOL_MF_MOVE);
#else
    (void)p; (void)size;
#endif
}


void zenrx::VirtualMemory::bindToNode(void *p, size_t size, int node)
{
#ifdef __linux__
    if (node < 0 || node >= 64) return;
    unsigned long nodemask[1] = { 1UL << node };
    zenrx_mbind(p, size, MPOL_BIND, nodemask, 64, MPOL_MF_MOVE);
#else
    (void)p; (void)size; (void)node;
#endif
}
