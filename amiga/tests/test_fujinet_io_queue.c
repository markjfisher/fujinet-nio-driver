#include "fujinet_io_queue.h"

#include <stdio.h>

#define START 1U
#define FLUSH 2U
#define READ 3U
#define WRITE 4U
#define UPDATE 5U

static unsigned failures;

#define CHECK(name, expression) do {                                      \
    if (!(expression)) {                                                  \
        fprintf(stderr, "FAIL: %s (line %d)\n", name, __LINE__);          \
        ++failures;                                                       \
    }                                                                     \
} while (0)

static void test_fifo_and_update_barrier(void)
{
    fujinet_io_queue_t queue;
    fujinet_io_queue_node_t nodes[3];
    uint8_t stopped[2] = {0, 0};
    int write_a, write_b, update;
    fujinet_io_queue_init(&queue);
    fujinet_io_queue_append(&queue, &nodes[0], &write_a, 0, WRITE);
    fujinet_io_queue_append(&queue, &nodes[1], &write_b, 0, WRITE);
    fujinet_io_queue_append(&queue, &nodes[2], &update, 0, UPDATE);
    CHECK("first write remains first", fujinet_io_queue_next(
              &queue, stopped, 2, START, FLUSH)->request == &write_a);
    CHECK("second write remains before update", fujinet_io_queue_next(
              &queue, stopped, 2, START, FLUSH)->request == &write_b);
    CHECK("update is the barrier after writes", fujinet_io_queue_next(
              &queue, stopped, 2, START, FLUSH)->request == &update);
}

static void test_stop_start_and_other_units(void)
{
    fujinet_io_queue_t queue;
    fujinet_io_queue_node_t nodes[3];
    uint8_t stopped[2] = {1, 0};
    int held, other, start;
    fujinet_io_queue_init(&queue);
    fujinet_io_queue_append(&queue, &nodes[0], &held, 0, READ);
    fujinet_io_queue_append(&queue, &nodes[1], &other, 1, READ);
    fujinet_io_queue_append(&queue, &nodes[2], &start, 0, START);
    CHECK("stopped unit does not block another unit", fujinet_io_queue_next(
              &queue, stopped, 2, START, FLUSH)->request == &other);
    CHECK("start bypasses its stopped unit", fujinet_io_queue_next(
              &queue, stopped, 2, START, FLUSH)->request == &start);
    stopped[0] = 0;
    CHECK("held work resumes after start", fujinet_io_queue_next(
              &queue, stopped, 2, START, FLUSH)->request == &held);
}

static void test_flush_and_abort_remove_only_queued_requests(void)
{
    fujinet_io_queue_t queue;
    fujinet_io_queue_node_t nodes[3];
    uint8_t stopped[2] = {0, 0};
    int active = 0, same_unit = 0, other_unit = 0;
    fujinet_io_queue_node_t *removed;
    fujinet_io_queue_init(&queue);
    fujinet_io_queue_append(&queue, &nodes[0], &same_unit, 0, WRITE);
    fujinet_io_queue_append(&queue, &nodes[1], &other_unit, 1, READ);
    removed = fujinet_io_queue_take_unit(&queue, 0);
    CHECK("flush takes queued request for selected unit",
          removed != 0 && removed->request == &same_unit);
    CHECK("active request is not in queue and remains non-abortable",
          fujinet_io_queue_remove_request(&queue, &active) == 0);
    CHECK("other unit remains after flush", fujinet_io_queue_next(
              &queue, stopped, 2, START, FLUSH)->request == &other_unit);

    fujinet_io_queue_init(&queue);
    fujinet_io_queue_append(&queue, &nodes[2], &same_unit, 0, WRITE);
    removed = fujinet_io_queue_remove_request(&queue, &same_unit);
    CHECK("AbortIO removes its queued request",
          removed != 0 && queue.head == 0 && queue.tail == 0);
}

int main(void)
{
    test_fifo_and_update_barrier();
    test_stop_start_and_other_units();
    test_flush_and_abort_remove_only_queued_requests();
    if (failures != 0) return 1;
    puts("All Amiga FIFO policy tests passed");
    return 0;
}
