/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2019      The Fluent Bit Authors
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

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_sds.h>
#include <fluent-bit/flb_http_client.h>
#include <fluent-bit/flb_aws_credentials.h>
#include <fluent-bit/flb_aws_util.h>

#include <jsmn/jsmn.h>
#include <stdlib.h>
#include <time.h>


/*
 * A provider that obtains credentials from an http endpoint.
 * In ECS the ECS Agent vends credentials via a link local IP address.
 * Some customers build local HTTP services that provide the same functionality.
 */
static struct aws_credentials_provider_http {
    struct aws_credentials *credentials;
    time_t cred_refresh;

    struct aws_http_client *client;

    /* Host and Path to request credentials */
    char *host;
    char *path;
};

/*
 * A provider that obtains credentials from EC2 IMDS.
 */
static struct aws_credentials_provider_imds {
    struct aws_credentials *credentials;
    time_t cred_refresh;

    /* upstream connection to IMDS */
    struct aws_http_client *client;

    /* IMDSv2 Token */
    flb_sds_t imds_v2_token;
    size_t imds_v2_token_len;
    time_t token_refresh;
};

/*
 * The standard credential provider chain:
 * 1. Environment variables
 * 2. Shared credentials file (AWS Profile)
 * 3. EKS OIDC
 * 4. EC2 IMDS
 * 5. ECS HTTP credentials endpoint
 *
 * This provider will evaluate each provider in order, returning the result
 * from the first provider that returns valid credentials.
 *
 * Note: Client code should use this provider by default.
 */
struct aws_credentials_provider_default_chain {
    struct mk_list providers;
};

aws_credentials *get_credentials_fn_standard_chain(struct aws_credentials_provider *provider)
{
    struct aws_credentials_provider *sub_provider;
    struct aws_credentials_provider_default_chain *implementation;
    struct mk_list *tmp;
    struct mk_list *head;
    struct aws_credentials *creds;

    implementation = provider->implementation;

    /* return credentials from the first provider that produces a valid set */
    mk_list_foreach_safe(head, tmp, &implementation->providers) {
        sub_provider = mk_list_entry(head,
                                     struct aws_credentials_provider,
                                     _head);
        creds = sub_provider->provider_vtable->get_credentials();
        if (creds) {
            return creds;
        }

    }

    return NULL;
}

int refresh_fn_standard_chain(struct aws_credentials_provider *provider)
{
    struct aws_credentials_provider *sub_provider;
    struct aws_credentials_provider_default_chain *implementation;
    struct mk_list *tmp;
    struct mk_list *head;
    int ret;

    implementation = provider->implementation;

    /* return when a provider indicates it successfully refreshed */
    mk_list_foreach_safe(head, tmp, &implementation->providers) {
        sub_provider = mk_list_entry(head,
                                     struct aws_credentials_provider,
                                     _head);
        ret = sub_provider->provider_vtable->refresh();
        if (ret == 0) {
            return 0;
        }

    }

    return -1;
}

void destroy_fn_standard_chain(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider *sub_provider;
    struct aws_credentials_provider_default_chain *implementation;
    struct mk_list *tmp;
    struct mk_list *head;

    implementation = provider->implementation;

    mk_list_foreach_safe(head, tmp, &ctx->conditions) {
        sub_provider = mk_list_entry(head, struct aws_credentials_provider,
                                     _head);
        mk_list_del(&sub_provider->_head);
        aws_provider_destroy(sub_provider);
    }
}

static struct aws_credentials_provider_vtable standard_chain_provider_vtable = {
    .get_credentials = get_credentials_fn_standard_chain,
    .refresh = refresh_fn_standard_chain,
    .destroy = destroy_fn_standard_chain,
};

