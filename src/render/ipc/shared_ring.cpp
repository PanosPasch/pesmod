// shared_ring.cpp
#include "shared_ring.h"

#include <windows.h>
#include <cstring>

namespace SceneIPC
{

const char* const kDefaultSectionName = "Local\\PESMod.SceneStream";

namespace
{
    // The cursors are plain 64-bit values written by one side and read by the
    // other. On x86 a naturally aligned 64-bit load or store is not guaranteed
    // atomic, so both sides go through interlocked helpers rather than reading
    // the field directly — otherwise a torn cursor would desynchronise the
    // ring in a way that only shows up under load.
    inline uint64_t LoadAcquire64(const volatile uint64_t* p)
    {
        const uint64_t v = (uint64_t)InterlockedCompareExchange64(
            (volatile LONG64*)p, 0, 0);
        return v;
    }

    inline void StoreRelease64(volatile uint64_t* p, uint64_t value)
    {
        InterlockedExchange64((volatile LONG64*)p, (LONG64)value);
    }

    inline uint32_t ExchangeRelease32(volatile uint32_t* p, uint32_t value)
    {
        return (uint32_t)InterlockedExchange((volatile LONG*)p, (LONG)value);
    }

    inline void Increment64(volatile uint64_t* p, uint64_t delta)
    {
        InterlockedAdd64((volatile LONG64*)p, (LONG64)delta);
    }
}

SharedRing::SharedRing()
    : m_mapping(nullptr)
    , m_view(nullptr)
    , m_header(nullptr)
    , m_data(nullptr)
    , m_capacity(0)
    , m_isProducer(false)
{
}

SharedRing::~SharedRing()
{
    Close();
}

bool SharedRing::CreateAsProducer(const char* name, uint64_t capacityBytes)
{
    Close();

    capacityBytes = (capacityBytes + 7ull) & ~7ull;
    if (capacityBytes < 64ull * 1024ull) capacityBytes = 64ull * 1024ull;

    const uint64_t total = sizeof(RingHeader) + capacityBytes;

    HANDLE mapping = CreateFileMappingA(
        INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        (DWORD)(total >> 32), (DWORD)(total & 0xFFFFFFFFull),
        name ? name : kDefaultSectionName);
    if (!mapping) return false;

    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)total);
    if (!view)
    {
        CloseHandle(mapping);
        return false;
    }

    m_mapping    = mapping;
    m_view       = view;
    m_header     = (RingHeader*)view;
    m_data       = (uint8_t*)view + sizeof(RingHeader);
    m_capacity   = capacityBytes;
    m_isProducer = true;

    // The producer owns initialisation. A stale section left by a previous
    // run is simply reset — the host re-sends everything on reconnect.
    memset(m_header, 0, sizeof(RingHeader));
    m_header->magic         = kSceneMagic;
    m_header->version       = kSceneVersion;
    m_header->headerBytes   = (uint32_t)sizeof(RingHeader);
    m_header->capacityBytes = capacityBytes;
    m_header->producerPid   = GetCurrentProcessId();
    m_header->producerAlive = 1;
    return true;
}

bool SharedRing::OpenAsConsumer(const char* name)
{
    Close();

    HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE,
                                      name ? name : kDefaultSectionName);
    if (!mapping) return false;

    // Map the header first: the producer decides the capacity, so the full
    // extent is not known until the header has been read.
    void* headerView = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                                     sizeof(RingHeader));
    if (!headerView)
    {
        CloseHandle(mapping);
        return false;
    }

    RingHeader probe;
    memcpy(&probe, headerView, sizeof(probe));
    UnmapViewOfFile(headerView);

    if (probe.magic != kSceneMagic || probe.version != kSceneVersion ||
        probe.headerBytes != sizeof(RingHeader) || probe.capacityBytes == 0)
    {
        CloseHandle(mapping);
        return false;
    }

    const uint64_t total = sizeof(RingHeader) + probe.capacityBytes;
    void* view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, (SIZE_T)total);
    if (!view)
    {
        CloseHandle(mapping);
        return false;
    }

    m_mapping    = mapping;
    m_view       = view;
    m_header     = (RingHeader*)view;
    m_data       = (uint8_t*)view + sizeof(RingHeader);
    m_capacity   = probe.capacityBytes;
    m_isProducer = false;

    m_header->consumerPid   = GetCurrentProcessId();
    m_header->consumerAlive = 1;
    return true;
}

void SharedRing::Close()
{
    if (m_header)
    {
        if (m_isProducer) m_header->producerAlive = 0;
        else              m_header->consumerAlive = 0;
    }
    if (m_view)    UnmapViewOfFile(m_view);
    if (m_mapping) CloseHandle((HANDLE)m_mapping);

    m_mapping  = nullptr;
    m_view     = nullptr;
    m_header   = nullptr;
    m_data     = nullptr;
    m_capacity = 0;
}

uint64_t SharedRing::FreeBytes() const
{
    const uint64_t w = LoadAcquire64(&m_header->writeCursor);
    const uint64_t r = LoadAcquire64(&m_header->readCursor);
    return m_capacity - (w - r);
}

uint64_t SharedRing::PendingBytes() const
{
    if (!m_header) return 0;
    const uint64_t w = LoadAcquire64(&m_header->writeCursor);
    const uint64_t r = LoadAcquire64(&m_header->readCursor);
    return w - r;
}

