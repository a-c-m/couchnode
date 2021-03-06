#ifndef LCB_MCREQ_H
#define LCB_MCREQ_H

#include <libcouchbase/couchbase.h>
#include <libcouchbase/api3.h>
#include <libcouchbase/vbucket.h>
#include <memcached/protocol_binary.h>
#include "netbuf/netbuf.h"
#include "sllist.h"
#include "config.h"
#include "packetutils.h"

#ifdef __cplusplus
extern "C" {
#endif /** __cplusplus */

/**
 * @file
 * @brief Core memcached client routines
 */

/**
 * @defgroup MCREQ Memcached Packets
 *
 * @brief
 * This module defines the core routines which are used to construct, handle,
 * and enqueue packets. They also handle the retry mechanisms.
 *
 *
 * # Initializing the Queue
 *
 * Using the mcreq system involves first establishing an mc_CMDQUEUE structure.
 * This structure contains several mc_PIPELINE structures. The proper way to
 * initialize the mc_CMDQEUE structure is to call mcreq_queue_init().
 *
 * Once the queue has been initialized, it must be assigned a
 * `VBUCKET_CONFIG_HANDLE` (which it will _not_ own). This is done via the
 * mcreq_queue_add_pipelines(). This function takes an array of pipeline pointers,
 * and this will typically be a "subclass" (mc_SERVER) allocated via
 * mcserver_alloc()
 *
 * Once the pipelines have been established, operations may be scheduled and
 * distributed across the various pipelines.
 *
 * # Creating a Packet
 *
 * For each packet sent, the packet should first be reserved via the
 * mcreq_basic_packet() call which allocates space for the actual packet
 * as well as provides and populates the vbucket fields as needed.
 *
 * The header size must be the total size of the header plus any extras
 * following the header but before the actual key data.
 *
 * If the command carries a body in addition to the key, it should be provided
 * via mcreq_reserve_value().
 *
 * Once the packet has a key and value it must be assigned a cookie. The
 * cookie may either be of a simple embedded type or an extended type. Whatever
 * the case the appropriate flags should be set.
 *
 * # Scheduling Commands
 *
 * Scheduling commands is performed in an _enter_ and _leave_ sequence.
 * mcreq_sched_enter() should be called before one or more commands are added.
 * Then for each new command added, mcreq_sched_add() should be invoked with
 * the new packet, and finally either mcreq_sched_leave() or mcreq_sched_fail()
 * should be invoked to flush the commands to the network or free the resources
 * allocated. In both cases the commands affected are scoped by the last call
 * to mcreq_sched_enter().
 *
 * In order for commands to actually be flushed, the mc_PIPELINE::flush_start
 * field must be set. This can vary depending on what the state of the underlying
 * socket is. In server.c for example, the initial callback just schedules a
 * connection. While the connection is in progress this field is set to a no-op
 * callback, and finally when the socket is connected this field is set to
 * interact with the I/O system which actually writes the buffers.
 *
 * # Flushing Responses
 *
 * This module does not do network I/O by design. Its only bridge is the
 * mc_PIPELINE::flush_start function which should be set to actually flush
 * the data.
 *
 * # Handling Reponses
 *
 * The I/O system reading the responses should place the response into a
 * packet_info structure. Once this is done, the request for the response must
 * be found using the opaque. This may be done with mcreq_pipeline_find()
 * or mcreq_pipeline_remove() depending on whether this request expects multiple
 * responses (such as the 'stat' command). These parameters should be passed
 * to the mcreq_dispatch_response() function which will invoke the appropriate
 * user-defined handler for it.
 *
 * If the packet does not expect more responses (as above), the application
 * should call mcreq_packet_handled()
 *
 *
 * # Error Handling and Failing Commands
 *
 * This module offers facilities for failing commands from a pipeline while
 * safely allowing for their sharing of user-allocated data.
 *
 * The mcreq_pipeline_fail() and mcreq_pipeline_timeout() will fail packets
 * in a single pipeline (the former failing all packets, the latter failing
 * only packets older than a specified threshold).
 *
 * The mcreq_iterwipe() will clean a pipeline of its packets, invoking a
 * callback which allows the user to relocate the packet to another pipeline.
 * In this callback the user may invoke the mcreq_dup_packet() function to
 * duplicate a packet's _data_ without duplicating its _state_.
 *
 * @addtogroup MCREQ
 * @{
 */


/**
 * @name Core Packet Structure
 * @{
 */

/** @brief Constant defining the size of a memcached header */
#define MCREQ_PKT_BASESIZE 24

/** @brief Embedded user data for a simple request */
typedef struct {
    const void *cookie; /**< User pointer to place in callbacks */
    hrtime_t start; /**< Time of the initial request. Used for timeouts */
} mc_REQDATA;

struct mc_packet_st;
struct mc_pipeline_st;

/**
 * Callback to be invoked for "Extended" packet handling. This is only
 * available in the mc_REQDATAEX structure
 * @param pipeline the pipeline on which the response was received
 * @param pkt the request packet
 * @param rc the error code for the response
 * @param arg opaque pointer for callback
 */
typedef void (*mcreq_pktex_callback)
        (struct mc_pipeline_st *pipeline, struct mc_packet_st *pkt,
                lcb_error_t rc, const void *res);

/** @brief Allocated user data for an extended request. */
typedef struct {
    const void *cookie; /**< User data */
    hrtime_t start; /**< Start time */
    mcreq_pktex_callback callback; /**< Callback to invoke upon being handled */
} mc_REQDATAEX;

/**
 * Called when the buffers for a packet have been invoked
 * @param pl the pipeline
 * @param ucookie the cookie passed to the scheduler
 * @param kbuf the pointer to the beginning of the key/header buffer, if
 *        passed to the scheduler
 * @param vbuf the pointer to the beginning of the value buffer or the first
 *        IOV within the buffer.
 */
typedef void (*mcreq_bufdone_fn)(struct mc_pipeline_st *pl,
        const void *ucookie, void *kbuf, void *vbuf);

/**
 * Possible values for the mc_PACKET#flags field in the packet structure.
 * These provide
 * information as to which fields in the various unions are in use, and how
 * to allocate/release data buffers for requests.
 */
typedef enum {
    /** The key is user-allocated. Do not release to MBLOCK */
    MCREQ_F_KEY_NOCOPY = 1 << 0,

    /** The value is user-allocated. Do not release to MBLOCK */
    MCREQ_F_VALUE_NOCOPY = 1 << 1,

    /**
     * The value is user allocated and in the form of an IOV.
     * Use mc_VALUE#multi
     */
    MCREQ_F_VALUE_IOV = 1 << 2,

    /** The request has a value. Use mc_VALUE#single unless otherwise noted */
    MCREQ_F_HASVALUE = 1 << 3,

    /**
     * The request is tied to an 'extended' user data structure.
     * Use mc_USER#exdata
     */
    MCREQ_F_REQEXT = 1 << 4,

    /** The request is a one-to-one user forwarded packet */
    MCREQ_F_UFWD = 1 << 5,

    /**
     * Indicates that the entire packet has been flushed. Specifically this
     * also indicates that the packet's underlying buffers are no longer needed
     * by libcouchbase.
     */
    MCREQ_F_FLUSHED = 1 << 6,

    /**
     * Indicates that the callback should NOT be invoked for the request. This
     * is typically because the request is just present in the queue for buffer
     * management purposes and has expired or otherwise been invalidated.
     */
    MCREQ_F_INVOKED = 1 << 7,

    /**
     * Forwarded packet which should be passed through and not processed
     * with extended callback logic. Just emit the frame data promptly.
     */
    MCREQ_F_PASSTHROUGH = 1 << 8,

    MCREQ_F_DETACHED = 1 << 9
} mcreq_flags;

/** @brief mask of flags indicating user-allocated buffers */
#define MCREQ_UBUF_FLAGS (MCREQ_F_KEY_NOCOPY|MCREQ_F_VALUE_NOCOPY)
/** @brief mask of flags indicating response state of the packet */
#define MCREQ_STATE_FLAGS (MCREQ_F_INVOKED|MCREQ_F_FLUSHED)

/** Union representing the value within a packet */
union mc_VALUE {
    /** For a single contiguous value */
    nb_SPAN single;

    /** For a set of multiple IOV buffers */
    lcb_FRAGBUF multi;
};

/** Union representing application/command data within a packet structure */
union mc_USER {
    /** Embedded command info for simple commands; 16 bytes, 48B */
    mc_REQDATA reqdata;

    /** Pointer to extended data */
    mc_REQDATAEX *exdata;
};

/**
 * @brief Packet structure for a single Memcached command
 *
 * A single packet structure is allocated for each request
 * sent to a server. A packet structure may be associated with user data in the
 * u_rdata union field, either by using the embedded structure, or by referencing
 * an allocated chunk of 'extended' user data.
 */
typedef struct mc_packet_st {
    /** Node in the linked list for logical command ordering */
    sllist_node slnode;

    /**
     * Node in the linked list for actual output ordering.
     * @see netbuf_end_flush2(), netbuf_pdu_enqueue()
     */
    sllist_node sl_flushq;

    /** Span for key and header */
    nb_SPAN kh_span;

    /** Extras length */
    uint8_t extlen;

    /** Retries */
    uint8_t retries;

    /** flags for request. @see mcreq_flags */
    uint16_t flags;

    /** Cached opaque value */
    uint32_t opaque;

    /** User/CMDAPI Data */
    union mc_USER u_rdata;

    /** Value data */
    union mc_VALUE u_value;

    /** Allocation data for the PACKET structure itself */
    nb_MBLOCK *alloc_parent;
} mc_PACKET;

/**
 * @brief Gets the request data from the packet structure itself
 * @return an mc_REQDATA or mc_REQDATAEX pointer
 */
#define MCREQ_PKT_RDATA(pkt) \
    (((pkt)->flags & MCREQ_F_REQEXT) \
        ? ((mc_REQDATA *)(pkt)->u_rdata.exdata) \
        : (&(pkt)->u_rdata.reqdata))

/**
 * @brief Retrieve the cookie pointer from a packet
 * @param pkt
 */
#define MCREQ_PKT_COOKIE(pkt) MCREQ_PKT_RDATA(pkt)->cookie

/**@}*/

/**
 * Callback invoked when APIs request that a pipeline start flushing. It
 * receives a pipeline object as its sole argument.
 */
typedef void (*mcreq_flushstart_fn)(struct mc_pipeline_st *pipeline);

/**
 * @brief Structure representing a single input/output queue for memcached
 *
 * Memcached request pipeline. This contains the command log for
 * sending/receiving requests. This is basically the non-I/O part of the server
 */
typedef struct mc_pipeline_st {
    /** List of requests. Newer requests are appended at the end */
    sllist_root requests;

    /** Parent command queue */
    struct mc_cmdqueue_st *parent;

    /**
     * Flush handler. This is invoked to schedule a flush operation
     * the socket
     */
    mcreq_flushstart_fn flush_start;

    /** Index of this server within the configuration map */
    int index;

    /**
     * Intermediate queue where pending packets are placed. Moved to
     * the `requests` list when mcreq_sched_leave() is called
     */
    sllist_root ctxqueued;

    /**
     * Callback invoked for each packet (which has user-defined buffers) when
     * it is no longer required
     */
    mcreq_bufdone_fn buf_done_callback;

    /** Buffer manager for the respective requests. */
    nb_MGR nbmgr;

    /** Allocator for packet structures */
    nb_MGR reqpool;
} mc_PIPELINE;

typedef struct mc_cmdqueue_st {
    /** Indexed pipelines, i.e. server map target */
    mc_PIPELINE **pipelines;

    /**
     * Small array of size npipelines, for mcreq_sched_enter()/mcreq_sched_leave()
     * stuff. See those functions for usage
     */
    char *scheds;

    /** Number of pipelines in the queue */
    unsigned npipelines;

    /** Sequence number for pipeline. Incremented for each new packet */
    uint32_t seq;

    /** Configuration handle for vBucket mapping */
    VBUCKET_CONFIG_HANDLE config;

    /** Number of pending items which have not yet been marked as 'done' */
    unsigned nremaining;

    lcb_t instance;
} mc_CMDQUEUE;

/**
 * Allocate a packet belonging to a specific pipeline.
 * @param pipeline the pipeline to allocate against
 * @return a new packet structure or NULL on error
 */
mc_PACKET *
mcreq_allocate_packet(mc_PIPELINE *pipeline);


/**
 * Free the packet structure. This will simply free the skeleton structure.
 * The underlying members will not be touched.
 * @param pipeline the pipleine which was used to allocate the packet
 * @param packet the packet to release
 */
void
mcreq_release_packet(mc_PIPELINE *pipeline, mc_PACKET *packet);


/**
 * Detatches the packet src belonging to the given pipeline. A detached
 * packet has all its data allocated via malloc and does not belong to
 * any particular buffer. This is typically used for relocation or retries
 * where it is impractical to affect the in-order netbuf allocator.
 *
 * @param src the source packet to copy
 * @return a new packet structure. You should still clear the packet's data
 * with wipe_packet/release_packet but you may pass NULL as the pipeline
 * parameter.
 */
mc_PACKET *
mcreq_dup_packet(const mc_PACKET *src);

/**
 * Reserve the packet's basic header structure, this is for use for frames
 * which do not contain keys, or contain fixed size data which does not
 * need to be returned via get_key
 * @param pipeline the pipeline to use
 * @param packet the packet which should contain the header
 * @param hdrsize the total size of the header+extras+key
 */
lcb_error_t
mcreq_reserve_header(
        mc_PIPELINE *pipeline, mc_PACKET *packet, uint8_t hdrsize);

/**
 * Initialize the given packet's key structure
 * @param pipeline the pipeline used to allocate the packet
 * @param packet the packet which should have its key field initialized
 * @param hdrsize the size of the header before the key. This should contain
 *        the header size (i.e. 24 bytes) PLUS any extras therein.
 * @param kreq the user-provided key structure
 * @return LCB_SUCCESS on success, LCB_CLIENT_ENOMEM on allocation failure
 */
lcb_error_t
mcreq_reserve_key(
        mc_PIPELINE *pipeline, mc_PACKET *packet,
        uint8_t hdrsize, const lcb_KEYBUF *kreq);


/**
 * Initialize the given packet's value structure. Only applicable for storage
 * operations.
 * @param pipeline the pipeline used to allocate the packet
 * @param packet the packet whose value field should be initialized
 * @param vreq the user-provided structure containing the value parameters
 * @return LCB_SUCCESS on success, LCB_CLIENT_ENOMEM on allocation failure
 */
lcb_error_t
mcreq_reserve_value(mc_PIPELINE *pipeline, mc_PACKET *packet,
                    const lcb_VALBUF *vreq);

/**
 * Reserves value/body space, but doesn't actually copy the contents over
 * @param pipeline the pipeline to use
 * @param packet the packet to host the value
 * @param n the number of bytes to reserve
 */
lcb_error_t
mcreq_reserve_value2(mc_PIPELINE *pipeline, mc_PACKET *packet, lcb_size_t n);


/**
 * Enqueue the packet to the pipeline. This packet should have fully been
 * initialized. Specifically, the packet's data buffer contents (i.e. key,
 * header, and value) must not be modified once this function is called
 *
 * @param pipeline the target pipeline that the packet will be queued in
 * @param packet the packet to enqueue.
 * This function always succeeds.
 */
void
mcreq_enqueue_packet(mc_PIPELINE *pipeline, mc_PACKET *packet);

/**
 * Like enqueue packet, except it will also inspect the packet's timeout field
 * and if necessary, restructure the command inside the request list so that
 * it appears before newer commands.
 *
 * The default enqueue_packet() just appends the command to the end of the
 * queue while this will perform an additional check (and is less efficient)
 */
void
mcreq_reenqueue_packet(mc_PIPELINE *pipeline, mc_PACKET *packet);

/**
 * Wipe the packet's internal buffers, releasing them. This should be called
 * when the underlying data buffer fields are no longer needed, usually this
 * is called directly before release_packet.
 * Note that release_packet should be called to free the storage for the packet
 * structure itself.
 * @param pipeline the pipeline structure used to allocate this packet
 * @param packet the packet to wipe.
 */
void
mcreq_wipe_packet(mc_PIPELINE *pipeline, mc_PACKET *packet);

/**
 * Convenience function to extract the proper hashkey from a key_request_t
 * structure
 * @param key the key structure for the item key
 * @param hashkey the key structure for the hashkey
 * @param nheader the size of the header
 * @param[out] target the pointer to receive the target hashkey
 * @param[out] ntarget the pointer to receive the target length
 */
void
mcreq_extract_hashkey(
        const lcb_KEYBUF *key, const lcb_KEYBUF *hashkey,
        lcb_size_t nheader, const void **target, lcb_size_t *ntarget);


/**
 * Handle the basic requirements of a packet common to all commands
 * @param queue the queue
 * @param cmd the command base structure
 *
 * @param[out] req the request header which will be set with key, vbucket, and extlen
 *        fields. In other words, you do not need to initialize them once this
 *        function has completed.
 *
 * @param extlen the size of extras for this command
 * @param[out] packet a pointer set to the address of the allocated packet
 * @param[out] pipeline a pointer set to the target pipeline
 */

lcb_error_t
mcreq_basic_packet(
        mc_CMDQUEUE *queue, const lcb_CMDBASE *cmd,
        protocol_binary_request_header *req, uint8_t extlen,
        mc_PACKET **packet, mc_PIPELINE **pipeline);

/**
 * @brief Get the key from a packet
 * @param[in] packet The packet from which to retrieve the key
 * @param[out] key
 * @param[out] nkey
 */
void
mcreq_get_key(const mc_PACKET *packet, const void **key, lcb_size_t *nkey);

/** @brief Returns the size of the entire packet, in bytes */
uint32_t
mcreq_get_bodysize(const mc_PACKET *packet);

/**
 * @brief get the total packet size (header+body)
 * @param pkt the packet
 * @return the total size
 */
#define mcreq_get_size(pkt) (mcreq_get_bodysize(pkt) + MCREQ_PKT_BASESIZE)

/** Initializes a single pipeline object */
int
mcreq_pipeline_init(mc_PIPELINE *pipeline);

/** Cleans up any initialization from pipeline_init */
void
mcreq_pipeline_cleanup(mc_PIPELINE *pipeline);


/**
 * Set the pipelines that this queue will manage
 * @param queue the queue to take the pipelines
 * @param pipelines an array of pipeline pointers. The array itself will
 *        be owned by the queue but _not_ the underlying pipeline pointers
 * @param npipelines number of pipelines in the queue
 * @param config the configuration handle. The configuration is _not_ owned
 *        and _not_ copied and the caller must ensure it remains valid
 *        until it is replaces.
 */
void
mcreq_queue_add_pipelines(
        mc_CMDQUEUE *queue, mc_PIPELINE **pipelines,
        unsigned npipelines, VBUCKET_CONFIG_HANDLE config);


/**
 * Take ownership of the pipeline array set via add_pipelines.
 * @param queue the queue
 * @param count a pointer to the number of pipelines within the queue
 * @return the pipeline array.
 *
 * When this function completes another call to add_pipelines must be performed
 * in order for the queue to function properly.
 */
mc_PIPELINE **
mcreq_queue_take_pipelines(mc_CMDQUEUE *queue, unsigned *count);

int
mcreq_queue_init(mc_CMDQUEUE *queue);

void
mcreq_queue_cleanup(mc_CMDQUEUE *queue);

/**
 * @brief Add a packet to the current scheduling context
 * @param pipeline
 * @param pkt
 * @see mcreq_sched_enter()
 */
void
mcreq_sched_add(mc_PIPELINE *pipeline, mc_PACKET *pkt);

/**
 * @brief enter a scheduling scope
 * @param queue
 * @attention It is not safe to call this function twice
 * @volatile
 */
void
mcreq_sched_enter(struct mc_cmdqueue_st *queue);

/**
 * @brief successfully exit a scheduling scope
 *
 * All operations enqueued since the last call to mcreq_sched_enter() will be
 * placed in their respective pipelines' operation queue.
 *
 * @param queue
 * @param do_flush Whether the items in the queue should be flushed
 * @volatile
 */
void
mcreq_sched_leave(struct mc_cmdqueue_st *queue, int do_flush);

/**
 * @brief destroy all operations within the scheduling scope
 * All operations enqueued since the last call to mcreq_sched_enter() will
 * be destroyed
 * @param queue
 */
void
mcreq_sched_fail(struct mc_cmdqueue_st *queue);

/**
 * Find a packet with the given opaque value
 */
mc_PACKET *
mcreq_pipeline_find(mc_PIPELINE *pipeline, uint32_t opaque);

/**
 * Find and remove the packet with the given opaque value
 */
mc_PACKET *
mcreq_pipeline_remove(mc_PIPELINE *pipeline, uint32_t opaque);

/**
 * Handles a received packet in response to a command
 * @param pipeline the pipeline upon which the request was received
 * @param request the original request
 * @param response the packet received in the response
 * @param immerr an immediate error message. If this is not LCB_SUCCESS then
 *  the packet in `response` shall contain only a header and the request itself
 *  should be analyzed
 *
 * @return 0 on success, nonzero if the handler could not be found for the
 * command.
 */
int
mcreq_dispatch_response(mc_PIPELINE *pipeline, mc_PACKET *request,
                        packet_info *response, lcb_error_t immerr);


#define MCREQ_KEEP_PACKET 1
#define MCREQ_REMOVE_PACKET 2
/**
 * Callback used for packet iteration wiping
 *
 * @param queue the queue
 * @param srcpl the source pipeline which is being cleaned
 * @param pkt the packet which is being cleaned
 * @param cbarg the argument passed to the iterwipe
 *
 * @return one of MCREQ_KEEP_PACKET (if the packet should be kept inside the
 * pipeline) or MCREQ_REMOVE_PACKET (if the packet should not be kept)
 */
typedef int (*mcreq_iterwipe_fn)
        (mc_CMDQUEUE *queue, mc_PIPELINE *srcpl, mc_PACKET *pkt, void *cbarg);
/**
 * Wipe a single pipeline. This may be used to move and/or relocate
 * existing commands to other pipelines.
 * @param queue the queue to use
 * @param src the pipeline to wipe
 * @param callback the callback to invoke for each packet
 * @param arg the argument passed to the callback
 */
void
mcreq_iterwipe(mc_CMDQUEUE *queue, mc_PIPELINE *src,
               mcreq_iterwipe_fn callback, void *arg);


/**
 * Called when a packet does not need to have any more references to it
 * remaining. A packet by itself only has two implicit references; one is
 * a flush reference and the other is a handler reference.
 *
 * The flush reference is unset once the packet has been flushed and the
 * handler reference is unset once the packet's handler callback has been
 * invoked and the relevant data relayed to the user.
 *
 * Once this function is called, the packet passed will no longer be valid
 * and thus should not be used.
 */
void
mcreq_packet_done(mc_PIPELINE *pipeline, mc_PACKET *pkt);

/**
 * @brief Indicate that the packet was handled
 * @param pipeline the pipeline
 * @param pkt the packet which was handled
 * If the packet has also been flushed, the packet's storage will be released
 * and `pkt` will no longer point to valid memory.
 */
#define mcreq_packet_handled(pipeline, pkt) do { \
    (pkt)->flags |= MCREQ_F_INVOKED; \
    if ((pkt)->flags & MCREQ_F_FLUSHED) { \
        mcreq_packet_done(pipeline, pkt); \
    } \
} while (0);

