/* Copyright 2014-2015 Gregor Uhlenheuer
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "def.h"
#include "event.h"
#include "log.h"
#include "socket.h"

#include <errno.h>
#include <linux/cn_proc.h>
#include <linux/connector.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static volatile int need_exit = 0;

/**
 * Open netlink socket connection
 */
static int
netlink_connect(void)
{
    int rc;
    int netlink_socket;
    struct sockaddr_nl addr;

    netlink_socket = socket(
            PF_NETLINK,         /* kernel user interface device */
            SOCK_DGRAM,         /* datagram */
            NETLINK_CONNECTOR); /* netlink */

    if (netlink_socket == -1)
    {
        log_perror("nyx: socket");
        return -1;
    }

    /* initialize memory */
    memset(&addr, 0, sizeof(struct sockaddr_nl));

    addr.nl_family = AF_NETLINK;
    addr.nl_groups = CN_IDX_PROC;
    addr.nl_pid = getpid();

    rc = bind(netlink_socket, (struct sockaddr *)&addr, sizeof(addr));

    if (rc == -1)
    {
        log_perror("nyx: bind");
        close(netlink_socket);
        return -1;
    }

    return netlink_socket;
}

/**
 * Subscribe on process events
 */
static int
set_process_event_listen(int socket, int enable)
{
    int rc;

    struct __attribute__ ((aligned(NLMSG_ALIGNTO)))
    {
        struct nlmsghdr nl_hdr;
        struct __attribute__ ((__packed__))
        {
            struct cn_msg cn_msg;
            enum proc_cn_mcast_op cn_mcast;
        };
    } nlcn_msg;

    memset(&nlcn_msg, 0, sizeof(nlcn_msg));
    nlcn_msg.nl_hdr.nlmsg_len = sizeof(nlcn_msg);
    nlcn_msg.nl_hdr.nlmsg_pid = getpid();
    nlcn_msg.nl_hdr.nlmsg_type = NLMSG_DONE;

    /* connect to process events */
    nlcn_msg.cn_msg.id.idx = CN_IDX_PROC;
    nlcn_msg.cn_msg.id.val = CN_VAL_PROC;
    nlcn_msg.cn_msg.len = sizeof(enum proc_cn_mcast_op);

    /* either start or stop listening to events */
    nlcn_msg.cn_mcast =
        enable
        ? PROC_CN_MCAST_LISTEN
        : PROC_CN_MCAST_IGNORE;

    rc = send(socket, &nlcn_msg, sizeof(nlcn_msg), 0);

    if (rc == -1)
    {
        log_perror("nyx: send");
        return -1;
    }

    return 0;
}

static int
subscribe_event_listen(int socket)
{
    return set_process_event_listen(socket, 1);
}

static int
unsubscribe_event_listen(int socket)
{
    return set_process_event_listen(socket, 0);
}

static process_event_data_t *
new_event_data(void)
{
    process_event_data_t *data = xcalloc(1, sizeof(process_event_data_t));

    return data;
}

static int
set_event_data(process_event_data_t *data, struct proc_event *event)
{
    switch (event->what)
    {
        case PROC_EVENT_FORK:
            data->type = EVENT_FORK;

            data->fork.parent_pid = event->event_data.fork.parent_pid;
            data->fork.parent_thread_group_id = event->event_data.fork.parent_tgid;
            data->fork.child_pid = event->event_data.fork.child_pid;
            data->fork.child_thread_group_id = event->event_data.fork.child_tgid;

            log_debug("fork: parent tid=%d pid=%d -> child tid=%d pid=%d",
                    event->event_data.fork.parent_pid,
                    event->event_data.fork.parent_tgid,
                    event->event_data.fork.child_pid,
                    event->event_data.fork.child_tgid);

            return data->fork.parent_pid;

        case PROC_EVENT_EXIT:
            data->type = EVENT_EXIT;

            data->exit.pid = event->event_data.exit.process_pid;
            data->exit.exit_code = event->event_data.exit.exit_code;
            data->exit.exit_signal = event->event_data.exit.exit_signal;
            data->exit.thread_group_id = event->event_data.exit.process_tgid;

            log_debug("exit: tid=%d pid=%d exit_code=%d",
                    event->event_data.exit.process_pid,
                    event->event_data.exit.process_tgid,
                    event->event_data.exit.exit_code);

            return data->exit.pid;

        /* unhandled events */
        case PROC_EVENT_NONE:
        case PROC_EVENT_EXEC:
        case PROC_EVENT_UID:
        case PROC_EVENT_GID:
        default:
            break;
    }

    return 0;
}

