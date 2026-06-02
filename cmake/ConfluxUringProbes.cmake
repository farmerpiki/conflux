function(conflux_configure_uring_probes target)
    include(CheckCXXSourceCompiles)
    include(CheckCXXSourceRuns)
    set(_conflux_saved_required_libraries "${CMAKE_REQUIRED_LIBRARIES}")
    set(CMAKE_REQUIRED_LIBRARIES PkgConfig::LIBURING)

    check_cxx_source_compiles("
#include <liburing.h>
int main() {
    auto* fn = &io_uring_resize_rings;
    auto _ = fn;
    return 0;
}
" CONFLUX_HAVE_IO_URING_RESIZE_RINGS)
    check_cxx_source_compiles("
#include <liburing.h>
int main() {
    auto* fn = &io_uring_clone_buffers_offset;
    auto _ = fn;
    return 0;
}
" CONFLUX_HAVE_IO_URING_CLONE_BUFFERS_OFFSET)
    check_cxx_source_runs("
#include <liburing.h>
int main() {
    io_uring ring{};
    if (io_uring_queue_init(8, &ring, 0) < 0) {
        return 1;
    }
    int err = 0;
    io_uring_buf_ring* br = io_uring_setup_buf_ring(&ring, 8, 0, IOU_PBUF_RING_INC, &err);
    if (br == nullptr) {
        io_uring_queue_exit(&ring);
        return 1;
    }
    io_uring_free_buf_ring(&ring, br, 8, 0);
    io_uring_queue_exit(&ring);
    return 0;
}
" CONFLUX_RUNTIME_HAS_IOU_PBUF_RING_INC)
    if(CONFLUX_HAVE_IO_URING_RESIZE_RINGS)
        check_cxx_source_runs("
#include <liburing.h>
int main() {
    io_uring ring{};
    io_uring_params init{};
    init.flags = IORING_SETUP_DEFER_TASKRUN;
    if (io_uring_queue_init_params(8, &ring, &init) < 0) {
        return 1;
    }
    io_uring_params resize{};
    resize.flags = IORING_SETUP_CQSIZE;
    resize.sq_entries = 8;
    resize.cq_entries = 16;
    int rc = io_uring_resize_rings(&ring, &resize);
    io_uring_queue_exit(&ring);
    return rc < 0 ? 1 : 0;
}
" CONFLUX_RUNTIME_HAS_IO_URING_RESIZE_RINGS)
    else()
        set(CONFLUX_RUNTIME_HAS_IO_URING_RESIZE_RINGS FALSE)
    endif()
    if(CONFLUX_HAVE_IO_URING_CLONE_BUFFERS_OFFSET)
        check_cxx_source_runs("
#include <liburing.h>
#include <sys/uio.h>
int main() {
    io_uring src{};
    io_uring dst{};
    char storage[4096]{};
    iovec vec{storage, sizeof(storage)};
    if (io_uring_queue_init(8, &src, 0) < 0) {
        return 1;
    }
    if (io_uring_queue_init(8, &dst, 0) < 0) {
        io_uring_queue_exit(&src);
        return 1;
    }
    if (io_uring_register_buffers(&src, &vec, 1) < 0) {
        io_uring_queue_exit(&dst);
        io_uring_queue_exit(&src);
        return 1;
    }
    int rc = io_uring_clone_buffers_offset(&dst, &src, 0, 0, 1, 0);
    io_uring_unregister_buffers(&src);
    io_uring_queue_exit(&dst);
    io_uring_queue_exit(&src);
    return rc < 0 ? 1 : 0;
}
" CONFLUX_RUNTIME_HAS_IO_URING_CLONE_BUFFERS)
    else()
        set(CONFLUX_RUNTIME_HAS_IO_URING_CLONE_BUFFERS FALSE)
    endif()
    set(CMAKE_REQUIRED_LIBRARIES "${_conflux_saved_required_libraries}")
    unset(_conflux_saved_required_libraries)

    target_compile_definitions(${target}
        PUBLIC CONFLUX_HAVE_IO_URING_RESIZE_RINGS=$<BOOL:${CONFLUX_HAVE_IO_URING_RESIZE_RINGS}>
        PUBLIC CONFLUX_RUNTIME_HAS_IOU_PBUF_RING_INC=$<BOOL:${CONFLUX_RUNTIME_HAS_IOU_PBUF_RING_INC}>
        PUBLIC CONFLUX_RUNTIME_HAS_IO_URING_RESIZE_RINGS=$<BOOL:${CONFLUX_RUNTIME_HAS_IO_URING_RESIZE_RINGS}>
        PUBLIC CONFLUX_RUNTIME_HAS_IO_URING_CLONE_BUFFERS=$<BOOL:${CONFLUX_RUNTIME_HAS_IO_URING_CLONE_BUFFERS}>
    )
endfunction()
