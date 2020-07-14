/***
  This file is part of PulseAudio.

  Copyright (C) 2020 Microsoft

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.

***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>

#include <stdlib.h>
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <time.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/source.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>

/* defined in pulse/version.h */
#if PA_PROTOCOL_VERSION > 28
/* these used to be defined in pulsecore/macro.h */
typedef bool pa_bool_t;
#define FALSE ((pa_bool_t) 0)
#define TRUE (!FALSE)
#else
#endif

PA_MODULE_AUTHOR("Steve Pronovost");
PA_MODULE_DESCRIPTION("RDP Source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "source_name=<name for the source> "
        "source_properties=<properties for the source> ");

#define DEFAULT_SOURCE_NAME "RDP Source"
#define BLOCK_USEC 10000

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_source *source;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_usec_t failed_connect_time;

    int fd;

    pa_memchunk memchunk;
};

static const char* const valid_modargs[] = {
    "source_name",
    "source_properties",
    NULL
};

static int open_socket(struct userdata *u) {
    char *source_socket;
    int fd;
    int bytes;
    struct sockaddr_un s;

    if (u->fd != -1) {
        return 0;
    }

    if (u->failed_connect_time != 0) {
        if (pa_rtclock_now() - u->failed_connect_time < 1000000) {
            return -1;
        }
    }

    fd = socket(PF_LOCAL, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }

    memset(&s, 0, sizeof(s));
    s.sun_family = AF_UNIX;
    bytes = sizeof(s.sun_path) - 1;
    source_socket = getenv("PULSE_AUDIO_RDP_SOURCE");
    if (source_socket == NULL || source_socket[0] == '\0')
    {
        source_socket = "/tmp/PulseAudioRDPSource";
    }
    
    snprintf(s.sun_path, bytes, "%s", source_socket);
    pa_log("RDP Source - Trying to connect to %s", s.sun_path);

    if (connect(fd, (struct sockaddr *)&s,
                sizeof(struct sockaddr_un)) != 0) {
        u->failed_connect_time = pa_rtclock_now();
        pa_log("Connected failed");
        close(fd);
        return -1;
    }

    u->failed_connect_time = 0;
    pa_log("RDP Source - Connected to fd %d", fd);
    u->fd = fd;

    return 0;
}

static char* SourceMsgToString(int code) {
    switch(code)
    {
        case PA_SOURCE_MESSAGE_ADD_OUTPUT:                return "PA_SOURCE_MESSAGE_ADD_OUTPUT";
        case PA_SOURCE_MESSAGE_REMOVE_OUTPUT:             return "PA_SOURCE_MESSAGE_REMOVE_OUTPUT";
        case PA_SOURCE_MESSAGE_GET_VOLUME:                return "PA_SOURCE_MESSAGE_GET_VOLUME";
        case PA_SOURCE_MESSAGE_SET_SHARED_VOLUME:         return "PA_SOURCE_MESSAGE_SET_SHARED_VOLUME";
        case PA_SOURCE_MESSAGE_SET_VOLUME_SYNCED:         return "PA_SOURCE_MESSAGE_SET_VOLUME_SYNCED";
        case PA_SOURCE_MESSAGE_SET_VOLUME:                return "PA_SOURCE_MESSAGE_SET_VOLUME";
        case PA_SOURCE_MESSAGE_SYNC_VOLUMES:              return "PA_SOURCE_MESSAGE_SYNC_VOLUMES";
        case PA_SOURCE_MESSAGE_GET_MUTE:                  return "PA_SOURCE_MESSAGE_GET_MUTE";
        case PA_SOURCE_MESSAGE_SET_MUTE:                  return "PA_SOURCE_MESSAGE_SET_MUTE";
        case PA_SOURCE_MESSAGE_GET_LATENCY:               return "PA_SOURCE_MESSAGE_GET_LATENCY";
        case PA_SOURCE_MESSAGE_GET_REQUESTED_LATENCY:     return "PA_SOURCE_MESSAGE_GET_REQUESTED_LATENCY";
        case PA_SOURCE_MESSAGE_SET_STATE:                 return "PA_SOURCE_MESSAGE_SET_STATE";
        case PA_SOURCE_MESSAGE_SET_LATENCY_RANGE:         return "PA_SOURCE_MESSAGE_SET_LATENCY_RANGE";
        case PA_SOURCE_MESSAGE_GET_LATENCY_RANGE:         return "PA_SOURCE_MESSAGE_GET_LATENCY_RANGE";
        case PA_SOURCE_MESSAGE_SET_FIXED_LATENCY:         return "PA_SOURCE_MESSAGE_SET_FIXED_LATENCY";
        case PA_SOURCE_MESSAGE_GET_FIXED_LATENCY:         return "PA_SOURCE_MESSAGE_GET_FIXED_LATENCY";
        case PA_SOURCE_MESSAGE_GET_MAX_REWIND:            return "PA_SOURCE_MESSAGE_GET_MAX_REWIND";
        case PA_SOURCE_MESSAGE_SET_MAX_REWIND:            return "PA_SOURCE_MESSAGE_SET_MAX_REWIND";
        case PA_SOURCE_MESSAGE_UPDATE_VOLUME_AND_MUTE:    return "PA_SOURCE_MESSAGE_UPDATE_VOLUME_AND_MUTE";
        case PA_SOURCE_MESSAGE_SET_PORT_LATENCY_OFFSET:   return "PA_SOURCE_MESSAGE_SET_PORT_LATENCY_OFFSET";
    }

    return "unknown";
}

