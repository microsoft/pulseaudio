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
#include <pulsecore/sink.h>
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
PA_MODULE_DESCRIPTION("RDP Sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "sink_name=<name for the sink> "
        "sink_properties=<properties for the sink> ");

#define DEFAULT_SINK_NAME "RDP Sink"
#define BLOCK_USEC 10000

#define RDP_SINK_INTERFACE_VERSION 1

#define RDP_AUDIO_CMD_VERSION 0
#define RDP_AUDIO_CMD_TRANSFER 1
#define RDP_AUDIO_CMD_GET_LATENCY 2
#define RDP_AUDIO_CMD_RESET_LATENCY 3

typedef struct _rdp_audio_cmd_header
{
    uint32_t cmd;
    union {
        uint32_t version;
        struct {
            uint32_t bytes;
            uint64_t timestamp;
        } transfer;
        uint64_t reserved[8];
    };
} rdp_audio_cmd_header;

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_usec_t requested_latency;
    pa_usec_t timestamp;
    pa_usec_t failed_connect_time;

    int fd;
    int RDPSinkVersion;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "sink_properties",
    NULL
};

static uint64_t rdp_audio_timestamp() {
    struct timespec time;
    clock_gettime(CLOCK_REALTIME, &time);
    return time.tv_sec * 1000000 + time.tv_nsec / 1000;
}

static int lsend(int fd, char *data, int bytes) {
    int sent = 0;
    int error;
    while (sent < bytes) {
        error = send(fd, data + sent, bytes - sent, 0);
        if (error < 1) {
            return error;
        }
        sent += error;
    }
    return sent;
}

static int open_socket(struct userdata *u) {
    char *sink_socket;
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
    sink_socket = getenv("PULSE_AUDIO_RDP_SINK");
    if (sink_socket == NULL || sink_socket[0] == '\0')
    {
        sink_socket = "/tmp/PulseAudioRDPSink";
    }
    
    snprintf(s.sun_path, bytes, "%s", sink_socket);
    pa_log("RDP Sink - Trying to connect to %s", s.sun_path);

    if (connect(fd, (struct sockaddr *)&s,
                sizeof(struct sockaddr_un)) != 0) {
        u->failed_connect_time = pa_rtclock_now();
        pa_log("Connected failed");
        close(fd);
        return -1;
    }

    rdp_audio_cmd_header header;
    header.cmd = RDP_AUDIO_CMD_VERSION;
    header.version = RDP_SINK_INTERFACE_VERSION;
    lsend(fd, (char*)&header, sizeof(header));

    bytes = read(fd, &u->RDPSinkVersion, sizeof(u->RDPSinkVersion));
    if (bytes != sizeof(u->RDPSinkVersion)) {
        u->failed_connect_time = pa_rtclock_now();
        pa_log("Failed to exchange version information with RDPSink");
        close(fd);
        return -1;
    }

    u->failed_connect_time = 0;
    pa_log("RDP Sink - Connected to fd %d", fd);
    u->fd = fd;

    return 0;
}

static uint32_t get_latency(struct userdata *u) {
    rdp_audio_cmd_header header;
    uint32_t latency;
    int bytes;

    if (open_socket(u) < 0) {
        return 0;
    }

    header.cmd = RDP_AUDIO_CMD_GET_LATENCY;
    lsend(u->fd, (char*)&header, sizeof(header));

    bytes = read(u->fd, &latency, sizeof(latency));
    if (bytes != sizeof(latency)) {
        return 0;
    }
    return latency;
}

static void reset_latency(struct userdata *u) {
    rdp_audio_cmd_header header;

    if (open_socket(u) < 0) {
        return;
    }

    header.cmd = RDP_AUDIO_CMD_RESET_LATENCY;
    lsend(u->fd, (char*)&header, sizeof(header));
}

