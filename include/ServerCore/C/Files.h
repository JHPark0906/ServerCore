#ifndef SERVERCORE_C_FILES_H
#define SERVERCORE_C_FILES_H
#include "ServerCore/C/Types.h"
#ifdef __cplusplus
extern "C"
{
#endif
    enum
    {
        SC_FILE_SYNC_NONE = 0,
        SC_FILE_SYNC_FILE = 1,
        SC_FILE_SYNC_DIRECTORY = 2
    };
    typedef struct sc_atomic_file sc_atomic_file;
    typedef struct sc_atomic_file_options
    {
        uint32_t abi_version, struct_size;
        uint64_t maximum_bytes;
        uint32_t sync, reserved;
    } sc_atomic_file_options;
    SC_API sc_status sc_atomic_file_options_init(sc_atomic_file_options*, size_t size) SC_NOEXCEPT;
    SC_API int sc_file_supports_directory_sync(void) SC_NOEXCEPT;
    /* Blocking calls. Serialize handle access. Path is UTF-8 and copied. Parent is
 * an existing trusted directory. Drop/cancel removes the uncommitted temporary.
 * No application file is modified until commit. Directory sync is explicit and
 * unsupported modes fail before creating a file; no durability downgrade. */
    SC_API sc_status sc_atomic_file_create(
        sc_bytes target, const sc_atomic_file_options*, sc_atomic_file** out) SC_NOEXCEPT;
    SC_API sc_status sc_atomic_file_write(sc_atomic_file*, sc_bytes data) SC_NOEXCEPT;
    SC_API sc_status sc_atomic_file_commit(sc_atomic_file*) SC_NOEXCEPT;
    /* Inspect committed even after commit errors: post-rename sync may fail. */
    SC_API int sc_atomic_file_committed(const sc_atomic_file*) SC_NOEXCEPT;
    SC_API uint64_t sc_atomic_file_written(const sc_atomic_file*) SC_NOEXCEPT;
    SC_API sc_status sc_atomic_file_cancel(sc_atomic_file*) SC_NOEXCEPT;
    SC_API void sc_atomic_file_destroy(sc_atomic_file*) SC_NOEXCEPT;
#ifdef __cplusplus
}
#endif
#endif
