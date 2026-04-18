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


#include <windows.h>
#include <cstdio>
#include <cstdint>

#include "crypto/mm_malloc.h"
#include "crypto/VirtualMemory.h"


int zenrx::VirtualMemory::m_globalFlags = 0;


zenrx::VirtualMemory::VirtualMemory(size_t size, bool hugePages, size_t align) :
    m_size(size)
{
    if (hugePages) {
        // Large pages require aligned size
        size_t alignedSize = VirtualMemory::align(size);
        m_scratchpad = static_cast<uint8_t*>(allocateLargePagesMemory(alignedSize));
        if (m_scratchpad) {
            m_size = alignedSize;
            m_flags |= HUGEPAGES;

            if (VirtualLock(m_scratchpad, m_size)) {
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
            VirtualUnlock(m_scratchpad, m_size);
        }

        freeLargePagesMemory(m_scratchpad, m_size);
    }
    else {
        _mm_free(m_scratchpad);
    }
}



void *zenrx::VirtualMemory::allocateExecutableMemory(size_t size)
{
    return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

void *zenrx::VirtualMemory::allocateLargePagesMemory(size_t size, size_t page_size)
{
    (void)page_size;

    const SIZE_T largePageMin = GetLargePageMinimum();
    if (largePageMin == 0) {
        return nullptr;
    }

    // Round up to large page boundary
    const SIZE_T roundedSize = ((size + largePageMin - 1) / largePageMin) * largePageMin;

    return VirtualAlloc(nullptr, roundedSize, MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES, PAGE_READWRITE);
}


void zenrx::VirtualMemory::flushInstructionCache(void *p, size_t size)
{
    ::FlushInstructionCache(GetCurrentProcess(), p, size);
}


void zenrx::VirtualMemory::freeLargePagesMemory(void *p, size_t size)
{
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
}


void zenrx::VirtualMemory::init(bool hugePages)
{
    if (hugePages && GetLargePageMinimum() > 0) {
        m_globalFlags = HUGEPAGES | HUGEPAGES_AVAILABLE;
    }
}


void zenrx::VirtualMemory::protectExecutableMemory(void *p, size_t size)
{
    DWORD oldProtect;
    VirtualProtect(p, size, PAGE_EXECUTE_READ, &oldProtect);
}


void zenrx::VirtualMemory::unprotectExecutableMemory(void *p, size_t size)
{
    DWORD oldProtect;
    VirtualProtect(p, size, PAGE_EXECUTE_READWRITE, &oldProtect);
}


void zenrx::VirtualMemory::protectRW(void *p, size_t size)
{
    DWORD oldProtect;
    VirtualProtect(p, size, PAGE_READWRITE, &oldProtect);
}


void zenrx::VirtualMemory::protectRX(void *p, size_t size)
{
    DWORD oldProtect;
    VirtualProtect(p, size, PAGE_EXECUTE_READ, &oldProtect);
}


void zenrx::VirtualMemory::bindInterleave(void *p, size_t size)
{
    // NUMA binding not implemented on Windows (mbind is Linux-only)
    (void)p; (void)size;
}


void zenrx::VirtualMemory::bindToNode(void *p, size_t size, int node)
{
    // NUMA binding not implemented on Windows (mbind is Linux-only)
    (void)p; (void)size; (void)node;
}
