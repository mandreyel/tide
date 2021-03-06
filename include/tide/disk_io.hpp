#ifndef TIDE_DISK_IO_HEADER
#define TIDE_DISK_IO_HEADER

#include "average_counter.hpp"
#include "bdecode.hpp"
#include "bitfield.hpp"
#include "block_cache.hpp"
#include "block_source.hpp"
#include "disk_buffer.hpp"
#include "disk_io_error.hpp"
#include "exponential_backoff.hpp"
#include "interval.hpp"
#include "log.hpp"
#include "path.hpp"
#include "sha1_hasher.hpp"
#include "string_view.hpp"
#include "thread_pool.hpp"
#include "time.hpp"
#include "torrent_storage.hpp"
#include "torrent_storage_handle.hpp"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <system_error>
#include <utility> // pair
#include <vector>

#include <asio/io_context.hpp>

namespace tide {

class metainfo;
class bmap_encoder;
struct torrent_info;
struct disk_io_settings;

/**
 * All operations run asynchronously.
 * TODO more commentz
 */
class disk_io
{
public:
    struct stats
    {
        int num_blocks_written = 0;
        int num_blocks_read = 0;

        int num_read_cache_hits = 0;
        int num_read_cache_misses = 0;

        int read_cache_capacity = 0;
        int read_cache_size = 0;

        // int write_queue_size = 0;
        // int read_queue_size = 0;
        // int peak_write_queue_size = 0;
        // int peak_read_queue_size = 0;

        // How many of each jobs are currently being performed by threads.
        // TODO since we merge hashing and writing in some cases, this would not be
        // representative of the number of working threads
        // int num_hashing_threads = 0;
        // int num_writing_threads = 0;
        // int num_reading_threads = 0;

        // int num_threads = 0;

        // The number of in-progress pieces and blocks buffered in disk_io.
        int num_partial_pieces = 0;
        int num_buffered_blocks = 0;

        // The average number of milliseconds a job is queued up (is waiting to be
        // executed).
        // milliseconds avg_wait_time{0};
        // milliseconds avg_write_time{0};
        // milliseconds avg_read_time{0};
        // milliseconds avg_hash_time{0};

        // milliseconds total_job_time{0};
        // milliseconds total_write_time{0};
        // milliseconds total_read_time{0};
        // milliseconds total_hash_time{0};
    };

private:
    // This is the io_context that runs the network thread. It is used to post handlers
    // on the network thread so that no syncing is required between the two threads, as
    // io_context is thread-safe.
    asio::io_context& network_ios_;

    const disk_io_settings& settings_;

    // All disk jobs are posted to and executed by this thread pool. Note that anything
    // posted to this that accesses fields in disk_io will need to partake in mutual
    // exclusion.
    thread_pool thread_pool_;

    // Before we attempt to read in blocks from disk we first check whether it's not
    // already in cache. Read in blocks are always placed in the cache. Cache is only
    // ever accessed from the network thread.
    block_cache read_cache_;

    // Only disk_io can instantiate disk_buffers so that instances can be reused. All
    // buffers made by pool are 16KiB in size.
    disk_buffer_pool disk_buffer_pool_;