struct aws_credentials_provider *new_standard_chain_provider(struct flb_config
                                                             *config,
                                                             struct flb_tls *tls,
                                                             char *region,
                                                             char *proxy,
                                                             aws_http_client_generator
                                                             *generator)
{
    struct aws_credentials_provider *sub_provider;
    struct aws_credentials_provider *provider;
    struct aws_credentials_provider_default_chain *implementation;

    provider = flb_calloc(1, sizeof(struct aws_credentials_provider));

    if (!provider) {
        flb_errno();
        return NULL;
    }

    implementation = flb_calloc(1,
                                sizeof(
                                struct aws_credentials_provider_default_chain));

    if (!implementation) {
        flb_errno();
        flb_free(provider);
        return NULL;
    }

    provider->provider_vtable = &standard_chain_provider_vtable;
    provider->implementation = implementation;

    /* Create chain of providers */
    mk_list_init(&implementation->providers);

    sub_provider = new_environment_provider();
    if (!sub_provider) {
        /* Env provider will only fail creation if a memory alloc failed */
        aws_provider_destroy(provider);
        return NULL;
    }

    mk_list_add(&sub_provider->_head, &implementation->providers);

    sub_provider = new_profile_provider();
    if (sub_provider) {
        /* Profile provider can fail if HOME env var is not set */;
        mk_list_add(&sub_provider->_head, &implementation->providers);
    }

    sub_provider = *new_eks_provider(config, tls, region, proxy, generator);
    if (sub_provider) {
        /* EKS provider can fail if we are not running in k8s */;
        mk_list_add(&sub_provider->_head, &implementation->providers);
    }

    sub_provider = new_imds_provider(config, generator);
    if (!sub_provider) {
        /* IMDS provider will only fail creation if a memory alloc failed */
        aws_provider_destroy(provider);
        return NULL;
    }

    mk_list_add(&sub_provider->_head, &implementation->providers);

    sub_provider = new_ecs_provider(config, generator);
    if (sub_provider) {
        /* ECS Provider will fail creation if we are not running in ECS */
        mk_list_add(&sub_provider->_head, &implementation->providers);
    }

    return provider;
}

/* Environment Provider */
aws_credentials *get_credentials_fn_environment(struct aws_credentials_provider
                                                *provider)
{
    char *access_key;
    char *secret_key;
    char *session_token;
    aws_credentials *creds;

    flb_debug("[aws_credentials] Requesting credentials from the env provider..");

    access_key = getenv(AWS_ACCESS_KEY_ID);
    if (!access_key || strlen(access_key) <= 0) {
        return NULL;
    }

    secret_key = getenv(AWS_SECRET_ACCESS_KEY);
    if (!secret_key || strlen(secret_key) <= 0) {
        return NULL;
    }

    creds = flb_malloc(sizeof(struct aws_credentials));
    if (!creds) {
        flb_errno();
        return NULL;
    }

    creds->access_key_id = flb_sds_create(access_key);
    if (!creds->access_key_id) {
        aws_credentials_destroy(creds);
        flb_errno();
        return NULL;
    }

    creds->secret_access_key = flb_sds_create(secret_key);
    if (!creds->secret_access_key) {
        aws_credentials_destroy(creds);
        flb_errno();
        return NULL;
    }

    session_token = getenv(AWS_SESSION_TOKEN);
    if (session_token && strlen(session_token) > 0) {
        creds->session_token = flb_sds_create(session_token);
        if (!creds->session_token) {
            aws_credentials_destroy(creds);
            flb_errno();
            return NULL;
        }
    } else {
        creds->session_token = NULL;
    }

    return creds;

}

/*
 * For the env provider, refresh simply checks if the environment
 * variables are available.
 */
int refresh_fn_environment(struct aws_credentials_provider *provider)
{
    char *access_key;
    char *secret_key;

    flb_debug("[aws_credentials] Refresh called on the env provider");

    access_key = getenv(AWS_ACCESS_KEY_ID);
    if (!access_key || strlen(access_key) <= 0) {
        return -1;
    }

    secret_key = getenv(AWS_SECRET_ACCESS_KEY);
    if (!secret_key || strlen(secret_key) <= 0) {
        return -1;
    }

    return 0;
}