static void handle_eventfd(struct epoll_event *event, nyx_t *nyx)
{
    int err = 0;
    uint64_t value = 0;

    log_debug("Received epoll event on eventfd interface (%d)", nyx->event);

    err = read(event->data.fd, &value, sizeof(value));

    if (err == -1)
        log_perror("nyx: read");

    need_exit = 1;
}

/**
 * Handle a single process event
 */
static int
handle_process_event(int nl_sock, nyx_t *nyx, process_handler_t handler)
{
    static int max_conn = 16;
    int pid = 0, rc = 0, epfd = 0;
    struct epoll_event ev;
    struct epoll_event *events = NULL;

    process_event_data_t *event_data = new_event_data();

    struct __attribute__ ((aligned(NLMSG_ALIGNTO)))
    {
        struct nlmsghdr nl_hdr;
        struct __attribute__ ((__packed__))
        {
            struct cn_msg cn_msg;
            struct proc_event proc_ev;
        };
    } nlcn_msg;

    log_debug("Starting event manager loop");

    /* initialize epoll */
    epfd = epoll_create(max_conn);
    if (epfd == -1)
    {
        log_perror("nyx: epoll_create");
        goto teardown;
    }

    if (!unblock_socket(nl_sock))
        goto teardown;

    if (!add_epoll_socket(nl_sock, &ev, epfd))
        goto teardown;

    /* add eventfd socket to epoll as well */
    if (nyx->event > 0)
    {
        if (!unblock_socket(nyx->event))
            goto teardown;

        if (!add_epoll_socket(nyx->event, &ev, epfd))
            goto teardown;
    }

    events = xcalloc(max_conn, sizeof(struct epoll_event));

    while (!need_exit)
    {
        int i = 0, n = 0;
        struct epoll_event *event = NULL;

        n = epoll_wait(epfd, events, max_conn, -1);

        for (i = 0, event = events; i < n; event++, i++)
        {
            int fd = event->data.fd;

            /* handle eventfd */
            if (fd == nyx->event)
            {
                handle_eventfd(event, nyx);
                rc = 1;
            }
            else
            {
                rc = recv(fd, &nlcn_msg, sizeof(nlcn_msg), 0);

                /* socket shutdown */
                if (rc == 0)
                {
                    rc = 1;
                    break;
                }
                else if (rc == -1)
                {
                    /* interrupted by a signal */
                    if (errno == EINTR)
                    {
                        rc = 1;
                        continue;
                    }

                    log_perror("nyx: recv");
                    break;
                }

                pid = set_event_data(event_data, &nlcn_msg.proc_ev);

                if (pid > 0)
                    handler(pid, event_data, nyx);
            }
        }
    }

teardown:
    if (event_data != NULL)
    {
        free(event_data);
        event_data = NULL;
    }

    if (events != NULL)
    {
        free(events);
        events = NULL;
    }

    if (epfd > 0)
        close(epfd);

    return rc;
}

static void
on_terminate(UNUSED int signum)
{
    log_debug("Caught termination signal - exiting event manager loop");
    need_exit = 1;
}

int
event_loop(nyx_t *nyx, process_handler_t handler)
{
    int socket;
    int rc = 1;

    socket = netlink_connect();
    if (socket == -1)
        return 0;

    rc = subscribe_event_listen(socket);
    if (rc == -1)
    {
        rc = 0;
        goto out;
    }

    /* register termination handler */
    setup_signals(nyx, on_terminate);

    /* start listening on process events */
    rc = handle_process_event(socket, nyx, handler);
    if (rc == -1)
    {
        rc = 0;
        goto out;
    }

    unsubscribe_event_listen(socket);

out:
    close(socket);
    return rc;
}

/* vim: set et sw=4 sts=4 tw=80: */
