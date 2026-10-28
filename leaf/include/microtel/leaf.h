/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file leaf.h
 * @brief microtel-leaf: build spans in caller-owned memory and encode them as
 *        an OTLP ExportTraceServiceRequest.
 *
 * **Experimental in v1.2.** This C API may change in any 1.x minor release; it
 * becomes stable in v2.0. The design is `docs/leaf-concentrator-design.md` §1.
 *
 * The leaf never allocates. The caller provides the leaf state
 * (`microtel_leaf_t`), a record buffer that holds every started span until it
 * is encoded, and an output buffer (or a write callback) per encode. It starts
 * no thread, does no I/O and does not frame its payloads: the application owns
 * the transport and sends the bytes to a concentrator (`LeafReceiver`).
 *
 * A `microtel_leaf_t` is not thread-safe. Serialise access to one leaf, or use
 * one leaf per execution context.
 *
 * Every string parameter is UTF-8 with an explicit length and is copied into
 * the record buffer, so the caller's pointer need only be valid for the call.
 * The leaf does not validate UTF-8; the concentrator rejects a payload that is
 * not valid UTF-8.
 *
 * **Ids on weak entropy (§1.6.1).** Ids are never taken from `random_bytes`
 * directly: each 64-bit id word is `splitmix64(device_key + counter) XOR
 * random`, where `device_key` hashes the configured Resource and `boot_id`. A
 * fleet whose `random_bytes` is the same PRNG sequence on every device still
 * gets distinct ids, as long as the Resource names the device (`device.id`,
 * `service.instance.id`, a serial number) and `boot_id` changes per boot. What
 * this cannot fix: a fleet whose PRNG, Resource and `boot_id` are all identical
 * on every device produces the same ids everywhere.
 *
 * Valid C11 (`-std=c11 -pedantic-errors`) and valid C++.
 */