/* Destroy is a no-op for the env provider */
void destroy_fn_environment(struct aws_credentials_provider *provider) {
    return;
}

static struct aws_credentials_provider_vtable environment_provider_vtable = {
    .get_credentials = get_credentials_fn_environment,
    .refresh = refresh_fn_environment,
    .destroy = destroy_fn_environment,
};

struct aws_credentials_provider *new_environment_provider() {
    struct aws_credentials_provider *provider = flb_calloc(1,
                                                          sizeof(
                                                          struct aws_credentials_provider));

    if (!provider) {
        flb_errno();
        return NULL;
    }

    provider->provider_vtable = &environment_provider_vtable;
    provider->implementation = NULL;

    return provider;
}

/* EC2 IMDS Provider */

aws_credentials *get_credentials_fn_imds(struct aws_credentials_provider *provider) {
    aws_credentials *creds;
    int ret;
    struct aws_credentials_provider_imds *implementation = provider->implementation;

    flb_debug("[aws_credentials] Requesting credentials from the EC2 provider..");

    if (!implementation->credentials || time(NULL) > implementation->cred_refresh) {
        ret = get_creds_imds(implementation);
        if (ret < 0) {
            return NULL:
        }
    }

    creds = flb_malloc(sizeof(struct aws_credentials));
    if (!creds) {
        flb_errno();
        return NULL;
    }

    creds->access_key_id = flb_sds_create(implementation->credentials->access_key_id);
    if (!creds->access_key_id) {
        flb_errno();
        aws_credentials_destroy(creds);
        return NULL;
    }

    creds->secret_access_key = flb_sds_create(implementation->credentials->secret_access_key);
    if (!creds->secret_access_key) {
        flb_errno();
        aws_credentials_destroy(creds);
        return NULL;
    }

    if (implementation->credentials->session_token) {
        creds->session_token = flb_sds_create(implementation->credentials->session_token);
        if (!creds->session_token) {
            flb_errno();
            aws_credentials_destroy(creds);
            return NULL;
        }

    } else {
        creds->session_token = NULL;
    }

    return creds;
}

int refresh_fn_imds(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_imds *implementation = provider->implementation;
    flb_debug("[aws_credentials] Refresh called on the EC2 IMDS provider");
    return get_creds_imds(implementation);
}

void destroy_fn_imds(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_imds *implementation = provider->implementation;

    if (implementation) {
        if (implementation->credentials) {
            aws_credentials_destroy(implementation->credentials);
        }

        if (implementation->upstream) {
            flb_upstream_destroy(implementation->upstream);
        }

        if (implementation->imds_v2_token) {
            flb_sds_destroy(implementation->imds_v2_token);
        }

        flb_free(implementation);
        provider->implementation = NULL;
    }

    return;
}

static struct aws_credentials_provider_vtable imds_provider_vtable = {
    .get_credentials = get_credentials_fn_imds,
    .refresh = refresh_fn_imds,
    .destroy = destroy_fn_imds,
};

struct aws_credentials_provider *new_imds_provider(struct flb_config *config,
                                                   struct aws_http_client_generator
                                                   *generator)
{
    struct aws_credentials_provider_imds *implementation;
    struct aws_credentials_provider *provider;
    struct flb_upstream *upstream;

    provider = flb_calloc(1, sizeof(struct aws_credentials_provider));

    if (!provider) {
        flb_errno();
        return NULL;
    }

    implementation = flb_calloc(1, sizeof(struct aws_credentials_provider_imds));

    if (!implementation) {
        flb_free(provider);
        flb_errno();
        return NULL;
    }

    provider->provider_vtable = &imds_provider_vtable;
    provider->implementation = implementation;

    upstream = flb_upstream_create(config, AWS_IMDS_V2_HOST, 80,
                                   FLB_IO_TCP, NULL);
    if (!upstream) {
        aws_provider_destroy(provider);
        flb_error("[aws_credentials] EC2 IMDS: connection initialization error");
        return NULL;
    }

    implementation->client = generator->new();
    if (!implementation->client) {
        aws_provider_destroy(provider);
        flb_upstream_destroy(upstream);
        flb_error("[aws_credentials] EC2 IMDS: client creation error");
        return NULL;
    }
    implementation->client->name = "ec2_imds_provider_client";
    implementation->client->has_auth = FLB_FALSE;
    implementation->client->provider = NULL;
    implementation->client->region = NULL;
    implementation->client->service = NULL;
    implementation->client->port = 80
    implementation->client->flags = 0;
    implementation->client->proxy = NULL;
    implementation->client->upstream = upstream;

    return provider;
}


