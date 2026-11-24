# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Amodo Design Ltd

# pose-runtime.cmake — how the daemons link, per architecture.
#
#   x86_64, aarch64   glibc linked dynamically, everything else statically.
#                     A dynamic glibc is what lets the daemon dlopen() the
#                     GPU driver's libcuda.so.1, which is fragile or broken
#                     from a static glibc. A POST_BUILD step gathers the
#                     loader and the libraries the binary needs.
#   armv7 (BMC)       fully static: it runs on the BMC's own Linux, not in a
#                     UKI, so it must not depend on that distro's libc.

set(POSE_GATHER_RUNTIME ${CMAKE_CURRENT_LIST_DIR}/gather-runtime.sh)

# pose_daemon_link(tgt [LIB...]): LIB are further sonames whose closure the
# runtime must carry, for libraries the daemon dlopen()s rather than links.
function(pose_daemon_link tgt)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "^armv7")
        target_link_options(${tgt} PRIVATE -static)
    else()
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${POSE_GATHER_RUNTIME} $<TARGET_FILE:${tgt}>
                    ${CMAKE_CURRENT_BINARY_DIR}/runtime ${CMAKE_C_COMPILER} ${ARGN}
            COMMENT "Gathering the runtime libraries for ${tgt}"
            VERBATIM)
    endif()
endfunction()
