#ifndef MR_REDUCER_H
#define MR_REDUCER_H

#include "mr.h"
#include "mr_common.h"

int reducer_process_main(
    size_t queue_size,
    size_t reducer_threads,
    mr_reducer_t reducer_cb,
    void *user_arg,
    mr_hash_t hash_func,
    void *hash_arg,
    mr_logger_t *logger
);

#endif /* MR_REDUCER_H */
