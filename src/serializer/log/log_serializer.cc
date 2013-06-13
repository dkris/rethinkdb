// Copyright 2010-2013 RethinkDB, all rights reserved.
#include "serializer/log/log_serializer.hpp"

#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "arch/io/disk.hpp"
#include "arch/runtime/runtime.hpp"
#include "buffer_cache/types.hpp"
#include "perfmon/perfmon.hpp"

filepath_file_opener_t::filepath_file_opener_t(const serializer_filepath_t &filepath, io_backender_t *backender)
    : filepath_(filepath), backender_(backender), opened_temporary_(false) { }

filepath_file_opener_t::~filepath_file_opener_t() { }

std::string filepath_file_opener_t::file_name() const {
    return filepath_.permanent_path();
}

std::string filepath_file_opener_t::temporary_file_name() const {
    return filepath_.temporary_path();
}

std::string filepath_file_opener_t::current_file_name() const {
    return opened_temporary_ ? temporary_file_name() : file_name();
}

void filepath_file_opener_t::open_serializer_file(const std::string &path, int extra_flags, scoped_ptr_t<file_t> *file_out) {
    const file_open_result_t res = open_direct_file(path.c_str(),
                                                    linux_file_t::mode_read | linux_file_t::mode_write | extra_flags,
                                                    backender_,
                                                    file_out);
    if (res.outcome == file_open_result_t::ERROR) {
        crash_due_to_inaccessible_database_file(path.c_str(), res);
    }

    if (res.outcome == file_open_result_t::BUFFERED) {
        logWRN("Could not turn off filesystem caching for database file: \"%s\" "
               "(Is the file located on a filesystem that doesn't support direct I/O "
               "(e.g. some encrypted or journaled file systems)?) "
               "This can cause performance problems.",
               path.c_str());
    }
}

void filepath_file_opener_t::open_serializer_file_create_temporary(scoped_ptr_t<file_t> *file_out) {
    mutex_assertion_t::acq_t acq(&reentrance_mutex_);
    open_serializer_file(temporary_file_name(), linux_file_t::mode_create | linux_file_t::mode_truncate, file_out);
    opened_temporary_ = true;
}

void filepath_file_opener_t::move_serializer_file_to_permanent_location() {
    // TODO: Make caller not require that this not block, run ::rename in a blocker pool.
    ASSERT_NO_CORO_WAITING;

    mutex_assertion_t::acq_t acq(&reentrance_mutex_);

    guarantee(opened_temporary_);
    const int res = ::rename(temporary_file_name().c_str(), file_name().c_str());

    if (res != 0) {
        crash("Could not rename database file %s to permanent location %s\n",
              temporary_file_name().c_str(), file_name().c_str());
    }

    opened_temporary_ = false;
}

void filepath_file_opener_t::open_serializer_file_existing(scoped_ptr_t<file_t> *file_out) {
    mutex_assertion_t::acq_t acq(&reentrance_mutex_);
    open_serializer_file(current_file_name(), 0, file_out);
}

void filepath_file_opener_t::unlink_serializer_file() {
    // TODO: Make caller not require that this not block, run ::unlink in a blocker pool.
    ASSERT_NO_CORO_WAITING;

    mutex_assertion_t::acq_t acq(&reentrance_mutex_);
    guarantee(opened_temporary_);
    const int res = ::unlink(current_file_name().c_str());
    guarantee_err(res == 0, "unlink() failed");
}

#ifdef SEMANTIC_SERIALIZER_CHECK
void filepath_file_opener_t::open_semantic_checking_file(int *fd_out) {
    const std::string semantic_filepath = filepath_.permanent_path() + "_semantic";
    int semantic_fd;
    do {
        semantic_fd = open(semantic_filepath.c_str(),
                           O_RDWR | O_CREAT, S_IRWXU | S_IRWXG | S_IRWXO);
    } while (semantic_fd == -1 && errno == EINTR);

    if (semantic_fd == INVALID_FD) {
        fail_due_to_user_error("Inaccessible semantic checking file: \"%s\": %s", semantic_filepath.c_str(), errno_string(errno).c_str());
    } else {
        *fd_out = semantic_fd;
    }
}
#endif  // SEMANTIC_SERIALIZER_CHECK



