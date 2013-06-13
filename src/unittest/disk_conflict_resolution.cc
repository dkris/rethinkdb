// Copyright 2010-2012 RethinkDB, all rights reserved.
#include <vector>

#include "errors.hpp"
#include <boost/bind.hpp>

#include "arch/io/disk/conflict_resolving.hpp"
#include "arch/runtime/thread_pool.hpp"
#include "containers/intrusive_list.hpp"
#include "containers/scoped.hpp"
#include "unittest/gtest.hpp"

namespace unittest {

struct core_action_t : public intrusive_list_node_t<core_action_t> {
    /* We need for multiple test_driver_t objects to share a file
       descriptor in order to test the conflict resolution logic, but
       it doesn't matter what that file descriptor is. */
    static const int IRRELEVANT_DEFAULT_FD = 0;

    bool get_is_write() const { return !is_read; }
    bool get_is_read() const { return is_read; }
    fd_t get_fd() const { return fd; }
    void *get_buf() const { return buf; }
    size_t get_count() const { return count; }
    int64_t get_offset() const { return offset; }
    void set_successful_due_to_conflict() { }

    bool is_read;
    void *buf;
    size_t count;
    int64_t offset;

    core_action_t() :
        has_begun(false), done(false), fd(IRRELEVANT_DEFAULT_FD) { }
    bool has_begun, done;
    fd_t fd;
};

void debug_print(printf_buffer_t *buf,
                  const core_action_t &action) {
    buf->appendf("core_action{is_read=%s, buf=%p, count=%zu, "
                 "offset=%" PRIi64 ", has_begun=%s, done=%s, fd=%d}",
                 action.is_read ? "true" : "false",
                 action.buf,
                 action.count,
                 action.offset,
                 action.has_begun ? "true" : "false",
                 action.done ? "true" : "false",
                 action.fd);
}

struct test_driver_t {
    intrusive_list_t<core_action_t> running_actions;
    std::vector<char> data;

    conflict_resolving_diskmgr_t<core_action_t> conflict_resolver;

    typedef conflict_resolving_diskmgr_t<core_action_t>::action_t action_t;

    int old_thread_id;
    test_driver_t() : conflict_resolver(&get_global_perfmon_collection()) {
        /* Fake thread-context to make perfmons work. */
        old_thread_id = linux_thread_pool_t::thread_id;
        linux_thread_pool_t::thread_id = 0;

        conflict_resolver.submit_fun = boost::bind(
            &test_driver_t::submit_from_conflict_resolving_diskmgr, this, _1);
        conflict_resolver.done_fun = boost::bind(
            &test_driver_t::done_from_conflict_resolving_diskmgr, this, _1);
    }
    ~test_driver_t() {
        linux_thread_pool_t::thread_id = old_thread_id;
    }

    void submit(action_t *a) {
        conflict_resolver.submit(a);
    }

    void submit_from_conflict_resolving_diskmgr(core_action_t *a) {

        rassert(!a->has_begun);
        rassert(!a->done);
        a->has_begun = true;

        /* The conflict_resolving_diskmgr_t should not have sent us two potentially
        conflicting actions */
        for (core_action_t *p = running_actions.head(); p; p = running_actions.next(p)) {
            if (!(a->is_read && p->is_read)) {
                ASSERT_TRUE(a->offset >= static_cast<int64_t>(p->offset + p->count)
                            || p->offset >= static_cast<int64_t>(a->offset + a->count));
            }
        }

        running_actions.push_back(a);
    }

    void permit(core_action_t *a) {
        if (a->done) return;
        rassert(a->has_begun);
        running_actions.remove(a);

        if (a->offset + a->count > data.size()) {
            data.resize(a->offset + a->count, 0);
        }
        if (a->is_read) {
            memcpy(a->buf, data.data() + a->offset, a->count);
        } else {
            memcpy(data.data() + a->offset, a->buf, a->count);
        }

        conflict_resolver.done(a);
    }

    void done_from_conflict_resolving_diskmgr(core_action_t *a) {
        a->done = true;
    }
};

struct read_test_t {

    read_test_t(test_driver_t *_driver, int64_t o, const std::string &e) :
        driver(_driver),
        offset(o),
        expected(e),
        buffer(expected.size())
    {
        action.is_read = true;
        action.fd = 0;
        action.buf = buffer.data();
        action.count = expected.size();
        action.offset = offset;
        driver->submit(&action);
    }
    test_driver_t *driver;
    int64_t offset;
    std::string expected;
    scoped_array_t<char> buffer;
    test_driver_t::action_t action;
    bool was_sent() {
        return action.done || action.has_begun;
    }
    bool was_completed() {
        return action.done;
    }
    void go() {
        ASSERT_TRUE(was_sent());
        driver->permit(&action);
        ASSERT_TRUE(was_completed());
    }
    ~read_test_t() {
        EXPECT_TRUE(was_completed());
        std::string got(buffer.data(), expected.size());
        EXPECT_EQ(expected, got) << "Read returned wrong data.";
    }
};

struct write_test_t {