/**
 * Callback to be invoked when a packet is about to be failed out from the
 * request queue. This should be used to possibly invoke handlers. The packet
 * will then be removed from the queue.
 * @param pipeline the pipeline which has been errored
 * @param packet the current packet
 * @param err the error received
 * @param arg an opaque pointer
 */
typedef void (*mcreq_pktfail_fn)
        (mc_PIPELINE *pipeline, mc_PACKET *packet, lcb_error_t err, void *arg);

/**
 * Fail out a given pipeline. All commands in the pipeline will be removed
 * from the pipeline (though they may still not be freed if they are pending
 * a flush).
 *
 * @param pipeline the pipeline to fail out
 * @param err the error which caused the failure
 * @param failcb a callback invoked to handle each failed packet
 * @param cbarg a pointer passed as the last parameter to the callback
 *
 * @return the number of items actually failed.
 */
unsigned
mcreq_pipeline_fail(
        mc_PIPELINE *pipeline, lcb_error_t err,
        mcreq_pktfail_fn failcb, void *cbarg);

/**
 * Fail out all commands in the pipeline which are older than a specified
 * interval. This is similar to the pipeline_fail() function except that commands
 * which are newer than the threshold are still kept
 *
 * @param pipeline the pipeline to fail out
 * @param err the error to provide to the handlers (usually LCB_ETIMEDOUT)
 * @param failcb the callback to invoke
 * @param cbarg the last argument to the callback
 * @param oldest_valid the _oldest_ time for a command to still be valid
 * @param oldest_start set to the start time of the _oldest_ command which is
 *        still valid.
 *
 * @return the number of commands actually failed.
 */