/* Requests creds from IMDS and sets them on the provider */
static int get_creds_imds(struct aws_credentials_provider_imds *implementation)
{
    int ret;
    flb_sds_t instance_role;
    size_t instance_role_len;
    char *cred_path;
    size_t cred_path_size;

    flb_debug("[aws_credentials] requesting credentials from EC2 IMDS");

    if (!implementation->imds_v2_token || (time(NULL) > implementation->token_refresh)) {
        flb_debug("[aws_credentials] requesting a new IMDSv2 token");
        ret = get_ec2_token(implementation->client,
                            &implementation->imds_v2_token,
                            &implementation->imds_v2_token_len);
        if (ret < 0) {
            return -1;
        }

        implementation->token_refresh = time(NULL)
                                        + AWS_IMDS_V2_TOKEN_TTL
                                        - FLB_AWS_REFRESH_WINDOW;
    }

    /* Get the name of the instance role */
    ret = get_metadata(implementation->client, AWS_IMDS_V2_ROLE_PATH,
                       &instance_role, &instance_role_len,
                       implementation->imds_v2_token,
                       implementation->imds_v2_token_len);

    if (ret < 0) {
        return -1;
    }

    flb_debug("[aws_credentials] Requesting credentials for instance role %s",
              instance_role);

    /* Construct path where we will find the credentials */
    cred_path_size = sizeof(char) * (AWS_IMDS_V2_ROLE_PATH_LEN + instance_role_len) + 1;
    cred_path = flb_malloc(cred_path_size);
    if (!cred_path) {
        flb_sds_destroy(instance_role);
        flb_errno();
        return -1;
    }

    ret = snprintf(cred_path, cred_path_size, "%s%s", AWS_IMDS_V2_ROLE_PATH, instance_role);
    if (ret < 0) {
        flb_sds_destroy(instance_role);
        flb_free(cred_path);
        flb_errno();
        return -1;
    }

    /* request creds */
    ret = imds_credentials_request(implementation, cred_path);

    flb_sds_destroy(instance_role);
    flb_free(cred_path);
    return ret;

}

static int imds_credentials_request(struct aws_credentials_provider_imds
                                    *implementation, char *cred_path)
{
    int ret;
    flb_sds_t credentials_response;
    size_t credentials_response_len;
    struct aws_credentials *creds;
    time_t expiration;

    ret = get_metadata(implementation->client, cred_path,
                       &credentials_response, &credentials_response_len,
                       implementation->imds_v2_token,
                       implementation->imds_v2_token_len);

    if (ret < 0) {
        return -1;
    }

    creds = process_http_credentials_response(credentials_response,
                                              credentials_response_len,
                                              &expiration);

    if (creds == NULL) {
        flb_sds_destroy(credentials_response);
        return -1;
    }
    implementation->credentials = creds;
    implementation->cred_refresh = expiration - FLB_AWS_REFRESH_WINDOW;

    flb_sds_destroy(credentials_response);
    return 0;
}

/*
 * HTTP Credentials Provider - retrieve credentials from a local http server
 * Used to implement the ECS Credentials provider.
 * Equivalent to:
 * https://github.com/aws/aws-sdk-go/tree/master/aws/credentials/endpointcreds
 */