    /**
     * This class represents an in-progress piece. It is used to store the hash context
     * (blocks are incrementally hashed) and to buffer blocks so that they may be
     * processed (hashed and written to disk) in batches.
     *
     * For optimal performance, blocks should be supplied in contiguous batches (need
     * not be in order within the batch) of settings::write_cache_line_size, following
     * the last such batch, so that they may be hashed and written to disk by a single
     * thread. However, this is the optimum, otherwise  at most
     * write_buffer_capacity blocks are kept in memory, after which they are flushed to
     * disk, hashed or not. This means that most of them (unless some follow the last
     * hashed block) won't be hashed and thus need to be pulled back for hashing later,
     * when the missing blocks have been downloaded.
     *
     * Crucially, only a single thread may work on a piece at any given time.
     *
     * If an error occurs while trying to write to disk, i.e. blocks could not be saved,
     * they are placed back into piece's buffer for future reattempt.
     *
     * Once the piece is completed, its hashing is finished, and the resulting hash is
     * compared to the piece's expected hash, and this result is passed along to
     * completion_handler. If the hash test was passed, the remaining blocks are written
     * to disk. (If the piece is large, that is, it has to be written to disk in several
     * settings::write_cache_line_size chunks, it means that even if a piece turns out
     * to be bad, some blocks will inevitably be persisted to disk. This is of no concern
     * as they will be overwritten once valid data is received.)
     *
     * Note, however, that while a thread is hashing and saving the current write buffer,
     * the network thread might fill up blocks with another batch ready to be hashed,
     * but since unhashed_offset is only updated once the hasher thread is finished, the
     * network thread will not initiate another hash job (since it assumes that there is
     * a gap in blocks, due to the mismatch between blocks[0].offset and unhashed_offset,
     * (which is the desired behaviour)).
     * Therefore, once the current hasher finishes, it must check whether another batch
     * needs hashing.
     TODO decide if you want this or we should introduce atomic variables for this
     */
    struct partial_piece
    {
        struct block
        {
            // The actual data. This must always be valid.
            disk_buffer buffer;

            // The offset (a multiple of 16KiB) where this block starts in piece.
            int offset;

            // Invoked once the block could be saved to disk.
            std::function<void(const std::error_code&)> save_handler;

            block(disk_buffer buffer_, int offset_,
                    std::function<void(const std::error_code&)> save_handler_);
        };

        // `buffer` is used to buffer blocks so that they may be written to disk
        // in batches. The batch size varies, for in the optimal case we try to
        // wait for `disk_io_settings::write_cache_line_size` contiguous, or even
        // better, hashable (which means contiguous and follows the last hashed
        // block) blocks, but if this is not fulfilled, blocks are buffered
        // until the buffer size reaches `disk_io_settings::write_buffer_capacity`
        // blocks, after which the entire buffer is flushed.
        // `work_buffer` is used to hold blocks that are being processed by
        // a worker thread.
        //
        // In the case of disk errors, blocks that we couldn't save are put back
        // into buffer and kept there until successfully saved to disk.
        //
        // Blocks in both `buffer` and `work_buffer` are ordered by their
        // offsets.
        //
        // To avoid race conditions and mutual exclusion, network thread only
        // handles buffer, and when we want to flush its blocks, before
        // launching the async operation (i.e. still on the network thread) the
        // to-be-flushed blocks are transferred to `work_buffer`. This may only be
        // done while `is_busy` is not set, because the worker thread does access
        // `work_buffer`.
        std::vector<block> buffer;
        std::vector<block> work_buffer;

        // Blocks may be saved to disk without being hashed, so `unhashed_offset`
        // is not sufficient to determine how many blocks we have. Thus each
        // block that was saved is marked as `true`. The vector is preallocated
        // to the number of blocks.
        //
        // Only handled on the network thread.
        std::vector<bool> save_progress;

        // Only one thread may process a `partial_piece` at a time, so this is set
        // before such a thread is launched, and unset on the network thread as
        // well, once thread calls the completion handler (i.e. it need not be
        // atomic since it's only accessed from the network thread).
        //
        // Only handled on the network thread.
        bool is_busy = false;

        // A cached value of the number of true bits in `blocks_saved_`.
        int num_saved_blocks = 0;

        // This is always the first byte of the first unhashed block (that is,
        // one past the last hashed block's last byte). It's used to check if we
        // can hash blocks since they must be passed to `hasher_.update()` in
        // order.
        //
        // It's handled on both network and worker threads, but never at the
        // same time.
        // This is done by synchronizing using the `is_busy` field, i.e. if piece
        // is busy, we don't bother this field (and piece in general), so while
        // this flag is set worker thread may freely update this field without
        // further syncing.
        int unhashed_offset = 0;

