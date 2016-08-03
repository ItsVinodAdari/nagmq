#include "config.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#define NSCORE 1
#include "nebcallbacks.h"
#include "nebmods.h"
#include "nebmodules.h"
#include "nebstructs.h"
#ifdef HAVE_ICINGA
#include "icinga.h"
#else
#include "nagios.h"
#endif
#include "broker.h"
#include "nagmq_common.h"
#include "jansson.h"
#include "objects.h"
#include "utlist.h"

NEB_API_VERSION(CURRENT_NEB_API_VERSION)
static void* nagmq_handle = NULL;
void* zmq_ctx;
nebmodule* handle;
extern void* pubext;
extern int daemon_mode;
extern int sigrestart;
json_t* config;
char *curve_publickey = NULL, *curve_privatekey = NULL, *curve_knownhosts = NULL;
int keyfile_refresh_interval = 60;

// This is defined in nagios's objects.c, it should match the current
// ABI version of the nagios nagmq is loaded into.
extern int __nagios_object_structure_version;

int nebmodule_deinit(int flags, int reason) {
    neb_deregister_module_callbacks(nagmq_handle);
    if (config)
        json_decref(config);
#ifdef HAVE_SHUTDOWN_COMMAND_FILE_WORKER
    shutdown_command_file_worker();
#endif
    return 0;
}

int input_reaper(int sd, int events, void* param) {
    while (1) {
        struct socket_list* sock_desc = (struct socket_list*)param;
        zmq_msg_t input;
        zmq_msg_init(&input);

        if (zmq_msg_recv(&input, sock_desc->sock, ZMQ_DONTWAIT) == -1) {
            if (errno == EAGAIN)
                break;
            else if (errno == EINTR)
                continue;
            logit(NSLOG_RUNTIME_WARNING,
                  TRUE,
                  "Error receiving message from %s socket: %s",
                  sock_desc->name,
                  zmq_strerror(errno));
            continue;
        }

        sock_desc->processing_fn(&input);
        zmq_msg_close(&input);
    }

    return 0;
}

struct socket_list* sockets = NULL;
extern iobroker_set* nagios_iobs;

void register_zmq_sock(void* sock, char* name) {
    struct socket_list* new_sock = calloc(1, sizeof(struct socket_list));
    new_sock->sock = sock;
    new_sock->fd = -1;
    new_sock->name = name;

    setup_sockmonitor(new_sock);
    LL_APPEND(sockets, new_sock);
}

void register_zmq_sock_for_poll(void* sock, const char* name,
        void (*processing_fn)(zmq_msg_t* msg)) {
    struct socket_list* new_sock = calloc(1, sizeof(struct socket_list));
    new_sock->sock = sock;
    new_sock->processing_fn = processing_fn;
    new_sock->name = name;

    size_t throwaway = sizeof(new_sock->fd);
    zmq_getsockopt(sock, ZMQ_FD, &new_sock->fd, &throwaway);
    iobroker_register(nagios_iobs, new_sock->fd, new_sock, input_reaper);

    setup_sockmonitor(new_sock);
    // Call the input_reaper once manually to clear out any
    // level-triggered polling problems.
    input_reaper(0, 0, sock);

    LL_APPEND(sockets, new_sock);
}

void close_all_zmq_sockets() {
    struct socket_list* cur_sock = NULL;
    LL_FOREACH(sockets, cur_sock) {
        if (cur_sock->fd >= 0) {
            iobroker_unregister(nagios_iobs, cur_sock->fd);
        }
        iobroker_unregister(nagios_iobs, cur_sock->mon_fd);
        zmq_close(cur_sock->mon_sock);
        if (zmq_close(cur_sock->sock)) {
            logit(NSLOG_RUNTIME_ERROR,
                  TRUE,
                  "Error closing NagMQ command socket: %s",
                  zmq_strerror(errno));
        }
        LL_DELETE(sockets, cur_sock);
    }
}

