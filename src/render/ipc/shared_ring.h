// shared_ring.h
//
// A single-producer / single-consumer byte ring in Win32 shared memory,
// carrying the tagged message stream defined in scene_protocol.h.
//
// The producer is the 32-bit game process; the consumer is the 64-bit render
// host. Both map the same section, so the control block shares all the ABI
// constraints of the protocol header: fixed-width fields, explicit padding,
// and compile-time layout asserts.
//
// ── Synchronisation ──────────────────────────────────────────────────────
//
// One producer and one consumer, so a lock-free ring with two monotonically
// increasing 64-bit cursors is sufficient. `writeCursor` is written only by
// the producer, `readCursor` only by the consumer; each side reads the
// other's with acquire semantics and publishes its own with release. The
// cursors never wrap in practice (64 bits of bytes), so free space is simply
// `capacity - (write - read)` with no ambiguity between full and empty.
//
// Records are copied in two spans when they straddle the end of the buffer.
//
// ── Back-pressure ────────────────────────────────────────────────────────
//
// Resource messages must never be dropped — the host would render geometry it
// has no vertices for. Frame messages may be dropped freely, since the next
// frame supersedes them. `TryWrite` therefore takes a `droppable` flag: a
// droppable message that does not fit is counted and discarded, while a
// non-droppable one that does not fit fails loudly so the caller can retry or
// report. Nothing ever blocks the game's render thread.
#pragma once

#include "scene_protocol.h"

#include <stdint.h>

namespace SceneIPC
{
    // Shared control block, at offset 0 of the mapping. The data buffer
    // follows immediately after.
    // How many geometry ids the consumer can ask to have re-sent in one
    // frame. A request that does not fit is simply made again next frame, so
    // this bounds the table rather than the recovery.
    static const uint32_t kMaxResendRequests = 256;

    struct RingHeader
    {
        uint32_t magic;
        uint32_t version;
        uint32_t headerBytes;      // offset from mapping base to the data
        uint32_t _pad0;
        uint64_t capacityBytes;    // size of the data area

        volatile uint64_t writeCursor;   // producer-owned, monotonic
        volatile uint64_t readCursor;    // consumer-owned, monotonic

        volatile uint64_t framesProduced;
        volatile uint64_t framesDropped;
        volatile uint64_t bytesDropped;

        volatile uint32_t producerPid;
        volatile uint32_t consumerPid;
        volatile uint32_t producerAlive;
        volatile uint32_t consumerAlive;

        // ── Consumer -> producer: geometry it needs re-sent ───────────────
        //
        // The producer decides whether to re-send a geometry from its own
        // record of what it has already sent; the consumer caches
        // independently. Two caches with no channel between them agree only
        // by luck, and the arrangement here was meant to make disagreement
        // impossible: the producer forgets an id after 60 frames of not
        // drawing it, the consumer keeps one for 900, so anything the
        // producer believes cached must still be there.
        //
        // That reasoning is wrong, because those are different clocks. The
        // producer's counts frames in which it *drew* the geometry; the
        // consumer's counts frames it actually *processed*. Whenever the
        // consumer runs behind, the second advances more slowly than the
        // first, and geometry drawn continuously can age out of the
        // consumer's cache while the producer still believes it is there -
        // after which it is never re-sent, because nothing ever tells the
        // producer otherwise. Measured live: 278 of 992 instances in a frame,
        // every frame, permanently.
        //
        // So the consumer asks instead of the producer guessing. It writes
        // the ids it was told to draw but does not have; the producer drops
        // those from its sent-set and re-sends them on the next draw. One
        // writer, one reader, and a lost or overwritten request is just made
        // again next frame - so a release on the count is all the ordering
        // this needs.
        volatile uint32_t resendCount;
        uint32_t          _pad1;
        uint64_t          resendIds[kMaxResendRequests];
    };

    // 80 bytes of cursors and counters, then the resend table: 8 for the
    // count and its padding, plus 256 ids.
    static_assert(sizeof(RingHeader) == 88 + kMaxResendRequests * 8,
                  "RingHeader changed size - 32/64-bit ABI would diverge");
    static_assert(offsetof(RingHeader, resendCount) == 80, "RingHeader layout");
    static_assert(offsetof(RingHeader, resendIds)   == 88, "RingHeader layout");
    static_assert(offsetof(RingHeader, capacityBytes) == 16, "RingHeader layout");
    static_assert(offsetof(RingHeader, writeCursor)   == 24, "RingHeader layout");
    static_assert(offsetof(RingHeader, readCursor)    == 32, "RingHeader layout");
    static_assert(offsetof(RingHeader, producerPid)   == 64, "RingHeader layout");

    // Default name of the shared section. The host and the ASI must agree.
    extern const char* const kDefaultSectionName;

    class SharedRing
    {
    public:
        SharedRing();
        ~SharedRing();

        // Producer side: creates (or reopens) the section and initialises the
        // header. `capacityBytes` is rounded up to a multiple of 8.
        bool CreateAsProducer(const char* name, uint64_t capacityBytes);

        // Consumer side: opens an existing section. Fails if it does not
        // exist, or if the magic/version do not match this build.
        bool OpenAsConsumer(const char* name);

        void Close();
        bool IsOpen() const { return m_header != nullptr; }

        // ── Producer ─────────────────────────────────────────────────────
        // Writes one complete message: `header` followed by `payloadBytes`
        // from `payload`. The message is padded to an 8-byte boundary.
        //
        // Returns false when there is not enough free space. For a droppable
        // message the shortfall is recorded in bytesDropped and the caller
        // should simply carry on; for a non-droppable one the caller must
        // decide (retry, or mark the stream inconsistent).
        bool TryWrite(MessageType type, const void* fixedPart,
                      uint32_t fixedBytes, const void* payload,
                      uint32_t payloadBytes, bool droppable);

        // ── Consumer ─────────────────────────────────────────────────────
        // Copies the next message into `buffer`. Returns false when the ring
        // is empty or the message is larger than `bufferBytes` (in which case
        // `outNeededBytes` reports the size required and nothing is consumed).
        // Consumer side: ask for these geometry ids to be sent again. Later
        // calls in the same frame overwrite earlier ones, which is harmless -
        // the consumer re-derives the list from the next frame it builds.
        void RequestResend(const uint64_t* ids, uint32_t count);

        // Producer side: take whatever the consumer asked for and clear the
        // table. Returns how many ids were written to `out`.
        uint32_t TakeResendRequests(uint64_t* out, uint32_t max);

        bool TryRead(void* buffer, uint32_t bufferBytes,
                     uint32_t* outMessageBytes, uint32_t* outNeededBytes);

        // Bytes currently queued.
        uint64_t PendingBytes() const;

        const RingHeader* Header() const { return m_header; }

    private:
        uint64_t FreeBytes() const;
        void     CopyIn(uint64_t cursor, const void* src, uint32_t bytes);
        void     CopyOut(uint64_t cursor, void* dst, uint32_t bytes) const;

        void*        m_mapping;      // HANDLE, kept opaque to avoid windows.h
        void*        m_view;
        RingHeader*  m_header;
        uint8_t*     m_data;
        uint64_t     m_capacity;
        bool         m_isProducer;
    };
}