log_serializer_stats_t::log_serializer_stats_t(perfmon_collection_t *parent)
    : serializer_collection(),
      pm_serializer_block_reads(secs_to_ticks(1)),
      pm_serializer_index_reads(),
      pm_serializer_block_writes(),
      pm_serializer_index_writes(secs_to_ticks(1)),
      pm_serializer_index_writes_size(secs_to_ticks(1), false),
      pm_extents_in_use(),
      pm_bytes_in_use(),
      pm_serializer_lba_extents(),
      pm_serializer_data_extents(),
      pm_serializer_data_extents_allocated(),
      pm_serializer_data_extents_reclaimed(),
      pm_serializer_data_extents_gced(),
      pm_serializer_data_blocks_written(),
      pm_serializer_old_garbage_blocks(),
      pm_serializer_old_total_blocks(),
      pm_serializer_lba_gcs(),
      parent_collection_membership(parent, &serializer_collection, "serializer"),
      stats_membership(&serializer_collection,
          &pm_serializer_block_reads, "serializer_block_reads",
          &pm_serializer_index_reads, "serializer_index_reads",
          &pm_serializer_block_writes, "serializer_block_writes",
          &pm_serializer_index_writes, "serializer_index_writes",
          &pm_serializer_index_writes_size, "serializer_index_writes_size",
          &pm_extents_in_use, "serializer_extents_in_use",
          &pm_bytes_in_use, "serializer_bytes_in_use",
          &pm_serializer_lba_extents, "serializer_lba_extents",
          &pm_serializer_data_extents, "serializer_data_extents",
          &pm_serializer_data_extents_allocated, "serializer_data_extents_allocated",
          &pm_serializer_data_extents_reclaimed, "serializer_data_extents_reclaimed",
          &pm_serializer_data_extents_gced, "serializer_data_extents_gced",
          &pm_serializer_data_blocks_written, "serializer_data_blocks_written",
          &pm_serializer_old_garbage_blocks, "serializer_old_garbage_blocks",
          &pm_serializer_old_total_blocks, "serializer_old_total_blocks",
          &pm_serializer_lba_gcs, "serializer_lba_gcs",
          NULLPTR)
{ }

void log_serializer_t::create(serializer_file_opener_t *file_opener, static_config_t static_config) {
    log_serializer_on_disk_static_config_t *on_disk_config = &static_config;

    scoped_ptr_t<file_t> file;
    file_opener->open_serializer_file_create_temporary(&file);

    co_static_header_write(file.get(), on_disk_config, sizeof(*on_disk_config));

    metablock_t metablock;
    bzero(&metablock, sizeof(metablock));

    extent_manager_t::prepare_initial_metablock(&metablock.extent_manager_part);

    data_block_manager_t::prepare_initial_metablock(&metablock.data_block_manager_part);
    lba_list_t::prepare_initial_metablock(&metablock.lba_index_part);

    metablock.block_sequence_id = NULL_BLOCK_SEQUENCE_ID;

    mb_manager_t::create(file.get(), static_config.extent_size(), &metablock);
}

/* The process of starting up the serializer is handled by the ls_start_*_fsm_t. This is not
necessary, because there is only ever one startup process for each serializer; the serializer could
handle its own startup process. It is done this way to make it clear which parts of the serializer
are involved in startup and which parts are not. */

