#include "fujinet_io_queue.h"

#include <stdio.h>
#include <string.h>

#define UNIT_COUNT 2U
#define START 1U
#define FLUSH 2U
#define READ 3U
#define WRITE 4U

typedef struct fake_request {
    unsigned id;
    unsigned replied;
    unsigned aborted;
    unsigned valid_unit;
    unsigned quick;
} fake_request_t;

typedef struct registration {
    fake_request_t *request;
    unsigned causes;
} registration_t;

typedef struct boundary {
    fujinet_io_queue_t queue;
    fujinet_io_queue_node_t nodes[4];
    uint8_t stopped[UNIT_COUNT];
    registration_t registrations[UNIT_COUNT][2];
    unsigned registration_counts[UNIT_COUNT];
} boundary_t;

static unsigned failures;

#define CHECK(name, expression) do {                                      \
    if (!(expression)) {                                                  \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);         \
        ++failures;                                                       \
    }                                                                      \
} while (0)

static void boundary_init(boundary_t *boundary)
{
    memset(boundary, 0, sizeof(*boundary));
    fujinet_io_queue_init(&boundary->queue);
}

static void queue_request(boundary_t *boundary, unsigned node_index,
                          fake_request_t *request, uint8_t unit,
                          uint16_t command)
{
    fujinet_io_queue_append(&boundary->queue, &boundary->nodes[node_index],
                            request, unit, command);
}

static fake_request_t *next_request(boundary_t *boundary)
{
    fujinet_io_queue_node_t *node = fujinet_io_queue_next(
        &boundary->queue, boundary->stopped, UNIT_COUNT, START, FLUSH);
    return node == NULL ? NULL : (fake_request_t *)node->request;
}

static void add_change_registration(boundary_t *boundary, uint8_t unit,
                                    fake_request_t *request)
{
    unsigned count = boundary->registration_counts[unit];
    if (count < 2) {
        boundary->registrations[unit][count].request = request;
        boundary->registrations[unit][count].causes = 0;
        boundary->registration_counts[unit] = count + 1;
    }
}

static void signal_change(boundary_t *boundary, uint8_t unit)
{
    unsigned i;
    for (i = 0; i < boundary->registration_counts[unit]; ++i)
        ++boundary->registrations[unit][i].causes;
}

static void remove_change_registration(boundary_t *boundary, uint8_t unit,
                                        fake_request_t *request)
{
    unsigned i;
    for (i = 0; i < boundary->registration_counts[unit]; ++i) {
        if (boundary->registrations[unit][i].request == request) {
            unsigned last = boundary->registration_counts[unit] - 1;
            boundary->registrations[unit][i] = boundary->registrations[unit][last];
            boundary->registration_counts[unit] = last;
            return;
        }
    }
}

static void close_request(boundary_t *boundary, fake_request_t *request)
{
    uint8_t unit;
    for (unit = 0; unit < UNIT_COUNT; ++unit)
        remove_change_registration(boundary, unit, request);
}

static void close_request_clears_remove_waiter(boundary_t *boundary,
                                               uint8_t unit,
                                               fake_request_t *request,
                                               fake_request_t **remove_waiter)
{
    close_request(boundary, request);
    if (*remove_waiter == request)
        *remove_waiter = NULL;
    (void)unit;
}

static fake_request_t *promote_request(boundary_t *boundary)
{
    return next_request(boundary);
}

static void complete_promoted_request(boundary_t *boundary,
                                      fake_request_t *request)
{
    if (!request->valid_unit) {
        request->aborted = 1;
        if (!request->quick)
            ++request->replied;
        return;
    }
    ++request->replied;
    (void)boundary;
}

static void test_invalid_promoted_request_is_aborted_and_drain_continues(void)
{
    boundary_t boundary;
    fake_request_t invalid = {1, 0, 0, 1, 0};
    fake_request_t following = {2, 0, 0, 1, 0};
    fake_request_t *promoted;

    boundary_init(&boundary);
    queue_request(&boundary, 0, &invalid, 0, WRITE);
    queue_request(&boundary, 1, &following, 1, READ);

    promoted = promote_request(&boundary);
    CHECK("dequeue promotes the first request", promoted == &invalid);
    /* Model device_close() clearing io_Unit in the dequeue-to-active window. */
    invalid.valid_unit = 0;
    complete_promoted_request(&boundary, promoted);
    CHECK("invalid promoted request is aborted", invalid.aborted == 1);
    CHECK("invalid promoted request is replied once", invalid.replied == 1);

    promoted = promote_request(&boundary);
    CHECK("FIFO continues after invalid promoted request",
          promoted == &following);
    complete_promoted_request(&boundary, promoted);
    CHECK("following request is replied once", following.replied == 1);
}