static int source_process_msg(pa_msgobject *o, int code, void *data,
                              int64_t offset, pa_memchunk *chunk) {

    struct userdata *u = PA_SOURCE(o)->userdata;

    pa_log_debug("source_process_msg: %s", SourceMsgToString(code));

    switch (code)
    {
        case PA_SOURCE_MESSAGE_GET_LATENCY:
            *((pa_usec_t*) data) = BLOCK_USEC;
            return 0;

        case PA_SOURCE_MESSAGE_SET_STATE:
        {
            switch (*(int*)(data))
            {
                case PA_SOURCE_RUNNING:
                    pa_log_debug("Source Running");
                    if (u->fd == -1) {
                        open_socket(u);
                    }
                    break;
                
                case PA_SOURCE_SUSPENDED:
                    pa_log_debug("Source Suspended");
                    if (u->fd != -1) {
                        close(u->fd);
                        u->fd = -1;
                    }
                    break;
            }
        }
    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;
    int read_type = 0;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);

    for (;;) {
        int ret;

        /* Try to read some data and pass it on to the source driver */
        if (u->source->thread_info.state == PA_SOURCE_RUNNING) {
            ssize_t l = 0;
            void *p;
            ssize_t bytes = (44100 / 100) * 2; // 10ms, 2 channel, 2bytes/channel

            pa_rtpoll_set_timer_relative(u->rtpoll, 1000);

            if (!u->memchunk.memblock) {
                u->memchunk.memblock = pa_memblock_new(u->core->mempool, bytes);
                u->memchunk.index = u->memchunk.length = 0;
            }

            pa_assert(pa_memblock_get_length(u->memchunk.memblock) > u->memchunk.index);

            p = pa_memblock_acquire(u->memchunk.memblock);

            u->memchunk.length = 0;
            while (bytes)
            {
                l = pa_read(u->fd, (uint8_t*) p + u->memchunk.index + l, bytes, &read_type);
                if (l<0)
                {
                    break;
                }
                else if (l==0)
                {
                    assert(false);
                    break;
                }
                else
                {
                    u->memchunk.length += l;
                    bytes -= l;
                } 
            }
            pa_memblock_release(u->memchunk.memblock);

            if (l < 0) {
                if (errno == EINTR)
                    continue;
                else if (errno != EAGAIN) {
                    pa_log("Failed to read data from FIFO: %s", pa_cstrerror(errno));
                    goto fail;
                }

            } else if (l != 0) {
                pa_source_post(u->source, &u->memchunk);
                u->memchunk.index += u->memchunk.length;

                if (u->memchunk.index >= pa_memblock_get_length(u->memchunk.memblock)) {
                    pa_memblock_unref(u->memchunk.memblock);
                    pa_memchunk_reset(&u->memchunk);
                }
            }
        } else {
            pa_rtpoll_set_timer_disabled(u->rtpoll);
        }

        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    pa_log_debug("Thread failed");

    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

int pa__init(pa_module*m) {
    struct userdata *u = NULL;
    pa_sample_spec ss = {PA_SAMPLE_S16NE, 44100, 1};
    pa_channel_map map = {1, {PA_CHANNEL_POSITION_MONO} };
    pa_modargs *ma = NULL;
    pa_source_new_data data;
    
    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll);

    pa_source_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_source_new_data_set_name(&data,
          pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME));
    pa_source_new_data_set_sample_spec(&data, &ss);
    pa_source_new_data_set_channel_map(&data, &map);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, DEFAULT_SOURCE_NAME);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract"); 

    if (pa_modargs_get_proplist(ma, "source_properties", data.proplist,
                                PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_source_new_data_done(&data);
        goto fail;
    }

    u->source = pa_source_new(m->core, &data, PA_SOURCE_LATENCY); 
    pa_source_new_data_done(&data);

    if (!u->source) {
        pa_log("Failed to create source object.");
        goto fail;
    }

    u->source->parent.process_msg = source_process_msg;
    u->source->userdata = u;

    pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
    pa_source_set_rtpoll(u->source, u->rtpoll);
    pa_source_set_fixed_latency(u->source, BLOCK_USEC);

    u->fd = -1;

    if (!(u->thread = pa_thread_new("rdp-source", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_source_put(u->source);

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma) {
        pa_modargs_free(ma);
    }

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_source_linked_by(u->source);
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata)) {
        return;
    }

    if (u->source) {
        pa_source_unlink(u->source);
    }

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN,
                          NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->source) {
        pa_source_unref(u->source);
    }

    if (u->rtpoll) {
        pa_rtpoll_free(u->rtpoll);
    }

    pa_xfree(u);
}