struct ls_start_existing_fsm_t :
    public static_header_read_callback_t,
    public mb_manager_t::metablock_read_callback_t,
    public lba_list_t::ready_callback_t
{
    explicit ls_start_existing_fsm_t(log_serializer_t *serializer)
        : ser(serializer), start_existing_state(state_start) {
    }

    ~ls_start_existing_fsm_t() {
    }

    bool run(cond_t *to_signal, serializer_file_opener_t *file_opener) {
        // STATE A
        rassert(start_existing_state == state_start);
        rassert(ser->state == log_serializer_t::state_unstarted);
        ser->state = log_serializer_t::state_starting_up;

        scoped_ptr_t<file_t> dbfile;
        file_opener->open_serializer_file_existing(&dbfile);
        ser->dbfile = dbfile.release();

        start_existing_state = state_read_static_header;
        // STATE A above implies STATE B here
        to_signal_when_done = NULL;
        if (next_starting_up_step()) {
            return true;
        } else {
            to_signal_when_done = to_signal;
            return false;
        }
    }

    bool next_starting_up_step() {
        if (start_existing_state == state_read_static_header) {
            // STATE B
            // TODO: static_header_read now always returns false.
            if (static_header_read(ser->dbfile,
                    &ser->static_config,
                    sizeof(log_serializer_on_disk_static_config_t),
                    this)) {
                crash("static_header_read always returns false");
                // start_existing_state = state_find_metablock;
            } else {
                start_existing_state = state_waiting_for_static_header;
                // STATE B above implies STATE C here
                return false;
            }
        }

        rassert(start_existing_state != state_waiting_for_static_header);

        if (start_existing_state == state_find_metablock) {
            // STATE D
            ser->extent_manager = new extent_manager_t(ser->dbfile, &ser->static_config,
                                                       ser->stats.get());
            {
                // We never end up releasing the static header extent reference.  Nobody says we
                // have to.
                extent_reference_t extent_ref
                    = ser->extent_manager->reserve_extent(0);  // For static header.
                UNUSED int64_t extent = extent_ref.release();
            }

            ser->metablock_manager = new mb_manager_t(ser->extent_manager);
            ser->lba_index = new lba_list_t(ser->extent_manager);
            ser->data_block_manager = new data_block_manager_t(&ser->dynamic_config, ser->extent_manager, ser, &ser->static_config, ser->stats.get());

            // STATE E
            if (ser->metablock_manager->start_existing(ser->dbfile, &metablock_found, &metablock_buffer, this)) {
                crash("metablock_manager_t::start_existing always returns false");
                // start_existing_state = state_start_lba;
            } else {
                // STATE E
                start_existing_state = state_waiting_for_metablock;
                return false;
            }
        }

        if (start_existing_state == state_start_lba) {
            // STATE G
            guarantee(metablock_found, "Could not find any valid metablock.");

            ser->latest_block_sequence_id = metablock_buffer.block_sequence_id;

            // STATE H
            if (ser->lba_index->start_existing(ser->dbfile, &metablock_buffer.lba_index_part, this)) {
                start_existing_state = state_reconstruct;
                // STATE J
            } else {
                // STATE H
                start_existing_state = state_waiting_for_lba;
                // STATE I
                return false;
            }
        }

        if (start_existing_state == state_reconstruct) {
            ser->data_block_manager->start_reconstruct();
            for (block_id_t id = 0; id < ser->lba_index->end_block_id(); id++) {
                flagged_off64_t offset = ser->lba_index->get_block_offset(id);
                if (offset.has_value()) {
                    ser->data_block_manager->mark_live(offset.get_value());
                }
            }
            ser->data_block_manager->end_reconstruct();
            ser->data_block_manager->start_existing(ser->dbfile, &metablock_buffer.data_block_manager_part);

            ser->extent_manager->start_existing(&metablock_buffer.extent_manager_part);

            start_existing_state = state_finish;
        }

        if (start_existing_state == state_finish) {
            start_existing_state = state_done;
            rassert(ser->state == log_serializer_t::state_starting_up);
            ser->state = log_serializer_t::state_ready;

            if (to_signal_when_done) to_signal_when_done->pulse();

            delete this;
            return true;
        }

        unreachable("Invalid state %d.", start_existing_state);
    }

    void on_static_header_read() {
        rassert(start_existing_state == state_waiting_for_static_header);
        // STATE C
        start_existing_state = state_find_metablock;
        // STATE C above implies STATE D here
        next_starting_up_step();
    }

    void on_metablock_read() {
        rassert(start_existing_state == state_waiting_for_metablock);
        // state after F, state before G
        start_existing_state = state_start_lba;
        // STATE G
        next_starting_up_step();
    }

    void on_lba_ready() {
        rassert(start_existing_state == state_waiting_for_lba);
        start_existing_state = state_reconstruct;
        next_starting_up_step();
    }

    log_serializer_t *ser;
    cond_t *to_signal_when_done;

    enum state_t {
        state_start,
        state_read_static_header,
        state_waiting_for_static_header,
        state_find_metablock,
        state_waiting_for_metablock,
        state_start_lba,
        state_waiting_for_lba,
        state_reconstruct,
        state_finish,
        state_done
    } start_existing_state;

    bool metablock_found;
    log_serializer_t::metablock_t metablock_buffer;

private:
    DISABLE_COPYING(ls_start_existing_fsm_t);
};

log_serializer_t::log_serializer_t(dynamic_config_t _dynamic_config, serializer_file_opener_t *file_opener, perfmon_collection_t *_perfmon_collection)
    : stats(new log_serializer_stats_t(_perfmon_collection)),  // can block in a perfmon_collection_t::add call.
      disk_stats_collection(),
      disk_stats_membership(_perfmon_collection, &disk_stats_collection, "disk"),  // can block in a perfmon_collection_t::add call.
#ifndef NDEBUG
      expecting_no_more_tokens(false),