aws_credentials *get_credentials_fn_http(struct aws_credentials_provider *provider)
{
    aws_credentials *creds;
    int ret;
    struct aws_credentials_provider_http *implementation = provider->implementation;

    flb_debug("[aws_credentials] Retrieving credentials from the HTTP provider..");

    if (!implementation->credentials || time(NULL) > implementation->cred_refresh) {
        ret = http_credentials_request(implementation);
        if (ret < 0) {
            return NULL:
        }
    }

    creds = flb_malloc(sizeof(struct aws_credentials));
    if (!creds) {
        goto error;
    }

    creds->access_key_id = flb_sds_create(implementation->credentials->access_key_id);
    if (!creds->access_key_id) {
        goto error;
    }

    creds->secret_access_key = flb_sds_create(implementation->credentials->secret_access_key);
    if (!creds->secret_access_key) {
        goto error;
    }

    if (implementation->credentials->session_token) {
        creds->session_token = flb_sds_create(implementation->credentials->session_token);
        if (!creds->session_token) {
            goto error;
        }

    } else {
        creds->session_token = NULL;
    }

    return creds;

error:
    flb_errno();
    aws_credentials_destroy(creds);
    return NULL;
}

int refresh_fn_http(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_http *implementation = provider->implementation;
    flb_debug("[aws_credentials] Refresh called on the http provider");
    return http_credentials_request(implementation);
}

void destroy_fn_http(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_http *implementation = provider->implementation;

    if (implementation) {
        if (implementation->credentials) {
            aws_credentials_destroy(implementation->credentials);
        }

        if (implementation->client) {
            aws_client_destroy(implementation->client);
        }

        if (implementation->host) {
            flb_free(implementation->host);
        }

        if (implementation->path) {
            flb_free(implementation->path);
        }

        flb_free(implementation);
        provider->implementation = NULL;
    }

    return;
}

static struct aws_credentials_provider_vtable http_provider_vtable = {
    .get_credentials = get_credentials_fn_http,
    .refresh = refresh_fn_http,
    .destroy = destroy_fn_http,
};

struct aws_credentials_provider *new_http_provider(struct flb_config *config,
                                                   char *host, char* path,
                                                   struct aws_http_client_generator
                                                   *generator)
{
    struct aws_credentials_provider_http *implementation;
    struct aws_credentials_provider *provider;
    struct flb_upstream *upstream;

    provider = flb_calloc(1, sizeof(struct aws_credentials_provider));

    if (!provider) {
        flb_errno();
        return NULL;
    }

    implementation = flb_calloc(1, sizeof(struct aws_credentials_provider_http));

    if (!implementation) {
        flb_free(provider);
        flb_errno();
        return NULL;
    }

    provider->provider_vtable = &http_provider_vtable;
    provider->implementation = implementation;

    implementation->host = host;
    implementation->path = path;

    upstream = flb_upstream_create(config, host, 80, FLB_IO_TCP, NULL);

    if (!upstream) {
        aws_provider_destroy(provider);
        flb_error("[aws_credentials] HTTP Provider: connection initialization "
                  "error");
        return NULL;
    }

    implementation->client = generator->new();
    if (!implementation->client) {
        aws_provider_destroy(provider);
        flb_upstream_destroy(upstream);
        flb_error("[aws_credentials] HTTP Provider: client creation error");
        return NULL;
    }
    implementation->client->name = "http_provider_client";
    implementation->client->has_auth = FLB_FALSE;
    implementation->client->provider = NULL;
    implementation->client->region = NULL;
    implementation->client->service = NULL;
    implementation->client->port = 80
    implementation->client->flags = 0;
    implementation->client->proxy = NULL;
    implementation->client->upstream = upstream;

    return provider;
}