        const piece_index_t index;
        // The length of this piece in bytes.
        const int length;

        // This is invoked once the piece has been hashed, which means that the
        // piece may not have been written to disk by the time of the
        // invocation.
        //
        // Only used by the network thread.
        std::function<void(bool)> completion_handler;

        // This contains the `sha1_context` that holds the current hashing
        // progress and is used to incrementally hash blocks.
        //
        // NOTE: blocks may only be hashed in order.
        //
        // Only used by the worker threads.
        sha1_hasher hasher;

        // This enforces an upper bound on how long blocks may stay in memory.
        // This is to avoid lingering blocks, which may occur if the client
        // started downloading a piece from the only peer that has it, then
        // disconnected.
        deadline_timer buffer_expiry_timer;

        /**
         * Initializes the const fields, `buffer_expiry_timer` and calculates
         * `num_blocks`.
         */
        partial_piece(piece_index_t index_, int length_, int max_write_buffer_size,
                std::function<void(bool)> completion_handler, asio::io_context& ios);

        /**
         * Determines whether all blocks have been received, regardless if they
         * are already hashed and saved to disk or are still in blocks buffer.
         */
        bool is_complete() const noexcept;

        /** The total number of blocks in piece (i.e. not just the ones we have). */
        int num_blocks() const noexcept;

        /**
         * We can only hash the blocks that are in order and have no gaps. If
         * there is no gap, it returns a pair of indices denoting the range in
         * buffer that constitutes the hashable blocks.
         */
        interval hashable_range() const noexcept;

        /**
         * Returns a left-inclusive interval that represents the range of the
         * largest contiguous block sequence within buffer.
         */
        interval largest_contiguous_range() const noexcept;

        /**
         * This is used when, after extracting blocks from buffer to work_buffer, we
         * fail to save them to disk, in which case we need to put them back in buffer
         * so that they may be saved later. The reason blocks are put back from
         * write_buffer to buffer is because it simplifies working with work_buffer and
         * even though it's sligthly expensive to do this, we don't expect to need this
         * frequently, and when we do, we have bigger problems.
         */
        void restore_buffer();
    };

    struct torrent_entry
    {
        const torrent_id_t id;

        // Each torrent is associated with a `torrent_storage` instance which
        // encapsulates the implementation of instantiating the storage, saving
        // and loading blocks from disk, renaming/moving/deleting files etc.
        // Higher level logic, like buffering writes or executing these
        // functions concurrently is done in `disk_io`, as `torrent_storage` only
        // takes care of the low level functions.
        torrent_storage storage;

        // Received blocks are not immediately written to disk, but are buffered
        // in this list until the number of blocks reach
        // disk_io_settings::write_cache_line_size or the piece is finished,
        // after which the blocks are hashed and written to disk. This defers
        // write jobs as much as possible so as to batch them together to
        // increase the amount of work performed within a context switch.
        //
        // `unique_ptr` is used to ensure that a thread referring to a piece does
        // not end up accessing invalid memory when write_buffer is reallocated
        // upon adding new entries from the network thread.
        std::vector<std::unique_ptr<partial_piece>> write_buffer;

        struct fetch_subscriber
        {
            std::function<void(const std::error_code&, block_source)> handler;
            int requested_offset;
        };

        // Each time a block fetch is issued, which usually pulls in more blocks
        // or, if the piece is not too large, the entire piece, it is registered
        // here, so that if other requests for the same block or for any of the
        // blocks that are pulled in with the first one (if read ahead is not
        // disabled), which is common when a peer sends us a request queue, they
        // don't launch their own fetch ops, but instead wait for the first
        // operation to finish and notify them of their block. The
        // `fetch_subscriber` list has to be ordered by the requested offset.
        //
        // After the operation is finished and all waiting requests are served,
        // the entry is removed from this map.
        //
        // Thus only the first block fetch request is recorded here, the rest
        // are attached to the subscriber queue.
        //
        // The original request handler is not stored in the subscriber list (so
        // if only a single request is issued for this block, we don't have to
        // allocate torrent).
        std::vector<std::pair<block_info, std::vector<fetch_subscriber>>> block_fetches;

