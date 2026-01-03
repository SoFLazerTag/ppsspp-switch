// Copyright (C) 2003 Dolphin Project / PPSSPP Project.

#include "ppsspp_config.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>

#include "Common/Log.h"
#include "Common/File/FileUtil.h"
#include "Common/MemoryUtil.h"
#include "Common/MemArena.h"

#ifdef HAVE_LIBNX
#include <malloc.h>
#include <switch.h>

static VirtmemReservation* baseRes = nullptr;
static VirtmemReservation* codeRes = nullptr;
static uintptr_t memoryBase = 0;
static uintptr_t memoryCodeBase = 0;
static uintptr_t memorySrcBase = 0;
#else
#include <sys/mman.h>
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#endif

size_t MemArena::roundup(size_t x) {
    return x;
}

bool MemArena::NeedsProbing() {
    return false;
}

bool MemArena::GrabMemSpace(size_t size) {
#ifdef HAVE_LIBNX
    // On Switch, we don't need a physical file for shm_open.
    return true;
#else
    mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    fd = shm_open("/ppsspp_ram", O_RDWR | O_CREAT | O_EXCL, mode);
    if (fd < 0) return false;
    ftruncate(fd, size);
    return true;
#endif
}

void MemArena::ReleaseSpace() {
#ifdef HAVE_LIBNX
    if (memoryCodeBase && memorySrcBase) {
        svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)memoryCodeBase, (u64)memorySrcBase, 0x10000000);
    }
    if (memorySrcBase) {
        free((void*)memorySrcBase);
        memorySrcBase = 0;
    }
    virtmemLock();
    if (baseRes) { virtmemRemoveReservation(baseRes); baseRes = nullptr; }
    if (codeRes) { virtmemRemoveReservation(codeRes); codeRes = nullptr; }
    virtmemUnlock();
    memoryBase = 0;
    memoryCodeBase = 0;
#else
    close(fd);
#endif
}

void *MemArena::CreateView(s64 offset, size_t size, void *base) {
#ifdef HAVE_LIBNX
    // Map a view into the reserved memoryBase
    Result rc = svcMapProcessMemory(base, envGetOwnProcessHandle(), (u64)(memoryCodeBase + offset), size);
    if (R_FAILED(rc)) {
        return nullptr;
    }
    return base;
#else
    void *retval = mmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED | ((base == 0) ? 0 : MAP_FIXED), fd, offset);
    return (retval == MAP_FAILED) ? nullptr : retval;
#endif
}

void MemArena::ReleaseView(s64 offset, void* view, size_t size) {
#ifdef HAVE_LIBNX
    svcUnmapProcessMemory(view, envGetOwnProcessHandle(), (u64)(memoryCodeBase + offset), size);
#else
    munmap(view, size);
#endif 
}

u8* MemArena::Find4GBBase() {
#if PPSSPP_ARCH(64BIT) && defined(HAVE_LIBNX)
    size_t arena_size = 0x10000000; // 256MB

    if (!memorySrcBase) {
        memorySrcBase = (uintptr_t)memalign(0x1000, arena_size);
    }

    virtmemLock();
    if (!memoryBase) {
        void* addr = virtmemFindAslr(arena_size, 0x1000);
        if (addr) {
            baseRes = virtmemAddReservation(addr, arena_size);
            memoryBase = (uintptr_t)addr;
        }
    }
    if (!memoryCodeBase) {
        void* addr = virtmemFindAslr(arena_size, 0x1000);
        if (addr) {
            codeRes = virtmemAddReservation(addr, arena_size);
            memoryCodeBase = (uintptr_t)addr;
        }
    }
    virtmemUnlock();

    // Map the source memory to the code region for JIT
    Result rc = svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)memoryCodeBase, (u64)memorySrcBase, arena_size);
    
    if (R_SUCCEEDED(rc)) {
        svcSetProcessMemoryPermission(envGetOwnProcessHandle(), memoryCodeBase, arena_size, Perm_Rx);
    } else {
        // Fallback to avoid immediate crash
        return (u8*)memorySrcBase;
    }

    return (u8*)memoryBase;
#else
    size_t size = 0x10000000;
    void* base = mmap(0, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED | MAP_NORESERVE, -1, 0);
    munmap(base, size);
    return (u8*)base;
#endif
}