static int http_credentials_request(struct aws_credentials_provider_http
                                    *implementation)
{
    flb_sds_t response;
    size_t response_len;
    time_t expiration;
    struct aws_credentials *creds;
    struct aws_http_client client = implementation->client;

    ret = client->client_vtable->request(client, FLB_HTTP_GET,
                                         implementation->path, NULL, 0,
                                         NULL, 0);

    if (ret != 0 || client->resp.status != 200) {
        return -1;
    }

    response = flb_sds_create_len(client->c->resp.payload,
                                  client->c->resp.payload_size);
    if (!response) {
        flb_errno();
        return -1;
    }

    response_len = client->c->resp.payload_size;

    creds = process_http_credentials_response(response, response_len,
                                              &expiration);

    flb_sds_destroy(response);

    if (!creds) {
        return -1;
    }

    implementation->credentials = creds;
    implementation->cred_refresh = expiration - FLB_AWS_REFRESH_WINDOW;
    return 0;
}

/*
 * ECS Provider
 * The ECS Provider is just a wrapper around the HTTP Provider
 * with the ECS credentials endpoint.
 */

struct aws_credentials_provider *new_ecs_provider(struct flb_config *config,
                                                  struct aws_http_client_generator
                                                  *generator)
{
    char *host;
    char *path;
    char *path_var;

    host = flb_malloc((ECS_CREDENTIALS_HOST_LEN + 1) * sizeof(char));
    if (!host) {
        flb_errno();
        return NULL;
    }

    memcpy(host, ECS_CREDENTIALS_HOST, ECS_CREDENTIALS_HOST_LEN);
    host[ECS_CREDENTIALS_HOST_LEN] = '\0';

    path_var = getenv(ECS_CREDENTIALS_PATH_ENV_VAR)
    if (path_var && strlen(path_var) > 0) {
        path = flb_malloc((strlen(path_var) + 1) * sizeof(char));
        if (!path) {
            flb_errno();
            flb_free(host);
            return NULL;
        }
        memcpy(path, path_var, strlen(path_var));
        path[strlen(path_var)] = '\0';

        return new_http_provider(config, host, path, generator);
    } else {
        flb_debug("[aws_credentials] Not initializing ECS Provider because"
                  " %s is not set", ECS_CREDENTIALS_PATH_ENV_VAR);
        return NULL;
    }

}


/*
 * All HTTP credentials endpoints (IMDS, ECS, custom) follow the same spec:
 * {
 *   "AccessKeyId": "ACCESS_KEY_ID",
 *   "Expiration": "2019-12-18T21:27:58Z",
 *   "SecretAccessKey": "SECRET_ACCESS_KEY",
 *   "Token": "SECURITY_TOKEN_STRING"
 * }
 * (some implementations (IMDS) have additional fields)
 * Returns NULL if any part of parsing was unsuccessful.
 */