int handle_startup(int which, void* obj) {
    struct nebstruct_process_struct* ps = (struct nebstruct_process_struct*)obj;
    time_t now = ps->timestamp.tv_sec;
    int rc;

    switch (ps->type) {
        case NEBTYPE_PROCESS_EVENTLOOPSTART: {
            json_t *pubdef = NULL, *cmddef = NULL, *curvedef = NULL, *distdef = NULL;
            int numthreads = 1;

            log_debug_info(
                DEBUGL_PROCESS, DEBUGV_BASIC, "Initializing NagMQ in process %u\n", getpid());
            if (get_values(config,
                           "iothreads",
                           JSON_INTEGER,
                           0,
                           &numthreads,
                           "event_publisher",
                           JSON_OBJECT,
                           0,
                           &pubdef,
                           "command_processor",
                           JSON_OBJECT,
                           0,
                           &cmddef,
                           "check_distributor",
                           JSON_OBJECT,
                           0,
                           &distdef,
#if ZMQ_VERSION_MAJOR > 3
                           "curve",
                           JSON_OBJECT,
                           0,
                           &curvedef,
#endif
                           NULL) != 0) {
                logit(NSLOG_CONFIG_ERROR, TRUE, "Invalid parameters in NagMQ configuration");
                exit(1);
                return -1;
            }

            if (!pubdef && !cmddef && !distdef)
                return 0;

            zmq_ctx = zmq_init(numthreads);
            if (zmq_ctx == NULL) {
                logit(NSLOG_RUNTIME_ERROR, TRUE, "Error initialzing ZMQ: %s", zmq_strerror(errno));
                exit(1);
                return -1;
            }

#if ZMQ_VERSION_MAJOR > 3
            if (curvedef) {
                if (get_values(curvedef,
                               "publickey",
                               JSON_STRING,
                               1,
                               &curve_publickey,
                               "privatekey",
                               JSON_STRING,
                               1,
                               &curve_privatekey,
                               "clientkeyfile",
                               JSON_STRING,
                               0,
                               &curve_knownhosts,
                               NULL) != 0) {
                    logit(NSLOG_RUNTIME_ERROR,
                          TRUE,
                          "Error getting public/private key for NagMQ curve security");
                    exit(1);
                    return -1;
                }

                if (curve_knownhosts) {
                    pthread_t tid;

                    void* zapsock = zmq_socket(zmq_ctx, ZMQ_REP);
                    if (zapsock == NULL) {
                        logit(NSLOG_RUNTIME_ERROR,
                              TRUE,
                              "Error creating NagMQ authentication socket: %s",
                              zmq_strerror(errno));
                        exit(1);
                        return -1;
                    }

                    if (zmq_bind(zapsock, "inproc://zeromq.zap.01") != 0) {
                        logit(NSLOG_RUNTIME_ERROR,
                              TRUE,
                              "Error binding to NagMQ authentication endpoint: %s",
                              zmq_strerror(errno));
                        exit(1);
                        return -1;
                    }
                    int rc = pthread_create(&tid, NULL, zap_handler, zapsock);
                    if (rc != 0) {
                        logit(NSLOG_RUNTIME_ERROR,
                              TRUE,
                              "Error starting NagMQ authentication thread?: %s",
                              strerror(errno));
                        exit(1);
                        return -1;
                    }
                }
            }
#endif

            if (pubdef) {
                void* pubsock;
                if ((pubsock = getsock("event_publisher", ZMQ_PUB, pubdef)) == NULL) {
                    exit(1);
                    return -1;
                }
                register_zmq_sock(pubsock, "event publisher");
            }

            if (cmddef) {
                void* pullsock;
                if ((pullsock = getsock("command_processor", ZMQ_PULL, cmddef)) == NULL) {
                    exit(1);
                    return -1;
                }
                register_zmq_sock_for_poll(pullsock, "command processor", process_pull_msg);
            }

            break;
        }
        case NEBTYPE_PROCESS_EVENTLOOPEND:
            if (pubext) {
                struct payload* payload;
                payload = payload_new();
                payload_new_string(payload, "type", "eventloopend");
                payload_new_timestamp(payload, "timestamp", &ps->timestamp);
                payload_finalize(payload);
                process_payload(payload);
            }

            close_all_zmq_sockets();
            while ((rc = zmq_term(zmq_ctx)) != 0) {
                if (errno == EINTR) {
                    logit(NSLOG_RUNTIME_WARNING,
                          FALSE,
                          "NagMQ ZeroMQ context termination was interrupted");
                    continue;
                } else
                    break;
            }
            break;
    }
    return 0;
}

int nebmodule_init(int flags, char* localargs, nebmodule* lhandle) {
    json_error_t loaderr;
    handle = lhandle;

    if (__nagios_object_structure_version != CURRENT_OBJECT_STRUCTURE_VERSION) {
        logit(NSLOG_RUNTIME_ERROR,
              TRUE,
              "NagMQ is loaded into a version of nagios with a different ABI "
              "than it was compiled for! You need to recompile NagMQ against the current "
              "nagios headers!");
        return -1;
    }

    neb_set_module_info(handle, NEBMODULE_MODINFO_TITLE, "NagMQ");
    neb_set_module_info(handle, NEBMODULE_MODINFO_AUTHOR, "Jonathan Reams");
    neb_set_module_info(handle, NEBMODULE_MODINFO_VERSION, "1.5.1");
    neb_set_module_info(handle, NEBMODULE_MODINFO_LICENSE, "Apache v2");
    neb_set_module_info(
        handle, NEBMODULE_MODINFO_DESC, "Provides interface into Nagios via ZeroMQ");

    config = json_load_file(localargs, 0, &loaderr);
    if (config == NULL) {
        logit(NSLOG_RUNTIME_ERROR,
              TRUE,
              "Error loading NagMQ config: %s (at %d:%d)",
              loaderr.text,
              loaderr.line,
              loaderr.column);
        exit(1);
        return -1;
    }

    neb_register_callback(NEBCALLBACK_PROCESS_DATA, lhandle, 0, handle_startup);

    return 0;
}
