#ifndef TRACY_EMBEDDED_CAPTURE_H
#define TRACY_EMBEDDED_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { TRACY_EMBEDDED_CAPTURE_ABI_VERSION = 2 };

typedef enum tracy_embedded_capture_disposition {
    TRACY_EMBEDDED_CAPTURE_SAVE = 1,
    TRACY_EMBEDDED_CAPTURE_DISCARD = 2
} tracy_embedded_capture_disposition;

typedef enum tracy_embedded_capture_status {
    TRACY_EMBEDDED_CAPTURE_OK = 0,
    TRACY_EMBEDDED_CAPTURE_INVALID_ARGUMENT = 1,
    TRACY_EMBEDDED_CAPTURE_INVALID_STATE = 2,
    TRACY_EMBEDDED_CAPTURE_OUTPUT_EXISTS = 3,
    TRACY_EMBEDDED_CAPTURE_IO_ERROR = 4,
    TRACY_EMBEDDED_CAPTURE_TRANSPORT_ERROR = 5,
    TRACY_EMBEDDED_CAPTURE_NO_DATA = 6,
    TRACY_EMBEDDED_CAPTURE_INTERNAL_ERROR = 7
} tracy_embedded_capture_status;

typedef enum tracy_embedded_capture_state {
    TRACY_EMBEDDED_CAPTURE_UNCONFIGURED = 0,
    TRACY_EMBEDDED_CAPTURE_CONFIGURED = 1,
    TRACY_EMBEDDED_CAPTURE_CAPTURING = 2,
    TRACY_EMBEDDED_CAPTURE_FINISHING = 3,
    TRACY_EMBEDDED_CAPTURE_FINISHED = 4,
    TRACY_EMBEDDED_CAPTURE_FAILED = 5,
    TRACY_EMBEDDED_CAPTURE_DISCARDED = 6
} tracy_embedded_capture_state;

typedef struct tracy_embedded_capture_statistics {
    uint64_t client_to_server_bytes;
    uint64_t server_to_client_bytes;
    uint64_t client_to_server_high_water;
    uint64_t server_to_client_high_water;
    uint64_t writer_open_count;
    uint64_t worker_write_count;
    uint64_t publish_count;
} tracy_embedded_capture_statistics;

/* Configure one process-global capture. Call before Tracy manual startup.
 * `path` is copied and must point to `path_len` UTF-8 bytes. `channel_capacity`
 * must be non-zero. Existing output files are never overwritten. */
int32_t ___tracy_embedded_capture_configure(const char* path, size_t path_len,
                                             size_t channel_capacity,
                                             int64_t worker_memory_limit);

/* Finalize after every instrumentation-producing thread and active zone/guard
 * has quiesced. SAVE publishes atomically; DISCARD drains and destroys the
 * in-memory model without opening an output file. */
int32_t ___tracy_embedded_capture_finish_with_disposition(int32_t disposition);

/* Backward-compatible shorthand for SAVE. */
int32_t ___tracy_embedded_capture_finish(void);

uint32_t ___tracy_embedded_capture_abi_version(void);
int32_t ___tracy_embedded_capture_get_state(void);
int32_t ___tracy_embedded_capture_get_statistics(
    tracy_embedded_capture_statistics* statistics);

/* Copies a NUL-terminated diagnostic when capacity is non-zero and returns the
 * full diagnostic byte length excluding NUL. */
size_t ___tracy_embedded_capture_get_error(char* destination, size_t capacity);

#ifdef __cplusplus
}
#endif

#endif