static int data_send(struct userdata *u, pa_memchunk *chunk) {
    char *data;
    int bytes;
    int sent;
    rdp_audio_cmd_header header;

    if (open_socket(u) < 0)
        return 0;

    bytes = chunk->length;
    pa_log_debug("bytes %d", bytes);

    header.cmd = RDP_AUDIO_CMD_TRANSFER;
    header.transfer.bytes = chunk->length;
    header.transfer.timestamp = rdp_audio_timestamp();
    lsend(u->fd, (char*)&header, sizeof(header));

    data = (char*)pa_memblock_acquire(chunk->memblock);
    data += chunk->index;
    sent = lsend(u->fd, data, bytes);
    pa_memblock_release(chunk->memblock);

    if (sent != bytes) {
        pa_log("data_send: send failed sent %d bytes %d", sent, bytes);
        close(u->fd);
        u->fd = -1;
        return 0;
    }

    return sent;
}

static char* SinkMsgToString(int code) {
    switch(code)
    {
        case PA_SINK_MESSAGE_ADD_INPUT:                 return "PA_SINK_MESSAGE_ADD_INPUT";
        case PA_SINK_MESSAGE_REMOVE_INPUT:              return "PA_SINK_MESSAGE_REMOVE_INPUT";
        case PA_SINK_MESSAGE_GET_VOLUME:                return "PA_SINK_MESSAGE_GET_VOLUME";
        case PA_SINK_MESSAGE_SET_SHARED_VOLUME:         return "PA_SINK_MESSAGE_SET_SHARED_VOLUME";
        case PA_SINK_MESSAGE_SET_VOLUME_SYNCED:         return "PA_SINK_MESSAGE_SET_VOLUME_SYNCED";
        case PA_SINK_MESSAGE_SET_VOLUME:                return "PA_SINK_MESSAGE_SET_VOLUME";
        case PA_SINK_MESSAGE_SYNC_VOLUMES:              return "PA_SINK_MESSAGE_SYNC_VOLUMES";
        case PA_SINK_MESSAGE_GET_MUTE:                  return "PA_SINK_MESSAGE_GET_MUTE";
        case PA_SINK_MESSAGE_SET_MUTE:                  return "PA_SINK_MESSAGE_SET_MUTE";
        case PA_SINK_MESSAGE_GET_LATENCY:               return "PA_SINK_MESSAGE_GET_LATENCY";
        case PA_SINK_MESSAGE_GET_REQUESTED_LATENCY:     return "PA_SINK_MESSAGE_GET_REQUESTED_LATENCY";
        case PA_SINK_MESSAGE_SET_STATE:                 return "PA_SINK_MESSAGE_SET_STATE";
        case PA_SINK_MESSAGE_START_MOVE:                return "PA_SINK_MESSAGE_START_MOVE";
        case PA_SINK_MESSAGE_FINISH_MOVE:               return "PA_SINK_MESSAGE_FINISH_MOVE";
        case PA_SINK_MESSAGE_SET_LATENCY_RANGE:         return "PA_SINK_MESSAGE_SET_LATENCY_RANGE";
        case PA_SINK_MESSAGE_GET_LATENCY_RANGE:         return "PA_SINK_MESSAGE_GET_LATENCY_RANGE";
        case PA_SINK_MESSAGE_SET_FIXED_LATENCY:         return "PA_SINK_MESSAGE_SET_FIXED_LATENCY";
        case PA_SINK_MESSAGE_GET_FIXED_LATENCY:         return "PA_SINK_MESSAGE_GET_FIXED_LATENCY";
        case PA_SINK_MESSAGE_GET_MAX_REWIND:            return "PA_SINK_MESSAGE_GET_MAX_REWIND";
        case PA_SINK_MESSAGE_GET_MAX_REQUEST:           return "PA_SINK_MESSAGE_GET_MAX_REQUEST";
        case PA_SINK_MESSAGE_SET_MAX_REWIND:            return "PA_SINK_MESSAGE_SET_MAX_REWIND";
        case PA_SINK_MESSAGE_SET_MAX_REQUEST:           return "PA_SINK_MESSAGE_SET_MAX_REQUEST";
        case PA_SINK_MESSAGE_UPDATE_VOLUME_AND_MUTE:    return "PA_SINK_MESSAGE_UPDATE_VOLUME_AND_MUTE";
        case PA_SINK_MESSAGE_SET_PORT_LATENCY_OFFSET:   return "PA_SINK_MESSAGE_SET_PORT_LATENCY_OFFSET";
    }

    return "unknown";
}