unsigned
mcreq_pipeline_timeout(
        mc_PIPELINE *pipeline, lcb_error_t err,
        mcreq_pktfail_fn failcb, void *cbarg,
        hrtime_t oldest_valid,
        hrtime_t *oldest_start);

void
mcreq_dump_packet(const mc_PACKET *pkt);

void
mcreq_dump_chain(const mc_PIPELINE *pipeline);

#define mcreq_write_hdr(pkt, hdr) \
        memcpy( SPAN_BUFFER(&(pkt)->kh_span), (hdr)->bytes, sizeof((hdr)->bytes) )

#define mcreq_write_exhdr(pkt, hdr, n) \
        memcpy(SPAN_BUFFER((&pkt)->kh_span), (hdr)->bytes, n)

#define mcreq_read_hdr(pkt, hdr) \
        memcpy( (hdr)->bytes, SPAN_BUFFER(&(pkt)->kh_span), sizeof((hdr)->bytes) )

#define mcreq_first_packet(pipeline) \
        SLLIST_IS_EMPTY(&(pipeline)->requests) ? NULL : \
                SLLIST_ITEM((pipeline)->requests.first, mc_PACKET, slnode)

/**@}*/

#ifdef __cplusplus
}
#endif /** __cplusplus */
#endif /* LCB_MCREQ_H */
