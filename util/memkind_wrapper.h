#pragma once
#include "util/concurrent_arena.h"
#include <memkind.h>

namespace rocksdb {

class MemkindWrapper : public ConcurrentArena {
public:
    MemkindWrapper(const MemkindWrapper&) = delete;
    void operator=(const MemkindWrapper&) = delete;

    explicit MemkindWrapper(std::string pmem) {
        if(memkind_create_pmem(pmem.c_str(), 0, &_kind)) {
            throw("Cannot create pmem");
        }
    }

    ~MemkindWrapper() {
        memkind_destroy_kind(_kind);
    }

    char* Allocate(size_t bytes) override {
        return (char*)memkind_malloc(_kind, bytes);
    }

    char* AllocateAligned(size_t bytes, size_t huge_page_size = 0, Logger *logger = nullptr) override {
        _huge_page_size = huge_page_size;
        _logger = logger;
        return Allocate(bytes);
    }
private:
    size_t _huge_page_size;
    Logger *_logger;
    memkind_t _kind;
};
}