        // Every time a thread is launched to do some operation on
        // `torrent_entry`, this counter is incremented, and when the operation
        // finished, it's decreased. It is used to keep `torrent_entry` alive
        // until the last async operation.
        std::atomic<int> num_pending_ops{0};

        torrent_entry(const torrent_info& info, string_view piece_hashes,
                path resume_data_path);

        bool is_block_valid(const block_info& block);
    };

    // All torrents in engine have a corresponding torrent_entry. Entries are sorted
    // in ascending order of torrent_entry::id.
    std::vector<std::unique_ptr<torrent_entry>> torrents_;

    // Statistics are gathered here. One copy persists throughout the entire application
    // and copies for other modules are made on demand.
    stats stats_;

    // When we encounter a fatal disk error, we keep retrying. This timer is used to
    // schedule retries.
    // TODO it's not implemented
    deadline_timer retry_timer_;

    // This is used to calculate how much time to wait between retries. We wait at most
    // 120 seconds.
    exponential_backoff<120> retry_delay_;

public:
    disk_io(asio::io_context& network_ios, const disk_io_settings& settings);
    ~disk_io();

    int num_buffered_pieces();
    int num_buffered_blocks();
    int num_buffered_blocks(const torrent_id_t id);

    void set_read_cache_capacity(const int n);
    void set_concurrency(const int n);
    void set_resume_data_path(const path& path);

    void read_metainfo(const path& path,
            std::function<void(const std::error_code&, metainfo)> handler);

    /**
     * As opposed to most other operations, allocating a torrent is not done on
     * another thread as this operation only creates an internal torrent entry within
     * disk_io and it creates the directory tree for the torrent, the cost of which
     * should be little (TODO verify this claim). Files are only allocated once actual
     * data needs to be written to them.
     *
     * If the operation results in an error, error is set and an invalid
     * torrent_storage_handle is returned.
     */
    torrent_storage_handle allocate_torrent(
            const torrent_info& info, std::string piece_hashes, std::error_code& error);
    void move_torrent(const torrent_id_t id, std::string new_path,
            std::function<void(const std::error_code&)> handler);
    void rename_torrent(const torrent_id_t id, std::string name,
            std::function<void(const std::error_code&)> handler);

    /** Completely removes the torrent (files + metadata). */
    void erase_torrent_files(
            const torrent_id_t id, std::function<void(const std::error_code&)> handler);

    /**
     * Only erases the torrent's resume data, which is useful when user no longer wants
     * to seed it but wishes to retain the file.
     */
    void erase_torrent_resume_data(
            const torrent_id_t id, std::function<void(const std::error_code&)> handler);
    void save_torrent_resume_data(const torrent_id_t id, bmap_encoder resume_data,
            std::function<void(const std::error_code&)> handler);
    void load_torrent_resume_data(const torrent_id_t id,
            std::function<void(const std::error_code&, bmap)> handler);

    /**
     * Reads the state of every torrent whose state was saved to disk and returns a list
     * of all torrent states through the handler. This should be used when starting the
     * application.
     */
    void load_all_torrent_resume_data(
            std::function<void(const std::error_code&, std::vector<bmap>)> handler);

    /**
     * Verifies that all pieces downloaded in torrent exist and are valid by hashing
     * each piece in the downloaded files and comparing them to their expected values.
     */
    void check_storage_integrity(const torrent_id_t id, bitfield pieces,
            std::function<void(const std::error_code&, bitfield)> handler);

    /**
     * This can be used to hash any generic data, but for hashing pieces/blocks, use
     * save_block which incrementally hashes a piece with each additional block.
     * The lifetime of data must be ensured until the invocation of the handler.
     */
    void create_sha1_digest(
            const_view<uint8_t> data, std::function<void(sha1_hash)> handler);