#endif
      dynamic_config(_dynamic_config),
      shutdown_callback(NULL),
      state(state_unstarted),
      dbfile(NULL),
      extent_manager(NULL),
      metablock_manager(NULL),
      lba_index(NULL),
      data_block_manager(NULL),
      last_write(NULL),
      active_write_count(0) {
    // STATE A
    /* This is because the serializer is not completely converted to coroutines yet. */
    ls_start_existing_fsm_t *s = new ls_start_existing_fsm_t(this);
    cond_t cond;
    if (!s->run(&cond, file_opener)) cond.wait();
}

log_serializer_t::~log_serializer_t() {
    assert_thread();
    cond_t cond;
    if (!shutdown(&cond)) cond.wait();

    rassert(state == state_unstarted || state == state_shut_down);
    rassert(last_write == NULL);
    rassert(active_write_count == 0);
}

void *log_serializer_t::malloc() {
    // TODO: we shouldn't use malloc_aligned here, we should use our
    // custom allocation system instead (and use corresponding
    // free). This is tough because serializer object may not be on
    // the same core as the cache that's using it, so we should expose
    // the malloc object in a different way.
    char *data = reinterpret_cast<char *>(malloc_aligned(static_config.block_size().ser_value(), DEVICE_BLOCK_SIZE));

    // Initialize the block sequence id...
    reinterpret_cast<ls_buf_data_t *>(data)->block_sequence_id = NULL_BLOCK_SEQUENCE_ID;

    data += sizeof(ls_buf_data_t);
    return reinterpret_cast<void *>(data);
}

// TODO: Make this parameter const void.
void *log_serializer_t::clone(void *_data) {
    // TODO: we shouldn't use malloc_aligned here, we should use our
    // custom allocation system instead (and use corresponding
    // free). This is tough because serializer object may not be on
    // the same core as the cache that's using it, so we should expose
    // the malloc object in a different way.
    char *data = reinterpret_cast<char*>(malloc_aligned(static_config.block_size().ser_value(), DEVICE_BLOCK_SIZE));
    memcpy(data, reinterpret_cast<char*>(_data) - sizeof(ls_buf_data_t), static_config.block_size().ser_value());
    data += sizeof(ls_buf_data_t);
    return reinterpret_cast<void *>(data);
}

void log_serializer_t::free(void *ptr) {
    char *data = reinterpret_cast<char *>(ptr);
    data -= sizeof(ls_buf_data_t);
    ::free(reinterpret_cast<void *>(data));
}

file_account_t *log_serializer_t::make_io_account(int priority, int outstanding_requests_limit) {
    assert_thread();
    rassert(dbfile);
    return new file_account_t(dbfile, priority, outstanding_requests_limit);
}

void log_serializer_t::block_read(const counted_t<ls_block_token_pointee_t>& token, void *buf, file_account_t *io_account) {
    assert_thread();
    struct : public cond_t, public iocallback_t {
        void on_io_complete() { pulse(); }
    } cb;
    block_read(token, buf, io_account, &cb);
    cb.wait();
}

// TODO(sam): block_read can call the callback before it returns. Is this acceptable?
void log_serializer_t::block_read(const counted_t<ls_block_token_pointee_t>& token, void *buf, file_account_t *io_account, iocallback_t *cb) {
    assert_thread();
    struct my_cb_t : public iocallback_t {
        void on_io_complete() {
            stats->pm_serializer_block_reads.end(&pm_time);
            if (cb) cb->on_io_complete();
            delete this;
        }
        my_cb_t(iocallback_t *_cb, const counted_t<ls_block_token_pointee_t>& _tok, log_serializer_stats_t *_stats) : cb(_cb), tok(_tok), stats(_stats) {}
        iocallback_t *cb;
        // tok is needed to ensure the block remains alive for appropriate period of time.
        counted_t<ls_block_token_pointee_t> tok;
        ticks_t pm_time;
        log_serializer_stats_t *stats;
    };

    my_cb_t *readcb = new my_cb_t(cb, token, stats.get());

    stats->pm_serializer_block_reads.begin(&readcb->pm_time);

    ls_block_token_pointee_t *ls_token = token.get();
    rassert(ls_token);
    assert_thread();
    rassert(state == state_ready);
    rassert(token_offsets.find(ls_token) != token_offsets.end());

    rassert(state == state_ready);

    std::map<ls_block_token_pointee_t *, int64_t>::const_iterator token_offsets_it = token_offsets.find(ls_token);
    rassert(token_offsets_it != token_offsets.end());

    const int64_t offset = token_offsets_it->second;
    data_block_manager->read(offset, buf, io_account, readcb);
}

