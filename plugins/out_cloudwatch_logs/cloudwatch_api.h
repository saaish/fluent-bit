/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2019-2020 The Fluent Bit Authors
 *  Copyright (C) 2015-2018 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#ifndef FLB_OUT_CLOUDWATCH_API
#define FLB_OUT_CLOUDWATCH_API

/*
 * The CloudWatch API documents that the maximum payload is 1,048,576 bytes
 * For reasons that are under investigation, using that number in this plugin
 * leads to API errors. No issues have been seen setting it to 1,000,000 bytes.
 */
#define PUT_LOG_EVENTS_PAYLOAD_SIZE    1000000
#define MAX_EVENTS_PER_PUT             10000

/* number of characters needed to 'end' a PutLogEvents payload */
#define PUT_LOG_EVENTS_FOOTER_LEN      4

#include "cloudwatch_logs.h"

/* buffers used for each flush */
struct cw_flush {
    /* temporary buffer for storing the serialized event messages */
    char *tmp_buf;
    size_t tmp_buf_size;
    /* log events- each of these has a pointer to their message in tmp_buf */
    struct event *events;
    int events_capacity;
    /* the payload of the API request */
    char *out_buf;
    size_t out_buf_size;
};

void destroy_cw_flush(struct cw_flush *buf);

int msg_pack_to_events(struct flb_cloudwatch *ctx, struct flush *buf,
                       const char *data, size_t bytes);
int send_in_batches(struct flb_cloudwatch *ctx, struct flush *buf,
                    struct log_stream *stream, int event_count);
int create_log_stream(struct flb_cloudwatch *ctx, struct log_stream *stream);
struct log_stream *get_log_stream(struct flb_cloudwatch *ctx,
                                  const char *tag, int tag_len);
int put_log_events(struct flb_cloudwatch *ctx, struct flush *buf,
                   struct log_stream *stream,
                   size_t payload_size);
int create_log_group(struct flb_cloudwatch *ctx);
int compare_events(const void *a_arg, const void *b_arg);

#endif
