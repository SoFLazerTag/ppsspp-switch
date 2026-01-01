// Copyright (C) 2003 Dolphin Project.
// ... (License headers)

#include "ppsspp_config.h"

#if !defined(_WIN32) && !defined(ANDROID) && !defined(__APPLE__)

#ifndef HAVE_LIBNX
#include <sys/mman.h>
#endif // !HAVE_LIBNX
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <string>

#ifndef MAP_NORESERVE
// Not implemented on BSDs
#define MAP_NORESERVE 0
#endif

#include "Common/Log.h"
#include "Common/File/FileUtil.h"
#include "Common/MemoryUtil.h"
#include "Common/MemArena.h"

static const std::string tmpfs_location = "/dev/shm";
static const std::string tmpfs_ram_temp_file = "/dev/shm/gc_mem.tmp";

#ifdef HAVE_LIBNX
#include <malloc.h> // memalign
#include <switch.h>

// We need to store the Reservation handles to release them properly later
static VirtmemReservation* baseRes = nullptr;
static VirtmemReservation* codeRes = nullptr;
static uintptr_t memoryBase = 0;
static uintptr_t memoryCodeBase = 0;
static uintptr_t memorySrcBase = 0;
#endif

// do not make this "static"
std::string ram_temp_file = "/tmp/gc_mem.tmp";

size_t MemArena::roundup(size_t x) {
    return x;
}

bool MemArena::NeedsProbing() {
    return false;
}

bool MemArena::GrabMemSpace(size_t size) {
#ifndef HAVE_LIBNX
    mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

    char ram_temp_filename[128]{};
    bool is_shm = false;
    for (int i = 0; i < 256; ++i) {
        snprintf(ram_temp_filename, sizeof(ram_temp_filename), "/ppsspp_%d.ram", i);
        fd = shm_open(ram_temp_filename, O_RDWR | O_CREAT | O_EXCL, mode);
        if (fd >= 0) {
            INFO_LOG(MEMMAP, "Got shm file: %s", ram_temp_filename);
            is_shm = true;
            if (shm_unlink(ram_temp_filename) != 0) {
                WARN_LOG(MEMMAP, "Failed to shm_unlink %s", ram_temp_file.c_str());
            }
            break;
        }
    }

    if (fd < 0 && File::Exists(Path(tmpfs_location))) {
        fd = open(tmpfs_ram_temp_file.c_str(), O_RDWR | O_CREAT, mode);
        if (fd >= 0) {
            ram_temp_file = tmpfs_ram_temp_file;
            INFO_LOG(MEMMAP, "Got tmpfs ram file: %s", tmpfs_ram_temp_file.c_str());
        }
    }

    if (fd < 0) {
        INFO_LOG(MEMMAP, "Trying '%s' as ram temp file", ram_temp_file.c_str());
        fd = open(ram_temp_file.c_str(), O_RDWR | O_CREAT, mode);
    }
    if (fd < 0) {
        ERROR_LOG(MEMMAP, "Failed to grab memory space as a file: %s of size: %08x. Error: %s", ram_temp_file.c_str(), (int)size, strerror(errno));
        return false;
    }
    if (!is_shm && unlink(ram_temp_file.c_str()) != 0) {
        WARN_LOG(MEMMAP, "Failed to unlink %s", ram_temp_file.c_str());
    }
    if (ftruncate(fd, size) != 0) {
        ERROR_LOG(MEMMAP, "Failed to ftruncate %d (%s) to size %08x", (int)fd, ram_temp_file.c_str(), (int)size);
    }

    return true;
#else
    return true;
#endif
}

void MemArena::ReleaseSpace() {
#ifndef HAVE_LIBNX
    close(fd);
#else
    if (memoryCodeBase && memorySrcBase) {
        svcUnmapProcessCodeMemory(envGetOwnProcessHandle(), (u64)memoryCodeBase, (u64)memorySrcBase, 0x10000000);
    }

    if (memorySrcBase) {
        free((void*)memorySrcBase);
        memorySrcBase = 0;
    }

    virtmemLock();
    if (baseRes) {
        virtmemRemoveReservation(baseRes);
        baseRes = nullptr;
        memoryBase = 0;
    }
    if (codeRes) {
        virtmemRemoveReservation(codeRes);
        codeRes = nullptr;
        memoryCodeBase = 0;
    }
    virtmemUnlock();
#endif
}

void *MemArena::CreateView(s64 offset, size_t size, void *base)
{
#ifdef HAVE_LIBNX
    Result rc = svcMapProcessMemory(base, envGetOwnProcessHandle(), (u64)(memoryCodeBase + offset), size);
    if (R_FAILED(rc)) {
        printf("Fatal error creating the view... base: %p offset: 0x%zx size: 0x%zx err: 0x%x\n", base, (size_t)offset, size, rc);
    }
    return base;
#else
    void *retval = mmap(base, size, PROT_READ | PROT_WRITE, MAP_SHARED |
#if defined(__DragonFly__) || defined(__FreeBSD__)
        MAP_NOSYNC |
#endif
        ((base == 0) ? 0 : MAP_FIXED), fd, offset);

    if (retval == MAP_FAILED) {
        NOTICE_LOG(MEMMAP, "mmap on %s (fd: %d) failed: %s", ram_temp_file.c_str(), (int)fd, strerror(errno));
        return 0;
    }
    return retval;
#endif
}

void MemArena::ReleaseView(s64 offset, void* view, size_t size) {
#ifndef HAVE_LIBNX
    munmap(view, size);
#else
    if (R_FAILED(svcUnmapProcessMemory(view, envGetOwnProcessHandle(), (u64)(memoryCodeBase + offset), size)))
        printf("Failed to unmap View...\n");
#endif 
}

u8* MemArena::Find4GBBase() {
#if PPSSPP_ARCH(64BIT) && !defined(USE_ASAN) && defined(HAVE_LIBNX)
    if (!memorySrcBase)
        memorySrcBase = (uintptr_t)memalign(0x1000, 0x10000000);

    virtmemLock();
    if (!memoryBase) {
        void* addr = virtmemFindAslr(0x10000000, 0x1000);
        if (addr) {
            baseRes = virtmemAddReservation(addr, 0x10000000);
            memoryBase = (uintptr_t)addr;
        }
    }
    if (!memoryCodeBase) {
        void* addr = virtmemFindAslr(0x10000000, 0x1000);
        if (addr) {
            codeRes = virtmemAddReservation(addr, 0x10000000);
            memoryCodeBase = (uintptr_t)addr;
        }
    }
    virtmemUnlock();

    if (R_FAILED(svcMapProcessCodeMemory(envGetOwnProcessHandle(), (u64)memoryCodeBase, (u64)memorySrcBase, 0x10000000)))
        printf("Failed to Map memory...\n");
    if (R_FAILED(svcSetProcessMemoryPermission(envGetOwnProcessHandle(), memoryCodeBase, 0x10000000, Perm_Rx)))
        printf("Failed to set perms...\n");

    return (u8*)memoryBase;
#else
    size_t size = 0x10000000;
    void* base = mmap(0, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED | MAP_NORESERVE, -1, 0);
    _assert_msg_(base != MAP_FAILED, "Failed to map 256 MB of memory space: %s", strerror(errno));
    munmap(base, size);
    return static_cast<u8*>(base);
#endif
}

#endif