// God this is such a hack.
#ifndef SEMANTIC_SERIALIZER_CHECK
counted_t<ls_block_token_pointee_t>
get_ls_block_token(const counted_t<ls_block_token_pointee_t> &tok) {
    return tok;
}
#else
counted_t<ls_block_token_pointee_t>
get_ls_block_token(const counted_t<scs_block_token_t<log_serializer_t> >& tok) {
    if (tok) {
        return tok->inner_token;
    } else {
        return counted_t<ls_block_token_pointee_t>();
    }
}
#endif  // SEMANTIC_SERIALIZER_CHECK


void log_serializer_t::index_write(const std::vector<index_write_op_t>& write_ops, file_account_t *io_account) {
    assert_thread();
    ticks_t pm_time;
    stats->pm_serializer_index_writes.begin(&pm_time);
    stats->pm_serializer_index_writes_size.record(write_ops.size());

    index_write_context_t context;
    index_write_prepare(&context, io_account);

    {
        // The in-memory index updates, at least due to the needs of
        // data_block_manager_t garbage collection, needs to be
        // atomic.
        ASSERT_NO_CORO_WAITING;

        for (std::vector<index_write_op_t>::const_iterator write_op_it = write_ops.begin();
             write_op_it != write_ops.end();
             ++write_op_it) {
            const index_write_op_t& op = *write_op_it;
            flagged_off64_t offset = lba_index->get_block_offset(op.block_id);

            if (op.token) {
                // Update the offset pointed to, and mark garbage/liveness as necessary.
                counted_t<ls_block_token_pointee_t> token
                    = get_ls_block_token(op.token.get());

                // Mark old offset as garbage
                if (offset.has_value()) {
                    data_block_manager->mark_garbage(offset.get_value(), &context.extent_txn);
                }

                // Write new token to index, or remove from index as appropriate.
                if (token.has()) {
                    ls_block_token_pointee_t *ls_token = token.get();
                    rassert(ls_token);
                    std::map<ls_block_token_pointee_t *, int64_t>::const_iterator to_it = token_offsets.find(ls_token);
                    rassert(to_it != token_offsets.end());
                    offset = flagged_off64_t::make(to_it->second);

                    /* mark the life */
                    data_block_manager->mark_live(offset.get_value());
                } else {
                    offset = flagged_off64_t::unused();
                }
            }

            repli_timestamp_t recency = op.recency ? op.recency.get()
                : lba_index->get_block_recency(op.block_id);

            lba_index->set_block_info(op.block_id, recency, offset, io_account, &context.extent_txn);
        }
    }

    index_write_finish(&context, io_account);

    stats->pm_serializer_index_writes.end(&pm_time);
}

void log_serializer_t::index_write_prepare(index_write_context_t *context, file_account_t *io_account) {
    assert_thread();
    active_write_count++;

    /* Start an extent manager transaction so we can allocate and release extents */
    extent_manager->begin_transaction(&context->extent_txn);

    /* Just to make sure that the LBA GC gets exercised */
    lba_index->consider_gc(io_account, &context->extent_txn);
}

void log_serializer_t::index_write_finish(index_write_context_t *context, file_account_t *io_account) {
    assert_thread();
    metablock_t mb_buffer;

    /* Sync the LBA */
    struct : public cond_t, public lba_list_t::sync_callback_t {
        void on_lba_sync() { pulse(); }
    } on_lba_sync;
    const bool offsets_were_written = lba_index->sync(io_account, &on_lba_sync);

    /* Prepare metablock now instead of in when we write it so that we will have the correct
    metablock information for this write even if another write starts before we finish writing
    our data and LBA. */
    prepare_metablock(&mb_buffer);

    /* Stop the extent manager transaction so another one can start, but don't commit it
    yet */
    extent_manager->end_transaction(&context->extent_txn);

    /* Get in line for the metablock manager */
    bool waiting_for_prev_write;
    cond_t on_prev_write_submitted_metablock;
    if (last_write) {
        last_write->next_metablock_write = &on_prev_write_submitted_metablock;
        waiting_for_prev_write = true;
    } else {
        waiting_for_prev_write = false;
    }
    last_write = context;

    if (!offsets_were_written) on_lba_sync.wait();
    if (waiting_for_prev_write) on_prev_write_submitted_metablock.wait();

    struct : public cond_t, public mb_manager_t::metablock_write_callback_t {
        void on_metablock_write() { pulse(); }
    } on_metablock_write;
    const bool done_with_metablock = metablock_manager->write_metablock(&mb_buffer, io_account, &on_metablock_write);

    /* If there was another transaction waiting for us to write our metablock so it could
    write its metablock, notify it now so it can write its metablock. */
    if (context->next_metablock_write) {
        context->next_metablock_write->pulse();
    } else {
        rassert(context == last_write);
        last_write = NULL;
    }

    if (!done_with_metablock) on_metablock_write.wait();

    active_write_count--;

    /* End the extent manager transaction so the extents can actually get reused. */
    extent_manager->commit_transaction(&context->extent_txn);

    //TODO I'm kind of unhappy that we're calling this from in here we should figure out better where to trigger gc
    consider_start_gc();

    // If we were in the process of shutting down and this is the
    // last transaction, shut ourselves down for good.
    if (state == log_serializer_t::state_shutting_down
        && shutdown_state == log_serializer_t::shutdown_waiting_on_serializer
        && last_write == NULL
        && active_write_count == 0) {

        next_shutdown_step();
    }
}