    /**
     * This creates a page aligend disk buffer into which peer_session can receive or
     * copy blocks. This is necessary to save blocks (save_block takes a disk_buffer as
     * argument), as better performance can be achieved this way.
     * This method is always guaranteed to return a valid disk_buffer, but peer_session
     * must make sure that it doesn't abuse disk performance and its receive buffer
     * capacity which includes its outstanding bytes being written to disk.
     * TODO create a stronger constraint on this

     // TODO this is muddy explanation
     * disk_buffers have a fix size of 16KiB (0x4000), but the caller may request that
     * the size be less than that, in which case the true buffer size will remain the
     * same but the conceptual buffer size will the be requested number of bytes.
     */
    disk_buffer get_disk_buffer(const int length = 0x4000);

    /**
     * Asynchronously hashes and saves a block to disk. However, unless configured
     * otherwise, blocks are buffered until a suitable number of adjacent blocks have
     * been downloaded so as to process them in bulk. This means that for the best
     * performance, settings::write_cache_line_size number of adjacent blocks should
     * be downloaded in quick succession.
     *
     * If by adding this block the piece to which it belongs is completed, the hasher
     * finalizes the incremental hashing and produces a SHA-1 hash of the piece. Then,
     * this hash is compared to the expected hash, and the result is passed onto the
     * piece_completion_handler. A true value means the piece passed, while a false
     * value indicates a corrupt piece.
     * Then, if the piece passed the hash test, the remaining buffered blocks in this
     * piece are written to disk. If it didn't, then the save handler is invoked right
     * after the completion_handler, with disk_io_errc::drop_corrupt_piece_data. This
     * is not a disk error, just a way to wrap up the save operation so that any logic
     * tied to the invocation of the save handlers may be concluded.
     *
     * save_handler is always invoked after the save operation has finished.
     *
     * The piece_completion_handler is only stored once per piece, i.e. the handler
     * supplied with the first block in piece that was saved.
     */
    void save_block(const torrent_id_t id, const block_info& block_info,
            disk_buffer block_data,
            std::function<void(const std::error_code&)> save_handler,
            std::function<void(bool)> piece_completion_handler);

    /**
     * Requests are queued up and those that ask for pieces that are cached are served
     * first over those whose requested pieces need to be pulled in from disk.
     * If multiple peers request the same uncached piece, only the first will launch a
     * disk read operation, while the others will be queued up and notified when the
     * piece is available.
     */
    void fetch_block(const torrent_id_t id, const block_info& block_info,
            std::function<void(const std::error_code&, block_source)> handler);

private:
    // -------
    // writing
    // -------

    /**
     * Depending on the state of the piece, invokes handle_complete_piece or
     * flush_buffer, and takes care of setting up those operations.
     */
    void dispatch_write(torrent_entry& torrent, partial_piece& piece);

    /**
     * This is called when piece has been completed by the most recent block that was
     * issued to be saved to disk. First, it checks whether some blocks have been
     * written to disk without being hashed (this happens when we don't receive blocks
     * in order, write buffer becomes fragmented, eventually reaching capacity, after
     * which it needs to be flushed), and if there are any, it reads them back for
     * hashing. After all blocks have been hashed, it compares the hash result to the
     * expected value, then user is notified of the result, and if piece is good, the
     * remaining blocks in piece's write buffer are written to disk, after which the
     * save handler is invoked.
     */
    void handle_complete_piece(torrent_entry& torrent, partial_piece& piece);

    /**
     * Called by handle_complete_piece, hashes all unhashed blocks in piece, which means
     * it may need to read back some blocks from disk. blocks in piece.work_buffer need
     * not be contiguous.
     */
    sha1_hash finish_hashing(
            torrent_entry& torrent, partial_piece& piece, std::error_code& error);

    /**
     * This is called when piece has settings::write_cache_line_size or more hashable
     * blocks (which means contiguous and following the last hashed block in piece), in
     * which case these blocks are extracted from the piece's write buffer, hashed and
     * saved in one batch.
     */
    void hash_and_save_blocks(torrent_entry& torrent, partial_piece& piece);

