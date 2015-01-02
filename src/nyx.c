#include "def.h"
#include "fs.h"
#include "hash.h"
#include "log.h"
#include "nyx.h"
#include "state.h"
#include "watch.h"

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

static void
_watch_destroy(void *watch)
{
    watch_destroy((watch_t *)watch);
}

static void
_state_destroy(void *state)
{
    state_destroy((void *)state);
}

void
print_usage(FILE *out)
{
    fputs("usage: nyx [options] <file>\n", out);
}

void
print_help(void)
{
    print_usage(stdout);
    printf("\n"
           "Options:\n"
           "   -s  --syslog   (log into syslog)\n"
           "   -q  --quiet    (output error messages only)\n"
           "   -C  --no-color (no terminal coloring)\n"
           "   -h  --help     (print this help)\n");
    exit(EXIT_SUCCESS);
}

static const struct option long_options[] =
{
    { .name = "help",     .has_arg = 0, .flag = NULL, .val = 'h'},
    { .name = "no-color", .has_arg = 0, .flag = NULL, .val = 'C'},
    { .name = "quiet",    .has_arg = 0, .flag = NULL, .val = 'q'},
    { .name = "syslog",   .has_arg = 0, .flag = NULL, .val = 's'},
    { NULL }
};

static const char *pid_dir_defaults[] =
{
    "/var/run/nyx",
    "~/.nyx/pid",
    "/tmp/nyx/pid",
    NULL
};

static const char *
determine_pid_dir(void)
{
    const char **dir = pid_dir_defaults;

    while (*dir)
    {
        if (mkdir_p(*dir))
        {
            log_debug("Using '%s' as nyx PID directory", *dir);
            return *dir;
        }

        dir++;
    }

    log_error("Failed to determine a PID directory for nyx");

    return NULL;
}

static void
handle_child_stop(UNUSED int signum)
{
    pid_t pid;
    int last_errno = errno;

    log_debug("Received child stop signal - waiting for termination");

    /* wait for all child processes */
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
    {
        /* do nothing */
    }

    errno = last_errno;
}

void
setup_signals(nyx_t *nyx, void (*terminate_handler)(int))
{
    log_debug("Setting up signals");

    struct sigaction action =
    {
        .sa_flags = SA_NOCLDSTOP | SA_RESTART,
        .sa_handler = handle_child_stop
    };

    sigfillset(&action.sa_mask);

    /* register SIGCHLD handler */
    sigaction(SIGCHLD, &action, NULL);

    /* register handler for termination:
     * SIGTERM and SIGINT */
    action.sa_handler = terminate_handler;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);
}

nyx_t *
nyx_initialize(int argc, char **args)
{
    int arg = 0, index = 0;

    nyx_t *nyx = calloc(1, sizeof(nyx_t));

    if (nyx == NULL)
    {
        perror("nyx: calloc");
        exit(EXIT_FAILURE);
    }

    /* parse command line arguments */
    while ((arg = getopt_long(argc, args, "qCh", long_options, NULL)) != -1)
    {
        switch (arg)
        {
            case 'q':
                nyx->options.quiet = 1;
                break;
            case 's':
                nyx->options.syslog = 1;
                break;
            case 'C':
                nyx->options.no_color = 1;
                break;
            case 'h':
                free(nyx);
                print_help();
                break;
        }
    }

    /* process remaining arguments */
    for (index = optind; index < argc; )
    {
        /* TODO: support multiple config files */
        nyx->options.config_file = args[index];
        break;
    }

    log_init(nyx);

    /* set default options */
    nyx->options.def_start_timeout = 5;

    nyx->pid_dir = determine_pid_dir();

    if (nyx->pid_dir == NULL)
        return NULL;

    nyx->pid = getpid();
    nyx->is_init = nyx->pid == 1;
    nyx->watches = hash_new(8, _watch_destroy);
    nyx->states = list_new(_state_destroy);

    return nyx;
}

int
nyx_watches_init(nyx_t *nyx)
{
    int rc = 1, init = 0;
    void *data = NULL;
    hash_iter_t *iter = hash_iter_start(nyx->watches);

    while (hash_iter(iter, &data))
    {
        state_t *state = NULL;
        watch_t *watch = data;

        log_debug("Initialize watch '%s'", watch->name);

        /* create new state instance */
        state = state_new(watch, nyx);
        list_add(nyx->states, state);

        /* start a new thread for each state */
        state->thread = xcalloc(1, sizeof(pthread_t));

        /* create with default thread attributes */
        init = pthread_create(state->thread, NULL, state_loop_start, state);
        if (init != 0)
        {
            log_error("Failed to create thread, error: %d", init);
            rc = 0;
            break;
        }
    }

    free(iter);
    return rc;
}

void
nyx_destroy(nyx_t *nyx)
{
    log_debug("Tearing down nyx");

    if (nyx == NULL)
        return;

    list_destroy(nyx->states);
    hash_destroy(nyx->watches);

    free(nyx);
    nyx = NULL;
}

/* vim: set et sw=4 sts=4 tw=80: */