counted_t<ls_block_token_pointee_t>
log_serializer_t::generate_block_token(int64_t offset) {
    assert_thread();
    counted_t<ls_block_token_pointee_t> ret(new ls_block_token_pointee_t(this, offset));
    return ret;
}

counted_t<ls_block_token_pointee_t>
log_serializer_t::block_write(const void *buf, block_id_t block_id, file_account_t *io_account, iocallback_t *cb) {
    assert_thread();
    // TODO: Implement a duration sampler perfmon for this
    ++stats->pm_serializer_block_writes;

    const int64_t offset = data_block_manager->write(buf, block_id, true, io_account, cb, true);

    return generate_block_token(offset);
}

counted_t<ls_block_token_pointee_t>
log_serializer_t::block_write(const void *buf, block_id_t block_id, file_account_t *io_account) {
    assert_thread();
    rassert(block_id != NULL_BLOCK_ID, "If this assertion fails, inform Sam and remove the assertion.");
    return serializer_block_write(this, buf, block_id, io_account);
}


void log_serializer_t::register_block_token(ls_block_token_pointee_t *token, int64_t offset) {
    assert_thread();
    DEBUG_VAR std::pair<std::map<ls_block_token_pointee_t *, int64_t>::iterator, bool> insert_res
        = token_offsets.insert(std::make_pair(token, offset));
    rassert(insert_res.second);

    const bool first_token_for_offset = offset_tokens.find(offset) == offset_tokens.end();
    if (first_token_for_offset) {
        // Mark offset live in GC
        data_block_manager->mark_token_live(offset);
    }

    offset_tokens.insert(std::pair<int64_t, ls_block_token_pointee_t *>(offset, token));
}

bool log_serializer_t::tokens_exist_for_offset(int64_t off) {
    assert_thread();
    return offset_tokens.find(off) != offset_tokens.end();
}

void log_serializer_t::unregister_block_token(ls_block_token_pointee_t *token) {
    assert_thread();

    ASSERT_NO_CORO_WAITING;

    rassert(!expecting_no_more_tokens);
    std::map<ls_block_token_pointee_t *, int64_t>::iterator token_offset_it = token_offsets.find(token);
    rassert(token_offset_it != token_offsets.end());

    {
        typedef std::multimap<int64_t, ls_block_token_pointee_t *>::iterator ot_iter;
        ot_iter erase_it = offset_tokens.end();
        for (std::pair<ot_iter, ot_iter> range = offset_tokens.equal_range(token_offset_it->second);
             range.first != range.second;
             ++range.first) {
            if (range.first->second == token) {
                erase_it = range.first;
                break;
            }
        }

        guarantee(erase_it != offset_tokens.end(), "We probably tried unregistering the same token twice.");
        offset_tokens.erase(erase_it);
    }

    const bool last_token_for_offset = offset_tokens.find(token_offset_it->second) == offset_tokens.end();
    if (last_token_for_offset) {
        // Mark offset garbage in GC
        data_block_manager->mark_token_garbage(token_offset_it->second);
    }

    token_offsets.erase(token_offset_it);

    rassert(token_offsets.empty() == offset_tokens.empty());
    if (token_offsets.empty() && offset_tokens.empty() && state == state_shutting_down && shutdown_state == shutdown_waiting_on_block_tokens) {
#ifndef NDEBUG
        expecting_no_more_tokens = true;
#endif
        next_shutdown_step();
    }
}

