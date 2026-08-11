#include "fujinet_io_queue.h"

static void detach(fujinet_io_queue_t *queue, fujinet_io_queue_node_t *node)
{
    if (node->previous != 0) node->previous->next = node->next;
    else queue->head = node->next;
    if (node->next != 0) node->next->previous = node->previous;
    else queue->tail = node->previous;
    node->next = node->previous = 0;
}

void fujinet_io_queue_init(fujinet_io_queue_t *queue)
{ queue->head = queue->tail = 0; }

void fujinet_io_queue_append(fujinet_io_queue_t *queue,
                             fujinet_io_queue_node_t *node, void *request,
                             uint8_t unit, uint16_t command)
{
    node->next = 0;
    node->previous = queue->tail;
    node->request = request;
    node->unit = unit;
    node->command = command;
    if (queue->tail != 0) queue->tail->next = node;
    else queue->head = node;
    queue->tail = node;
}

fujinet_io_queue_node_t *fujinet_io_queue_next(
    fujinet_io_queue_t *queue, const uint8_t *stopped, uint8_t unit_count,
    uint16_t start_command, uint16_t flush_command)
{
    fujinet_io_queue_node_t *node;
    for (node = queue->head; node != 0; node = node->next) {
        if (node->unit < unit_count &&
            (!stopped[node->unit] || node->command == start_command ||
             node->command == flush_command)) {
            detach(queue, node);
            return node;
        }
    }
    return 0;
}

fujinet_io_queue_node_t *fujinet_io_queue_remove_request(
    fujinet_io_queue_t *queue, const void *request)
{
    fujinet_io_queue_node_t *node;
    for (node = queue->head; node != 0; node = node->next) {
        if (node->request == request) {
            detach(queue, node);
            return node;
        }
    }
    return 0;
}

fujinet_io_queue_node_t *fujinet_io_queue_take_unit(
    fujinet_io_queue_t *queue, uint8_t unit)
{
    fujinet_io_queue_node_t *node;
    for (node = queue->head; node != 0; node = node->next) {
        if (node->unit == unit) {
            detach(queue, node);
            return node;
        }
    }
    return 0;
}