static struct aws_credentials *process_http_credentials_response(flb_sds_t
                                                                 response,
                                                                 size_t
                                                                 response_len,
                                                                 time_t *expiration)
{
    jsmntok_t *tokens;
    const jsmntok_t *t;
    char *current_token;
    jsmn_parser parser;
    int tokens_size = 10;
    size_t size;
    int ret;
    struct aws_credentials *creds;
    int i = 0;

    jsmn_init(&parser);

    size = sizeof(jsmntok_t) * tokens_size;
    tokens = flb_calloc(1, size);
    if (!tokens) {
        flb_errno();
        return NULL;
    }

    ret = jsmn_parse(&parser, response, response_len,
                     tokens, tokens_size);

    if (ret == JSMN_ERROR_INVAL || ret == JSMN_ERROR_PART) {
        flb_free(tokens);
        return NULL;
    }

    /* return value is number of tokens parsed */
    tokens_size = ret;

    creds = flb_calloc(1, sizeof(struct aws_credentials));
    if (!creds) {
        flb_free(tokens);
        flb_errno();
        return NULL;
    }

    /*
     * jsmn will create an array of tokens like:
     * key, value, key, value
     */
    while (i < (tokens_size - 1)) {
        t = &tokens[i];

        if (t->start == -1 || t->end == -1 || (t->start == 0 && t->end == 0)) {
            break;
        }

        if (t->type == JSMN_STRING) {
            current_token = &response[t->start];
            response[t->end + 1] = '\0';

            if (strcmp(current_token, AWS_HTTP_RESPONSE_ACCESS_KEY) == 0) {
                i++;
                t = &tokens[i];
                current_token = &response[t->start];
                creds->access_key_id = flb_sds_create_len(current_token, t->size);
                if (!creds->access_key_id) {
                    flb_errno();
                    aws_credentials_destroy(creds);
                    flb_free(tokens);
                    return NULL;
                }
                continue;
            }
            if (strcmp(current_token, AWS_HTTP_RESPONSE_SECRET_KEY) == 0) {
                i++;
                t = &tokens[i];
                current_token = &response[t->start];
                creds->secret_access_key = flb_sds_create_len(current_token, t->size);
                if (!creds->secret_access_key) {
                    flb_errno();
                    aws_credentials_destroy(creds);
                    flb_free(tokens);
                    return NULL;
                }
                continue;
            }
            if (strcmp(current_token, AWS_HTTP_RESPONSE_TOKEN) == 0) {
                i++;
                t = &tokens[i];
                current_token = &response[t->start];
                creds->session_token = flb_sds_create_len(current_token, t->size);
                if (!creds->session_token) {
                    flb_errno();
                    aws_credentials_destroy(creds);
                    flb_free(tokens);
                    return NULL;
                }
                continue;
            }
            if (strcmp(current_token, AWS_HTTP_RESPONSE_EXPIRATION) == 0) {
                i++;
                t = &tokens[i];
                current_token = &response[t->start];
                response[t->end + 1] = '\0';
                *expiration = parse_expiration(current_token);
                if (*expiration == 0) {
                    flb_errno();
                    aws_credentials_destroy(creds);
                    flb_free(tokens);
                    return NULL;
                }
            }
        }

        i++;
    }

    flb_free(tokens);

    if (creds->access_key_id == NULL || creds->secret_access_key == NULL ||
        creds->session_token == NULL) {
        aws_credentials_destroy(creds);
        return NULL:
    }

    return creds;
}

time_t parse_expiration(const char* timestamp)
{
    struct tm tm = {0};
    time_t expiration;
    if ( strptime(timestamp, "%Y-%m-%dT%H:%M:%SZ", &tm) != NULL ) {
      expiration = timegm(&tm);
    } else {
        flb_debug("[aws_credentials] Could not parse expiration: %s", timestamp);
        return 0;
    }
}

void aws_credentials_destroy(struct aws_credentials *creds)
{
    if (creds) {
        if (creds->access_key_id) {
            flb_sds_destroy(creds->access_key_id);
        }
        if (creds->secret_access_key) {
            flb_sds_destroy(creds->secret_access_key);
        }
        if (creds->secret_access_key) {
            flb_sds_destroy(creds->session_token);
        }

        flb_free(creds);
    }
}

void aws_provider_destroy(struct aws_credentials_provider *provider))
{
    if (provider) {
        if (provider->implementation) {
            provider->provider_vtable->destroy(provider->implementation);
        }

        flb_free(provider);
    }
}

int file_to_buf(const char *path, char **out_buf, size_t *out_size)
{
    int ret;
    long bytes;
    char *buf;
    FILE *fp;
    struct stat st;

    ret = stat(path, &st);
    if (ret == -1) {
        return -1;
    }

    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    buf = flb_malloc(st.st_size + sizeof(char));
    if (!buf) {
        flb_errno();
        fclose(fp);
        return -1;
    }

    bytes = fread(buf, st.st_size, 1, fp);
    if (bytes != 1) {
        flb_errno();
        flb_free(buf);
        fclose(fp);
        return -1;
    }

    /* fread does not add null byte */
    buf[st.st_size] = '\0';

    fclose(fp);
    *out_buf = buf;
    *out_size = st.st_size;

    return 0;
}