void log_serializer_t::remap_block_to_new_offset(int64_t current_offset, int64_t new_offset) {
    assert_thread();
    ASSERT_NO_CORO_WAITING;

    rassert(new_offset != current_offset);

    typedef std::multimap<int64_t, ls_block_token_pointee_t *>::iterator ot_iter;
    std::pair<ot_iter, ot_iter> range = offset_tokens.equal_range(current_offset);

    if (range.first != range.second) {
        --range.second;

        bool last_time = false;
        while (!last_time) {
            last_time = (range.first == range.second);
            std::map<ls_block_token_pointee_t*, int64_t>::iterator token_offsets_iter = token_offsets.find(range.first->second);
            guarantee(token_offsets_iter != token_offsets.end());
            guarantee(token_offsets_iter->second == current_offset);

            token_offsets_iter->second = new_offset;
            offset_tokens.insert(std::pair<int64_t, ls_block_token_pointee_t *>(new_offset, range.first->second));

            ot_iter prev = range.first;
            ++range.first;
            offset_tokens.erase(prev);
        }

        data_block_manager->mark_token_garbage(current_offset);
        data_block_manager->mark_token_live(new_offset);
    }
}

// TODO: Make this const.
block_size_t log_serializer_t::get_block_size() const {
    return static_config.block_size();
}

bool log_serializer_t::coop_lock_and_check() {
    assert_thread();
    rassert(dbfile != NULL);
    return dbfile->coop_lock_and_check();
}

// TODO: Should be called end_block_id I guess (or should subtract 1 frim end_block_id?
block_id_t log_serializer_t::max_block_id() {
    assert_thread();
    rassert(state == state_ready);

    return lba_index->end_block_id();
}

counted_t<ls_block_token_pointee_t> log_serializer_t::index_read(block_id_t block_id) {
    assert_thread();
    ++stats->pm_serializer_index_reads;

    rassert(state == state_ready);

    if (block_id >= lba_index->end_block_id()) {
        return counted_t<ls_block_token_pointee_t>();
    }

    flagged_off64_t offset = lba_index->get_block_offset(block_id);
    if (offset.has_value()) {
        counted_t<ls_block_token_pointee_t> ret(
            new ls_block_token_pointee_t(this, offset.get_value()));
        return ret;
    } else {
        return counted_t<ls_block_token_pointee_t>();
    }
}

bool log_serializer_t::get_delete_bit(block_id_t id) {
    assert_thread();
    rassert(state == state_ready);

    flagged_off64_t offset = lba_index->get_block_offset(id);
    return !offset.has_value();
}

repli_timestamp_t log_serializer_t::get_recency(block_id_t id) {
    assert_thread();
    return lba_index->get_block_recency(id);
}

bool log_serializer_t::shutdown(cond_t *cb) {
    assert_thread();
    rassert(coro_t::self());

    rassert(cb);
    rassert(state == state_ready);
    shutdown_callback = cb;

    shutdown_state = shutdown_begin;
    shutdown_in_one_shot = true;

    return next_shutdown_step();
}

bool log_serializer_t::next_shutdown_step() {
    assert_thread();

    if (shutdown_state == shutdown_begin) {
        // First shutdown step
        shutdown_state = shutdown_waiting_on_serializer;
        if (last_write || active_write_count > 0) {
            state = state_shutting_down;
            shutdown_in_one_shot = false;
            return false;
        }
        state = state_shutting_down;
    }

    if (shutdown_state == shutdown_waiting_on_serializer) {
        shutdown_state = shutdown_waiting_on_datablock_manager;
        if (!data_block_manager->shutdown(this)) {
            shutdown_in_one_shot = false;
            return false;
        }
    }

    // The datablock manager uses block tokens, so it goes before.
    if (shutdown_state == shutdown_waiting_on_datablock_manager) {
        shutdown_state = shutdown_waiting_on_block_tokens;
        rassert(!(token_offsets.empty() ^ offset_tokens.empty()));
        if (!(token_offsets.empty() && offset_tokens.empty())) {
            shutdown_in_one_shot = false;
            return false;
        } else {
#ifndef NDEBUG
            expecting_no_more_tokens = true;
#endif
        }
    }

    rassert(expecting_no_more_tokens);

    if (shutdown_state == shutdown_waiting_on_block_tokens) {
        shutdown_state = shutdown_waiting_on_lba;
        if (!lba_index->shutdown(this)) {
            shutdown_in_one_shot = false;
            return false;
        }
    }

    if (shutdown_state == shutdown_waiting_on_lba) {
        metablock_manager->shutdown();
        extent_manager->shutdown();

        delete lba_index;
        lba_index = NULL;

        delete data_block_manager;
        data_block_manager = NULL;

        delete metablock_manager;
        metablock_manager = NULL;

        delete extent_manager;
        extent_manager = NULL;

        delete dbfile;
        dbfile = NULL;

        state = state_shut_down;

        // Don't call the callback if we went through the entire
        // shutdown process in one synchronous shot.
        if (!shutdown_in_one_shot && shutdown_callback) {
            shutdown_callback->pulse();
        }

        return true;
    }

    unreachable("Invalid state.");
    return true; // make compiler happy
}

