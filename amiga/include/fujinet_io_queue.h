#ifndef FUJINET_IO_QUEUE_H
#define FUJINET_IO_QUEUE_H

#include <stdint.h>

typedef struct fujinet_io_queue_node {
    struct fujinet_io_queue_node *next;
    struct fujinet_io_queue_node *previous;
    void *request;
    uint16_t command;
    uint8_t unit;
} fujinet_io_queue_node_t;

typedef struct fujinet_io_queue {
    fujinet_io_queue_node_t *head;
    fujinet_io_queue_node_t *tail;
} fujinet_io_queue_t;

void fujinet_io_queue_init(fujinet_io_queue_t *queue);
void fujinet_io_queue_append(fujinet_io_queue_t *queue,
                             fujinet_io_queue_node_t *node, void *request,
                             uint8_t unit, uint16_t command);
fujinet_io_queue_node_t *fujinet_io_queue_next(
    fujinet_io_queue_t *queue, const uint8_t *stopped, uint8_t unit_count,
    uint16_t start_command, uint16_t flush_command);
fujinet_io_queue_node_t *fujinet_io_queue_remove_request(
    fujinet_io_queue_t *queue, const void *request);
fujinet_io_queue_node_t *fujinet_io_queue_take_unit(
    fujinet_io_queue_t *queue, uint8_t unit);

#endif
