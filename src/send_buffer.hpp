#ifndef TORRENT_SEND_BUFFER_HEADER
#define TORRENT_SEND_BUFFER_HEADER

#include "block_disk_buffer.hpp"
#include "mmap/mmap.hpp"
#include "payload.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>
#include <array>
#include <deque>

#include <asio/buffer.hpp>

/**
 * This class is used for accruing messages until it is drained and sent off to socket.
 *
 * It can take two types of data: raw and disk buffers. The former is some contiguous
 * byte sequence. This should be used for simple, short messages. The latter is what's
 * used for sending large piece blocks; these are views into memory mapped file buffers.
 * 
 * It takes ownership of the raw messages, and holds onto the disk buffers as well until
 * the consume() function is invoked, after which all resources that are confirmed to be
 * sent, are released.
 *
 * The output is a sequence of asio buffers that satisfies the ConstBufferSequence
 * concept.
 *
 * Memory allocation for the resources is dynamic, and currently no upper bound is
 * enforced on the buffer size (TODO).
 */
class send_buffer
{
    struct buffer_holder
    {
        virtual ~buffer_holder() = default;
        virtual const uint8_t* data() const noexcept = 0;
        virtual int size() const noexcept = 0;
    };

    struct raw_buffer_holder : public buffer_holder
    {
        std::vector<uint8_t> bytes;

        raw_buffer_holder(std::vector<uint8_t>&& b) : bytes(std::move(b)) {}
        const uint8_t* data() const noexcept override { return bytes.data(); }
        int size() const noexcept override { return bytes.size(); }
    };

    template<size_t N> struct raw_fixed_buffer_holder : public buffer_holder
    {
        std::array<uint8_t, N> bytes;

        raw_fixed_buffer_holder(const std::array<uint8_t, N>& b) : bytes(b) {}
        raw_fixed_buffer_holder(const uint8_t (&b)[N])
        {
            std::copy(b, b + N, bytes.data());
        }

        const uint8_t* data() const noexcept override { return bytes.data(); }
        int size() const noexcept override { return bytes.size(); }
    };

    struct disk_buffer_holder : public buffer_holder
    {
        mmap_source bytes;

        disk_buffer_holder(mmap_source b) : bytes(std::move(b)) {}
        const uint8_t* data() const noexcept override { return bytes.data(); }
        int size() const noexcept override { return bytes.size(); }
    };

    // These are the bytes we want to send off to socket. It ensures their lifetime
    // until they are confirmed to be sent, after which the resources are released.
    std::deque<std::unique_ptr<buffer_holder>> m_buffers;

    // This is the offset into the first buffer in m_buffers that marks the beginning of
    // unsent bytes. This is employed because it may be that not all of the buffer is
    // drained during a send operation, and if so, it is very likely that the number of
    // sent bytes will not align with buffer boundaries, leaving the first buffer with
    // sent and unsent fractions. Thus, this buffer must be kept alive until all its
    // unsent bytes have been sent off.
    int m_first_unsent_byte = 0;

    // The total number of UNSENT bytes we have in buffer. That is, if the first buffer
    // was not fully drained (m_first_unsent_byte > 0), it will have excess bytes, which
    // are not counted (since it's a temporary state and is not relevant to the caller).
    int m_size = 0;

public:

    bool is_empty() const noexcept;
    int size() const noexcept;

    void append(payload payload);
    void append(const std::vector<uint8_t>& bytes);
    template<size_t N> void append(const fixed_payload<N>& payload);
    template<size_t N> void append(const std::array<uint8_t, N>& bytes);
    template<size_t N> void append(const uint8_t (&bytes)[N]);
    void append(const block_source& block);

    /**
     * Returns an asio ConstBufferSequence compliant list of buffers whose total size is
     * at most num_bytes (less if there aren't that many bytes available for sending).
     */
    // TODO find a way to not have to build a vector every time we send something
    // maybe somehow return a view?
    std::vector<asio::const_buffer> get_send_buffers(int num_bytes) const;

    /**
     * Must be called after send_buffer has been drained (sent to socket), so that
     * resources may be cleaned up and the unsent message cursor adjusted.
     */
    void consume(int num_sent_bytes);
};

inline bool send_buffer::is_empty() const noexcept
{
    return size() == 0;
}

inline int send_buffer::size() const noexcept
{
    return m_size;
}

inline void send_buffer::append(payload payload)
{
    append(std::move(payload.data));
}

template<size_t N>
void send_buffer::append(const fixed_payload<N>& payload)
{
    append(payload.data);
}

template<size_t N>
void send_buffer::append(const std::array<uint8_t, N>& bytes)
{
    static_assert(N > 0, "buffer must not be empty");
    m_buffers.emplace_back(std::make_unique<raw_fixed_buffer_holder<N>>(bytes));
    m_size += N;
}

template<size_t N>
void send_buffer::append(const uint8_t (&bytes)[N])
{
    static_assert(N > 0, "buffer must not be empty");
    m_buffers.emplace_back(std::make_unique<raw_fixed_buffer_holder<N>>(bytes));
    m_size += N;
}

#endif // TORRENT_SEND_BUFFER_HEADER
