#include "log.h"
#include "nyx.h"
#include "watch.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>

static void
_watch_destroy(void *watch)
{
    watch_destroy((watch_t *)watch);
}

nyx_t *
nyx_initialize(int argc, char **args)
{
    int arg = 0, index = 0;

    nyx_t *nyx = calloc(1, sizeof(nyx_t));

    if (nyx == NULL)
        log_critical_perror("nyx: calloc");

    /* parse command line arguments */
    while ((arg = getopt(argc, args, "qC")) != -1)
    {
        switch (arg)
        {
            case 'q':
                nyx->options.quiet = 1;
                break;
            case 'C':
                nyx->options.no_color = 1;
                break;
        }
    }

    for (index = optind; index < argc; )
    {
        /* TODO: support multiple config files */
        nyx->options.filename = args[index];
        break;
    }

    nyx->pid = getpid();
    nyx->watches = hash_new(8, _watch_destroy);

    /* the states will be deleted via the watches hash */
    nyx->states = list_new(NULL);

    log_init(nyx);

    return nyx;
}

void
nyx_destroy(nyx_t *nyx)
{
    hash_destroy(nyx->watches);
    list_destroy(nyx->states);

    free(nyx);
    nyx = NULL;
}

/* vim: set et sw=4 sts=4 tw=80: */