    write_test_t(test_driver_t *_driver, int64_t o, const std::string &d) :
        driver(_driver),
        offset(o),
        data(d.begin(), d.end())
    {
        action.is_read = false;
        action.fd = 0;
        action.buf = data.data();
        action.count = d.size();
        action.offset = o;
        driver->submit(&action);
    }

    test_driver_t *driver;
    int64_t offset;
    std::vector<char> data;
    test_driver_t::action_t action;

    bool was_sent() {
        return action.done || action.has_begun;
    }
    bool was_completed() {
        return action.done;
    }
    void go() {
        ASSERT_TRUE(was_sent());
        driver->permit(&action);
        ASSERT_TRUE(was_completed());
    }
    ~write_test_t() {
        EXPECT_TRUE(was_completed());
    }
};

/* WriteWriteConflict verifies that if two writes are sent, they will be run in the correct
order. */

TEST(DiskConflictTest, WriteWriteConflict) {
    test_driver_t d;
    write_test_t w1(&d, 0, "foo");
    write_test_t w2(&d, 0, "bar");
    read_test_t verifier(&d, 0, "bar");
    w1.go();
    w2.go();
    verifier.go();
}

/* WriteReadConflict verifies that if a write and then a read are sent, the write will happen
before the read. */

TEST(DiskConflictTest, WriteReadConflict) {
    test_driver_t d;
    write_test_t initial_write(&d, 0, "initial");
    write_test_t w(&d, 0, "foo");
    read_test_t r(&d, 0, "foo");
    initial_write.go();
    w.go();
    r.go();
}

/* ReadWriteConflict verifies that if a read and then a write are sent, the read will happen
before the write. */

TEST(DiskConflictTest, ReadWriteConflict) {
    test_driver_t d;
    write_test_t initial_write(&d, 0, "initial");
    read_test_t r(&d, 0, "init");
    write_test_t w(&d, 0, "something_else");
    initial_write.go();
    r.go();
    w.go();
}

/* NoSpuriousConflicts verifies that if two writes that don't overlap are sent, there are
no problems. */

TEST(DiskConflictTest, NoSpuriousConflicts) {
    test_driver_t d;
    write_test_t w1(&d, 0, "foo");
    write_test_t w2(&d, 4096, "bar");
    ASSERT_TRUE(w1.was_sent());
    ASSERT_TRUE(w2.was_sent());
    w1.go();
    w2.go();
}

/* ReadReadPass verifies that reads do not block reads */

TEST(DiskConflictTest, NoReadReadConflict) {
    test_driver_t d;
    write_test_t initial_write(&d, 0, "foo");
    read_test_t r1(&d, 0, "foo");
    read_test_t r2(&d, 0, "foo");
    initial_write.go();
    ASSERT_TRUE(r1.was_sent());
    ASSERT_TRUE(r2.was_sent());
    r1.go();
    r2.go();
}

/* WriteReadSubrange verifies that if a write and then a read are sent, and the read
is for a subrange of the write, the read gets the right value */

TEST(DiskConflictTest, WriteReadSubrange) {
    test_driver_t d;
    write_test_t w(&d, 0, "abcdefghijklmnopqrstuvwxyz");
    read_test_t r(&d, 3, "defghijkl");
    w.go();
    r.go();
}

/* WriteReadSuperrange verifies that if a write and then a read are sent, and the read
is for a superrange of the write, the read gets the right value */

TEST(DiskConflictTest, WriteReadSuperrange) {
    test_driver_t d;
    write_test_t initial_write(&d, 0, "abc____________________xyz");
    write_test_t w(&d, 3, "defghijklmnopqrstuvw");
    read_test_t r(&d, 0, "abcdefghijklmnopqrstuvwxyz");
    initial_write.go();
    w.go();
    r.go();
}

/* MetaTest is a sanity check to make sure that the above tests are actually testing something. */

void cause_test_failure() {
    test_driver_t d;
    write_test_t w(&d, 0, "foo");
    read_test_t r(&d, 0, "bar");   // We write "foo" but expect to read "bar"
    w.go();
    r.go();
}

TEST(DiskConflictTest, MetaTest) {
    EXPECT_NONFATAL_FAILURE(cause_test_failure(), "Read returned wrong data.");
};

}  // namespace unittest