void log_serializer_t::on_datablock_manager_shutdown() {
    assert_thread();
    next_shutdown_step();
}

void log_serializer_t::on_lba_shutdown() {
    assert_thread();
    next_shutdown_step();
}

void log_serializer_t::prepare_metablock(metablock_t *mb_buffer) {
    assert_thread();
    bzero(mb_buffer, sizeof(*mb_buffer));
    extent_manager->prepare_metablock(&mb_buffer->extent_manager_part);
    data_block_manager->prepare_metablock(&mb_buffer->data_block_manager_part);
    lba_index->prepare_metablock(&mb_buffer->lba_index_part);
    mb_buffer->block_sequence_id = latest_block_sequence_id;
}


void log_serializer_t::consider_start_gc() {
    assert_thread();
    if (data_block_manager->do_we_want_to_start_gcing() && state == log_serializer_t::state_ready) {
        // We do not do GC if we're not in the ready state
        // (i.e. shutting down)
        data_block_manager->start_gc();
    }
}


bool log_serializer_t::disable_gc(gc_disable_callback_t *cb) {
    assert_thread();
    return data_block_manager->disable_gc(cb);
}

void log_serializer_t::enable_gc() {
    assert_thread();
    data_block_manager->enable_gc();
}

void log_serializer_t::register_read_ahead_cb(serializer_read_ahead_callback_t *cb) {
    assert_thread();

    read_ahead_callbacks.push_back(cb);
}

void log_serializer_t::unregister_read_ahead_cb(serializer_read_ahead_callback_t *cb) {
    assert_thread();

    for (std::vector<serializer_read_ahead_callback_t*>::iterator cb_it = read_ahead_callbacks.begin(); cb_it != read_ahead_callbacks.end(); ++cb_it) {
        if (*cb_it == cb) {
            read_ahead_callbacks.erase(cb_it);
            break;
        }
    }
}

bool log_serializer_t::offer_buf_to_read_ahead_callbacks(block_id_t block_id, void *buf, const counted_t<standard_block_token_t>& token, repli_timestamp_t recency_timestamp) {
    assert_thread();
    for (size_t i = 0; i < read_ahead_callbacks.size(); ++i) {
        if (read_ahead_callbacks[i]->offer_read_ahead_buf(block_id, buf, token, recency_timestamp)) {
            return true;
        }
    }
    return false;
}

bool log_serializer_t::should_perform_read_ahead() {
    assert_thread();
    return dynamic_config.read_ahead && !read_ahead_callbacks.empty();
}

ls_block_token_pointee_t::ls_block_token_pointee_t(log_serializer_t *serializer, int64_t initial_offset)
    : serializer_(serializer), ref_count_(0) {
    serializer_->assert_thread();
    serializer_->register_block_token(this, initial_offset);
}

void ls_block_token_pointee_t::do_destroy() {
    serializer_->assert_thread();
    rassert(ref_count_ == 0);
    serializer_->unregister_block_token(this);
    delete this;
}

void adjust_ref(ls_block_token_pointee_t *p, int adjustment) {
    struct adjuster_t : public linux_thread_message_t {
        void on_thread_switch() {
            rassert(p->ref_count_ + adjustment >= 0);
            p->ref_count_ += adjustment;
            if (p->ref_count_ == 0) {
                p->do_destroy();
            }
            delete this;
        }
        ls_block_token_pointee_t *p;
        int adjustment;
    };

    if (get_thread_id() == p->serializer_->home_thread()) {
        rassert(p->ref_count_ + adjustment >= 0);
        p->ref_count_ += adjustment;
        if (p->ref_count_ == 0) {
            p->do_destroy();
        }
    } else {
        adjuster_t *adjuster = new adjuster_t;
        adjuster->p = p;
        adjuster->adjustment = adjustment;
        DEBUG_VAR bool res = continue_on_thread(p->serializer_->home_thread(), adjuster);
        rassert(!res);
    }
}

void counted_add_ref(ls_block_token_pointee_t *p) {
    adjust_ref(p, 1);
}

void counted_release(ls_block_token_pointee_t *p) {
    adjust_ref(p, -1);
}