#ifndef MICROTEL_LEAF_H
#define MICROTEL_LEAF_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/** @name Library version
 *  Equal to the microtel release. `microtel_leaf_version()` returns the same
 *  numbers from the compiled library, so a firmware can detect a header /
 *  archive mismatch.
 *  @{ */
#define MICROTEL_LEAF_VERSION_MAJOR 1u
#define MICROTEL_LEAF_VERSION_MINOR 2u
#define MICROTEL_LEAF_VERSION_PATCH 5u
/** @} */

/** Packs a version as `(major << 16) | (minor << 8) | patch`. */
#define MICROTEL_LEAF_VERSION_PACK(major, minor, patch)                                            \
    ((((uint32_t)(major)) << 16u) | (((uint32_t)(minor)) << 8u) | ((uint32_t)(patch)))

/** The version this header describes, packed as by MICROTEL_LEAF_VERSION_PACK. */
#define MICROTEL_LEAF_VERSION                                                                      \
    MICROTEL_LEAF_VERSION_PACK(                                                                    \
        MICROTEL_LEAF_VERSION_MAJOR, MICROTEL_LEAF_VERSION_MINOR, MICROTEL_LEAF_VERSION_PATCH)

/** Size of the opaque leaf state, in 64-bit words. */
#define MICROTEL_LEAF_STATE_WORDS 32u

/**
 * @brief Opaque leaf state, allocated by the caller (static, stack or heap).
 *
 * Pass `sizeof(microtel_leaf_t)` to `microtel_leaf_init`, which refuses a size
 * smaller than the library was built with, so a header older than the archive
 * cannot make the library write past the caller's object.
 */
typedef struct microtel_leaf
{
    uint64_t microtel_private[MICROTEL_LEAF_STATE_WORDS];
} microtel_leaf_t;

/**
 * @brief A span handle: slot index in the low 16 bits, generation count in the
 *        high 16 bits. A handle whose span has since been encoded is rejected
 *        with `MICROTEL_LEAF_ERR_STATE`. 0 is never a valid handle.
 */
typedef uint32_t microtel_leaf_span_t;

/** @brief Result of every leaf call. Negative values are errors. */
typedef enum microtel_leaf_status
{
    MICROTEL_LEAF_OK = 0,
    MICROTEL_LEAF_ERR_ARG = -1,          /**< NULL or out-of-range argument */
    MICROTEL_LEAF_ERR_STATE = -2,        /**< not initialised, freed, stale or ended handle */
    MICROTEL_LEAF_ERR_NO_SPACE = -3,     /**< record buffer full; the item was dropped */
    MICROTEL_LEAF_ERR_LIMIT = -4,        /**< per-span cap reached; the item was dropped */
    MICROTEL_LEAF_ERR_BUFFER_SMALL = -5, /**< output buffer too small; nothing consumed */
    MICROTEL_LEAF_ERR_ENCODE = -6,       /**< backend failure, or the write callback failed */
    MICROTEL_LEAF_ERR_CLOCK = -7         /**< clock_sync given an invalid time */
} microtel_leaf_status_t;

/** @brief How the leaf's timestamps relate to wall time (design §5). */
typedef enum microtel_leaf_time_mode
{
    MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED = 0,
    MICROTEL_LEAF_TIME_SYNC_RELATIVE = 1,
    MICROTEL_LEAF_TIME_BOOT_RELATIVE = 2
} microtel_leaf_time_mode_t;

/** @brief Attribute value types the leaf can emit. */
typedef enum microtel_leaf_value_type
{
    MICROTEL_LEAF_VALUE_BOOL = 0,
    MICROTEL_LEAF_VALUE_INT64 = 1,
    MICROTEL_LEAF_VALUE_DOUBLE = 2,
    MICROTEL_LEAF_VALUE_STRING = 3
} microtel_leaf_value_type_t;

/** @brief One attribute. Keys must be non-empty. The layout is frozen for 1.x. */
typedef struct microtel_leaf_kv
{
    const char* key;
    size_t key_len;
    microtel_leaf_value_type_t type;
    union
    {
        int b; /**< MICROTEL_LEAF_VALUE_BOOL: 0 is false, anything else true */
        int64_t i;
        double d;
        struct
        {
            const char* ptr;
            size_t len;
        } s;
    } value;
} microtel_leaf_kv_t;

/**
 * @brief Leaf configuration. Zero-initialise it, then set `struct_size` to
 *        `sizeof(microtel_leaf_config_t)`.
 */
typedef struct microtel_leaf_config
{
    /** `sizeof(microtel_leaf_config_t)`. A later minor may append fields. */
    uint32_t struct_size;
    microtel_leaf_time_mode_t time_mode;

    /** Monotonic clock in nanoseconds. NULL means the leaf has no clock, which
     *  is allowed only in MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED. */
    uint64_t (*now_ns)(void* ctx);
    void* clock_ctx;

    /** Required: fills `out` with `len` random bytes, for trace and span ids.
     *  The byte stream must differ between devices and between boots of one
     *  device: seed it from a hardware RNG, or at least from a per-chip unique
     *  id plus boot_id. It need not be cryptographically secure. */
    void (*random_bytes)(void* ctx, uint8_t* out, size_t len);
    void* random_ctx;

    /** The leaf's own Resource. Copied at init. Keys starting with
     *  `microtel.leaf.` are reserved and rejected. */
    const microtel_leaf_kv_t* resource;
    size_t resource_count;

    /** Instrumentation scope for every span. Copied at init. */
    const char* scope_name;
    size_t scope_name_len;
    const char* scope_version;
    size_t scope_version_len;

    /** Per-span caps; 0 selects the default in brackets. */
    uint16_t max_attributes_per_span;  /**< [16] */
    uint16_t max_events_per_span;      /**< [4] */
    uint16_t max_attributes_per_event; /**< [4] */

    /** A value that differs on every boot: mixed into ids, and sent as the
     *  boot identity in boot-relative mode. 0 if unavailable. */
    uint32_t boot_id;

    /** upb backend only: memory for the encode arena. NULL means the heap.
     *  With a buffer, an encode that needs more fails with
     *  MICROTEL_LEAF_ERR_ENCODE instead of touching the heap. */
    void* scratch;
    size_t scratch_size;
} microtel_leaf_config_t;

/** @brief Span kinds; the values match OTLP's. */
typedef enum microtel_leaf_span_kind
{
    MICROTEL_LEAF_SPAN_KIND_INTERNAL = 1,
    MICROTEL_LEAF_SPAN_KIND_SERVER = 2,
    MICROTEL_LEAF_SPAN_KIND_CLIENT = 3,
    MICROTEL_LEAF_SPAN_KIND_PRODUCER = 4,
    MICROTEL_LEAF_SPAN_KIND_CONSUMER = 5
} microtel_leaf_span_kind_t;

/** @brief Span status codes; the values match OTLP's. */
typedef enum microtel_leaf_status_code
{
    MICROTEL_LEAF_STATUS_UNSET = 0,
    MICROTEL_LEAF_STATUS_OK = 1,
    MICROTEL_LEAF_STATUS_ERROR = 2
} microtel_leaf_status_code_t;

/** @brief Leaf-side drop counters since the last successful encode. */
typedef struct microtel_leaf_counters
{
    uint32_t dropped_spans;      /**< spans refused at start for lack of space */
    uint32_t dropped_attributes; /**< attributes refused for space or limits */
    uint32_t dropped_events;     /**< events refused for space or limits */
} microtel_leaf_counters_t;

/** @brief The compiled library's version, packed as MICROTEL_LEAF_VERSION. */
uint32_t microtel_leaf_version(void);

/**
 * @brief Initialises a leaf.
 *
 * Checks `leaf_size` first and returns MICROTEL_LEAF_ERR_ARG without writing
 * anything if it is smaller than `sizeof(microtel_leaf_t)` as the library was
 * built. Then validates the config and copies the Resource and scope into the
 * record buffer. On any error neither `leaf` nor the record buffer is touched.
 *
 * @param leaf               caller-allocated state
 * @param leaf_size          `sizeof(microtel_leaf_t)`
 * @param config             copied; need not outlive the call
 * @param record_buffer      holds spans until they are encoded; owned by the
 *                           caller and borrowed until microtel_leaf_free
 * @param record_buffer_size its size in bytes
 * @return MICROTEL_LEAF_OK, MICROTEL_LEAF_ERR_ARG, or MICROTEL_LEAF_ERR_NO_SPACE
 *         when the buffer cannot hold the Resource and scope
 */
microtel_leaf_status_t microtel_leaf_init(microtel_leaf_t* leaf,
                                          size_t leaf_size,
                                          const microtel_leaf_config_t* config,
                                          void* record_buffer,
                                          size_t record_buffer_size);

/**
 * @brief Ends the leaf's use of its memory. Releases nothing (there is nothing
 *        to release) but clears the state, so later calls return
 *        MICROTEL_LEAF_ERR_STATE. The record buffer may then be reused.
 */
void microtel_leaf_free(microtel_leaf_t* leaf);

/**
 * @brief Starts a span.
 * @param parent NULL for a root span, or a handle to an open or ended (not yet
 *               encoded) span of the same leaf
 */
microtel_leaf_status_t microtel_leaf_span_start(microtel_leaf_t* leaf,
                                                microtel_leaf_span_t* out_span,
                                                const char* name,
                                                size_t name_len,
                                                microtel_leaf_span_kind_t kind,
                                                const microtel_leaf_span_t* parent);

/** @brief Starts a span whose parent came from upstream. Both ids must be non-zero. */
microtel_leaf_status_t microtel_leaf_span_start_remote(microtel_leaf_t* leaf,
                                                       microtel_leaf_span_t* out_span,
                                                       const char* name,
                                                       size_t name_len,
                                                       microtel_leaf_span_kind_t kind,
                                                       const uint8_t trace_id[16],
                                                       const uint8_t parent_span_id[8]);

/** @brief Sets an attribute on an open span. Setting a key twice overwrites
 *         the value and keeps the attribute's position. */
microtel_leaf_status_t microtel_leaf_span_set_attribute(microtel_leaf_t* leaf,
                                                        microtel_leaf_span_t span,
                                                        const microtel_leaf_kv_t* attr);

/** @brief Adds an event to an open span. Attributes past
 *         max_attributes_per_event are dropped and counted. */
microtel_leaf_status_t microtel_leaf_span_add_event(microtel_leaf_t* leaf,
                                                    microtel_leaf_span_t span,
                                                    const char* name,
                                                    size_t name_len,
                                                    const microtel_leaf_kv_t* attrs,
                                                    size_t attr_count);

/** @brief Sets an open span's status. A second call overwrites the first. */
microtel_leaf_status_t microtel_leaf_span_set_status(microtel_leaf_t* leaf,
                                                     microtel_leaf_span_t span,
                                                     microtel_leaf_status_code_t code,
                                                     const char* message,
                                                     size_t message_len);

/** @brief Ends an open span; it is encoded by the next microtel_leaf_encode. */
microtel_leaf_status_t microtel_leaf_span_end(microtel_leaf_t* leaf, microtel_leaf_span_t span);

/**
 * @brief Records a wall-clock sync for MICROTEL_LEAF_TIME_SYNC_RELATIVE: the leaf
 *        clock read `leaf_now_ns` when Unix time was `unix_ns`.
 * @return MICROTEL_LEAF_ERR_CLOCK when `unix_ns` is 0, the leaf has no clock, or
 *         `leaf_now_ns` is ahead of the leaf's own clock
 */
microtel_leaf_status_t microtel_leaf_clock_sync(microtel_leaf_t* leaf,
                                                uint64_t unix_ns,
                                                uint64_t leaf_now_ns);

/** @brief Writes min(out_size, sizeof(microtel_leaf_counters_t)) bytes. */
void microtel_leaf_get_counters(const microtel_leaf_t* leaf,
                                microtel_leaf_counters_t* out,
                                size_t out_size);

/**
 * @brief Encodes every ended span into one ExportTraceServiceRequest in `out`.
 *
 * The payload is the protobuf body only: no gRPC prefix, no compression, one
 * ResourceSpans holding one ScopeSpans. On MICROTEL_LEAF_OK the encoded spans
 * are released from the record buffer and the drop counters reset. On
 * MICROTEL_LEAF_ERR_BUFFER_SMALL nothing is consumed and `*written` is the size
 * needed.
 *
 * With no span ended and every drop counter zero, the encode is a header only,
 * which a concentrator rejects as malformed: do not send it.
 */
microtel_leaf_status_t microtel_leaf_encode(microtel_leaf_t* leaf,
                                            uint8_t* out,
                                            size_t out_size,
                                            size_t* written);

/**
 * @brief Streaming form: bytes are handed to `write` in order. `write` returns 0
 *        on success; any other value aborts the encode with
 *        MICROTEL_LEAF_ERR_ENCODE and leaves the spans in the buffer. The upb
 *        backend calls `write` once with the whole payload; the nanopb backend
 *        calls it for each piece as it is encoded (every tag, length and
 *        value), so the payload is never held whole.
 */
microtel_leaf_status_t microtel_leaf_encode_to(microtel_leaf_t* leaf,
                                               int (*write)(void* ctx,
                                                            const uint8_t* bytes,
                                                            size_t len),
                                               void* write_ctx,
                                               size_t* written);

/** @brief Upper bound on the next encode's size, for sizing `out`. 0 on error. */
size_t microtel_leaf_encoded_size(const microtel_leaf_t* leaf);

#ifdef __cplusplus
}
#endif

#endif /* MICROTEL_LEAF_H */