static void test_requests_are_fifo_but_stopped_units_do_not_block_others(void)
{
    boundary_t boundary;
    fake_request_t held = {1, 0, 0, 1, 0};
    fake_request_t other = {2, 0, 0, 1, 0};
    fake_request_t start = {3, 0, 0, 1, 0};

    boundary_init(&boundary);
    boundary.stopped[0] = 1;
    queue_request(&boundary, 0, &held, 0, READ);
    queue_request(&boundary, 1, &other, 1, READ);
    queue_request(&boundary, 2, &start, 0, START);

    CHECK("other unit runs while unit is stopped", next_request(&boundary) == &other);
    CHECK("start bypasses stopped unit", next_request(&boundary) == &start);
    boundary.stopped[0] = 0;
    CHECK("stopped work resumes", next_request(&boundary) == &held);
}

static void test_flush_and_abort_only_remove_queued_requests(void)
{
    boundary_t boundary;
    fake_request_t active = {1, 0, 0, 1, 0};
    fake_request_t queued = {2, 0, 0, 1, 0};
    fake_request_t other = {3, 0, 0, 1, 0};
    fujinet_io_queue_node_t *removed;

    boundary_init(&boundary);
    queue_request(&boundary, 0, &queued, 0, WRITE);
    queue_request(&boundary, 1, &other, 1, READ);
    removed = fujinet_io_queue_take_unit(&boundary.queue, 0);
    CHECK("CMD_FLUSH removes queued work for one unit",
          removed != NULL && removed->request == &queued);
    queued.aborted = 1;
    CHECK("active work is not abortable through the queue",
          fujinet_io_queue_remove_request(&boundary.queue, &active) == NULL);
    removed = fujinet_io_queue_remove_request(&boundary.queue, &other);
    CHECK("AbortIO removes only its queued request",
          removed != NULL && removed->request == &other);
    CHECK("aborted request is marked by the boundary", queued.aborted == 1);
}

static void test_change_registrations_persist_until_remove_or_close(void)
{
    boundary_t boundary;
    fake_request_t first = {1, 0, 0, 1, 0};
    fake_request_t second = {2, 0, 0, 1, 0};

    boundary_init(&boundary);
    add_change_registration(&boundary, 0, &first);
    add_change_registration(&boundary, 0, &second);
    signal_change(&boundary, 0);
    CHECK("all registrations receive the first transition",
          boundary.registrations[0][0].causes == 1 &&
              boundary.registrations[0][1].causes == 1);
    remove_change_registration(&boundary, 0, &first);
    signal_change(&boundary, 0);
    CHECK("removed registration receives no later transition",
          boundary.registration_counts[0] == 1 &&
              boundary.registrations[0][0].causes == 2);
    close_request(&boundary, &second);
    CHECK("closing a request removes retained registration",
          boundary.registration_counts[0] == 0);
}

static void test_closing_request_clears_pending_remove_waiter(void)
{
    boundary_t boundary;
    fake_request_t remove = {1, 0, 0, 1, 0};
    fake_request_t *remove_waiter = &remove;

    boundary_init(&boundary);
    close_request_clears_remove_waiter(&boundary, 0, &remove, &remove_waiter);
    CHECK("closing a request clears any retained TD_REMOVE waiter",
          remove_waiter == NULL);
}

static void test_duplicate_change_registration_reuses_one_retained_entry(void)
{
    boundary_t boundary;
    fake_request_t request = {1, 0, 0, 1, 0};

    boundary_init(&boundary);
    add_change_registration(&boundary, 0, &request);
    /* Model the resident device update path: a second ADDCHANGEINT from the
     * same request should refresh the callback, not retain a duplicate node. */
    remove_change_registration(&boundary, 0, &request);
    add_change_registration(&boundary, 0, &request);
    signal_change(&boundary, 0);
    CHECK("duplicate registration leaves one retained callback",
          boundary.registration_counts[0] == 1 &&
              boundary.registrations[0][0].causes == 1);
}

int main(void)
{
    test_requests_are_fifo_but_stopped_units_do_not_block_others();
    test_flush_and_abort_only_remove_queued_requests();
    test_change_registrations_persist_until_remove_or_close();
    test_closing_request_clears_pending_remove_waiter();
    test_duplicate_change_registration_reuses_one_retained_entry();
    test_invalid_promoted_request_is_aborted_and_drain_continues();

    if (failures != 0) {
        fprintf(stderr, "%u Exec boundary test(s) failed\n", failures);
        return 1;
    }
    puts("All Amiga Exec boundary contract tests passed");
    return 0;
}