void SharedRing::CopyIn(uint64_t cursor, const void* src, uint32_t bytes)
{
    const uint64_t offset = cursor % m_capacity;
    const uint64_t firstSpan = m_capacity - offset;
    if (bytes <= firstSpan)
    {
        memcpy(m_data + offset, src, bytes);
    }
    else
    {
        // Straddles the end of the buffer; copy in two spans.
        memcpy(m_data + offset, src, (size_t)firstSpan);
        memcpy(m_data, (const uint8_t*)src + firstSpan,
               (size_t)(bytes - firstSpan));
    }
}

void SharedRing::CopyOut(uint64_t cursor, void* dst, uint32_t bytes) const
{
    const uint64_t offset = cursor % m_capacity;
    const uint64_t firstSpan = m_capacity - offset;
    if (bytes <= firstSpan)
    {
        memcpy(dst, m_data + offset, bytes);
    }
    else
    {
        memcpy(dst, m_data + offset, (size_t)firstSpan);
        memcpy((uint8_t*)dst + firstSpan, m_data, (size_t)(bytes - firstSpan));
    }
}

bool SharedRing::TryWrite(MessageType type, const void* fixedPart,
                          uint32_t fixedBytes, const void* payload,
                          uint32_t payloadBytes, bool droppable)
{
    if (!m_header || !m_isProducer) return false;

    const uint32_t body  = fixedBytes + payloadBytes;
    const uint32_t total = AlignMessage((uint32_t)sizeof(MessageHeader) + body);

    // A message larger than the whole ring can never be written; report it
    // rather than spinning forever on a request that cannot be satisfied.
    if ((uint64_t)total > m_capacity)
    {
        Increment64(&m_header->bytesDropped, total);
        return false;
    }

    if ((uint64_t)total > FreeBytes())
    {
        Increment64(&m_header->bytesDropped, total);
        if (droppable) Increment64(&m_header->framesDropped, 1);
        return false;
    }

    MessageHeader mh;
    mh.type       = (uint32_t)type;
    mh.byteLength = total;

    uint64_t cursor = LoadAcquire64(&m_header->writeCursor);
    CopyIn(cursor, &mh, (uint32_t)sizeof(mh));
    cursor += sizeof(mh);

    if (fixedPart && fixedBytes)
    {
        CopyIn(cursor, fixedPart, fixedBytes);
        cursor += fixedBytes;
    }
    if (payload && payloadBytes)
    {
        CopyIn(cursor, payload, payloadBytes);
        cursor += payloadBytes;
    }

    // Publish only after the whole record is in place, so the consumer can
    // never observe a partially written message.
    const uint64_t start = LoadAcquire64(&m_header->writeCursor);
    StoreRelease64(&m_header->writeCursor, start + total);

    if (type == kMsgFrameEnd) Increment64(&m_header->framesProduced, 1);
    return true;
}

bool SharedRing::TryRead(void* buffer, uint32_t bufferBytes,
                         uint32_t* outMessageBytes, uint32_t* outNeededBytes)
{
    if (outMessageBytes) *outMessageBytes = 0;
    if (outNeededBytes)  *outNeededBytes  = 0;
    if (!m_header || m_isProducer) return false;

    const uint64_t w = LoadAcquire64(&m_header->writeCursor);
    const uint64_t r = LoadAcquire64(&m_header->readCursor);
    if (w - r < sizeof(MessageHeader)) return false;

    MessageHeader mh;
    CopyOut(r, &mh, (uint32_t)sizeof(mh));

    // Defend against a corrupt or truncated record rather than trusting a
    // length that came from another process.
    if (mh.byteLength < sizeof(MessageHeader) ||
        (uint64_t)mh.byteLength > m_capacity ||
        (uint64_t)mh.byteLength > (w - r))
    {
        return false;
    }

    if (mh.byteLength > bufferBytes)
    {
        if (outNeededBytes) *outNeededBytes = mh.byteLength;
        return false;   // nothing consumed; caller can grow and retry
    }

    CopyOut(r, buffer, mh.byteLength);
    StoreRelease64(&m_header->readCursor, r + mh.byteLength);

    if (outMessageBytes) *outMessageBytes = mh.byteLength;
    return true;
}



void SharedRing::RequestResend(const uint64_t* ids, uint32_t count)
{
    if (!m_header || !ids) return;
    if (count > kMaxResendRequests) count = kMaxResendRequests;

    for (uint32_t i = 0; i < count; ++i) m_header->resendIds[i] = ids[i];

    // Published last, with a release, so the producer cannot read a slot that
    // has not been written yet.
    ExchangeRelease32(&m_header->resendCount, count);
}

uint32_t SharedRing::TakeResendRequests(uint64_t* out, uint32_t max)
{
    if (!m_header || !out) return 0;

    // Taking the count and clearing it in one operation means a request the
    // consumer writes while this runs is either seen now or seen next frame,
    // never dropped silently.
    uint32_t count = ExchangeRelease32(&m_header->resendCount, 0);
    if (count > kMaxResendRequests) count = kMaxResendRequests;
    if (count > max) count = max;

    for (uint32_t i = 0; i < count; ++i) out[i] = m_header->resendIds[i];
    return count;
}
} // namespace SceneIPC