    /**
     * This is called when piece is not yet complete (otherwise handle_complete_piece
     * is used) or when piece does not have hashable blocks (then hash_and_save_blocks
     * would be called), but piece has accrued so many blocks as to necessitate flushing
     * them to disk. Blocks don't have to be contiguous. If some of the blocks in the
     * beginning of blocks are hashable, they are hashed as well, to decrease the amount
     * needed to be read back (if all blocks are hashable, hash_and_save_blocks is used).
     */
    void flush_buffer(torrent_entry& torrent, partial_piece& piece);

    /**
     * Utility function that saves to disk the blocks in piece.work_buffer, which may
     * or may not be contiguous to disk.
     * The less fragmented the block sequence, the more efficient the operation.
     */
    void save_maybe_contiguous_blocks(
            torrent_entry& torrent, partial_piece& piece, std::error_code& error);

    /** The completion handler for hash_and_save_blocks and flush_buffer. */
    void on_blocks_saved(
            const std::error_code& error, torrent_entry& torrent, partial_piece& piece);

    /**
     * Saving blocks entails the same plumbing: preparing iovec buffers, the block_info
     * indicating where to save the blocks and calling storage's appropriate function.
     */
    void save_contiguous_blocks(torrent_storage& storage, const piece_index_t piece_index,
            view<partial_piece::block> blocks, std::error_code& error);

    /**
     * If a piece's buffer could not be flushed in time, it is flushed to avoid lingering
     * blocks in memory (see partial_piece::flush_timer comment).
     */
    void on_write_buffer_expiry(
            const std::error_code& error, torrent_entry& torrent, partial_piece& piece);

    // -------
    // reading
    // -------

    /**
     * Depending on the configuration and the number of blocks left in piece starting at
     * the requested block, we either read ahead or just read a single block.
     */
    void dispatch_read(torrent_entry& torrent, const block_info& info,
            std::function<void(const std::error_code&, block_source)> handler);

    void read_single_block(torrent_entry& torrent, const block_info& info,
            std::function<void(const std::error_code&, block_source)> handler);

    void read_ahead(torrent_entry& torrent, const block_info& block_info,
            std::function<void(const std::error_code&, block_source)> handler);

    block_info make_mmap_read_ahead_info(
            torrent_entry& torrent, const block_info& first_block) const noexcept;

    void on_blocks_read_ahead(torrent_entry& torrent, std::vector<block_source> blocks,
            std::function<void(const std::error_code&, block_source)> handler);

    // -----
    // utils
    // -----

    /**
     * Creates a vector of iovecs and counts the total number of bytes in blocks,
     * and returns buffers and the number of bytes as a pair.
     */
    std::pair<std::vector<iovec>, int> prepare_iovec_buffers(
            view<partial_piece::block> blocks);

    /** Counts the number of blocks that follow the first block in blocks. */
    static int count_contiguous_blocks(const_view<partial_piece::block> blocks) noexcept;

    /** id must be valid, otherwise an assertion will fail. */
    torrent_entry& find_torrent_entry(const torrent_id_t id);

    enum class log_event
    {
        info,
        cache,
        metainfo,
        torrent,
        write,
        read,
        resume_data,
        integrity_check
    };

    enum class invoked_on
    {
        network_thread,
        thread_pool
    };

    template <typename... Args>
    void log(const log_event event, const char* format, Args&&... args) const;
    template <typename... Args>
    void log(const log_event event, const log::priority priority, const char* format,
            Args&&... args) const;
    template <typename... Args>
    void log(const invoked_on thread, const log_event event, const char* format,
            Args&&... args) const;
    template <typename... Args>
    void log(const invoked_on thread, const log_event event, const log::priority priority,
            const char* format, Args&&... args) const;
};

} // namespace tide

#endif // TIDE_DISK_IO_HEADER
