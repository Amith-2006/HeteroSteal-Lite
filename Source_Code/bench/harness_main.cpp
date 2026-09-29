// bench/harness_main.cpp
// HeteroSteal experimental harness entry point
// Milestone M2: WorkStealingDeque, TaskPool, and IoUringEngine integrated.

#include <iostream>
#include <string_view>
#include "heterosteal/deque.hpp"
#include "heterosteal/object_pool.hpp"
#if defined(HS_HAS_IOURING)
#include "heterosteal/iouring_engine.hpp"
#endif

namespace {

void print_usage(std::string_view program_name) {
    std::cout << "HeteroSteal Research Runtime Harness (Milestone M2)\n"
              << "Usage: " << program_name << " [options]\n\n"
              << "Options:\n"
              << "  -h, --help       Show this help message and exit\n"
              << "  -v, --version    Print version and build mode\n";
}

void print_version() {
    std::cout << "HeteroSteal version 0.2.0 (Milestone M2 - TaskPool & IoUringEngine)\n";
#if defined(__SANITIZE_THREAD__) || defined(HS_TSAN_BUILD)
    std::cout << "Build mode: ThreadSanitizer (TSAN) enabled\n";
#elif defined(__SANITIZE_ADDRESS__)
    std::cout << "Build mode: AddressSanitizer (ASAN) enabled\n";
#else
    std::cout << "Build mode: Standard / Plain\n";
#endif
#if defined(HS_HAS_IOURING)
    std::cout << "io_uring support: ENABLED\n";
#else
    std::cout << "io_uring support: DISABLED (host lacks liburing)\n";
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc > 1) {
        std::string_view arg = argv[1];
        if (arg == "-h" || arg == "--help") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "-v" || arg == "--version") {
            print_version();
            return 0;
        }
    }

    // Verify WorkStealingDeque & TaskPool instantiation
    hs::WorkStealingDeque deque(64);
    hs::TaskPool pool(128);

    std::cout << "HeteroSteal harness initialized.\n"
              << "  WorkStealingDeque capacity: " << deque.capacity() << "\n"
              << "  TaskPool capacity:          " << pool.capacity() << "\n";

#if defined(HS_HAS_IOURING)
    hs::IoUringConfig io_cfg{.sq_entries = 64, .cq_entries = 128};
    hs::IoUringEngine engine(io_cfg, pool, [](hs::TaskDescriptor*, int32_t, uint32_t) {});
    std::cout << "  IoUringEngine initialized:  sq=" << io_cfg.sq_entries
              << ", cq=" << io_cfg.cq_entries << "\n";
#endif

    return 0;
}
