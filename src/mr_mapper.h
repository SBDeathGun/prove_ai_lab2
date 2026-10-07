#ifndef MR_MAPPER_H
#define MR_MAPPER_H

#include "mr.h"
#include "mr_common.h"

int mapper_process_main(
    size_t queue_size,
    size_t mapper_threads,
    mr_mapper_t mapper_cb,
    void *user_arg,
    mr_logger_t *logger
);

#endif /* MR_MAPPER_H */