static int sink_process_msg(pa_msgobject *o, int code, void *data,
                            int64_t offset, pa_memchunk *chunk) {

    struct userdata *u = PA_SINK(o)->userdata;
    uint32_t latency;

    pa_log_debug("sink_process_msg: %s", SinkMsgToString(code));

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY:
            latency = get_latency(u);
            *((pa_usec_t*) data) = (pa_usec_t)latency;
            pa_log_debug("Current Latency: %d us", latency);
            return 0;
        
        case PA_SINK_MESSAGE_SET_STATE:
            switch (*(int*)(data))
            {
                case PA_SINK_IDLE:
                case PA_SINK_SUSPENDED:
                    reset_latency(u);
                    pa_log_debug("Reset latency");
                    break;
            }
            break;
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u;
    size_t nbytes;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    u->requested_latency = pa_sink_get_requested_latency_within_thread(s);

    if (u->requested_latency == (pa_usec_t) -1) {
        u->requested_latency = BLOCK_USEC;
    }
    pa_log_debug("Requested lantency (%lu)us", u->requested_latency);
    nbytes = pa_usec_to_bytes(u->requested_latency, &s->sample_spec);
    pa_sink_set_max_request_within_thread(s, nbytes);
}

static void process_render(struct userdata *u, pa_usec_t now) {
    pa_memchunk chunk;
    int request_bytes;

    pa_assert(u);
    while (u->timestamp < now + u->requested_latency) {
        request_bytes = u->sink->thread_info.max_request;
        request_bytes = MIN(request_bytes, 16 * 1024);
        pa_sink_render(u->sink, request_bytes, &chunk);
        if (u->sink->thread_info.state == PA_SINK_RUNNING) {
            data_send(u, &chunk);
        }
        pa_memblock_unref(chunk.memblock);
        u->timestamp += pa_bytes_to_usec(chunk.length, &u->sink->sample_spec);
    }
}

static void thread_func(void *userdata) {

    struct userdata *u = userdata;

    pa_assert(u);

    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);

    u->timestamp = pa_rtclock_now();

    for (;;) {
        pa_usec_t now = 0;
        int ret;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            now = pa_rtclock_now();
        }
        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested)) {
            pa_sink_process_rewind(u->sink, 0);
        }
        /* Render some data and write it to the socket */
        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            if (u->timestamp <= now) {
                process_render(u, now);
            }
            
            pa_rtpoll_set_timer_absolute(u->rtpoll, u->timestamp);
        } else {
            pa_rtpoll_set_timer_disabled(u->rtpoll);
        }

        /* Hmm, nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0) {
            goto fail;
        }

        if (ret == 0) {
            goto finish;
        }
    }
fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core),
                      PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0,
                      NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

int pa__init(pa_module*m) {
    struct userdata *u = NULL;
    pa_sample_spec ss = {PA_SAMPLE_S16NE, 44100, 2};
    pa_channel_map map = {2, {PA_CHANNEL_POSITION_LEFT, PA_CHANNEL_POSITION_RIGHT}};
    pa_modargs *ma = NULL;
    pa_sink_new_data data;
    size_t nbytes;

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

    pa_sink_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_sink_new_data_set_name(&data,
          pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME));
    pa_sink_new_data_set_sample_spec(&data, &ss);
    pa_sink_new_data_set_channel_map(&data, &map);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, DEFAULT_SINK_NAME);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract"); 

    if (pa_modargs_get_proplist(ma, "sink_properties", data.proplist,
                                PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&data);
        goto fail;
    }

    u->sink = pa_sink_new(m->core, &data,
                          PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY);
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create sink object.");
        goto fail;
    }

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    u->requested_latency = BLOCK_USEC;
    nbytes = pa_usec_to_bytes(u->requested_latency, &u->sink->sample_spec);
    pa_sink_set_max_request(u->sink, nbytes);

    u->fd = -1;

    if (!(u->thread = pa_thread_new("rdp-sink", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_sink_put(u->sink);

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

    return pa_sink_linked_by(u->sink);
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata)) {
        return;
    }

    if (u->sink) {
        pa_sink_unlink(u->sink);
    }

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN,
                          NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink) {
        pa_sink_unref(u->sink);
    }

    if (u->rtpoll) {
        pa_rtpoll_free(u->rtpoll);
    }

    pa_xfree(u);
}
