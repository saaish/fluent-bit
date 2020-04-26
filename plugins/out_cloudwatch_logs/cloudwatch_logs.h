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

#ifndef FLB_OUT_CLOUDWATCH_LOGS_H
#define FLB_OUT_CLOUDWATCH_LOGS_H

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_sds.h>
#include <fluent-bit/flb_aws_credentials.h>
#include <fluent-bit/flb_http_client.h>
#include <fluent-bit/flb_aws_util.h>
#include <fluent-bit/flb_signv4.h>
#include <pthread.h>

struct event {
    char *json;
    size_t len;
    // TODO: re-usable in kinesis streams plugin if we make it timespec instead
    // uint64_t?
    unsigned long long timestamp;
};

struct log_stream {
    flb_sds_t name;
    flb_sds_t sequence_token;
    /*
     * log streams in CloudWatch do not expire; but our internal representations
     * of them are periodically cleaned up if they have been unused for too long
     */
    time_t expiration;

    /*
     * Used to track the "time span" of a single PutLogEvents payload
     * Which can not exceed 24 hours.
     */
    unsigned long long oldest_event;
    unsigned long long newest_event;

    /*
     * Concurrent writes to a single log stream will not work because
     * of the sequence token (I really wish they would remove that
     * from the API...)
     */
    pthread_mutex_t lock;

    struct mk_list _head;
};

void log_stream_destroy(struct log_stream *stream);

struct flb_cloudwatch {
    /*
     * TLS instances can not be re-used. So we have one for:
     * - Base cred provider (needed for EKS provider)
     * - STS Assume role provider
     * - The CloudWatch Logs client for this plugin
     */
    struct flb_tls cred_tls;
    struct flb_tls sts_tls;
    struct flb_tls client_tls;
    struct flb_aws_provider *aws_provider;
    struct flb_aws_provider *base_aws_provider;
    struct flb_aws_client *cw_client;

    /* configuration options */
    const char *log_stream_name;
    const char *log_stream_prefix;
    const char *log_group;
    const char *region;
    const char *log_format;
    const char *role_arn;
    const char *log_key;
    int custom_endpoint;
    /* Should the plugin create the log group */
    int create_group;

    /* has the log group successfully been created */
    int group_created;

    /* must be freed on shutdown if custom_endpoint is not set */
    char *endpoint;

    /* if we're writing to a static log stream, we'll use this */
    struct log_stream stream;
    int stream_created;
    /* if the log stream is dynamic, we'll use this */
    struct mk_list streams;

    /* Plugin output instance reference */
    struct flb_output_instance *ins;
};

void flb_cloudwatch_ctx_destroy(struct flb_cloudwatch *ctx);

#endif
