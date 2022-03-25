/*
 * A simple RTSP server implementation using libevent [1].
 *
 * To obtain `audio.g711a` and `video.h264`:
 *
 * $ ffmpeg -i http://docs.evostream.com/sample_content/assets/bun33s.mp4
 * -acodec pcm_mulaw -f mulaw -ar 8000 -ac 1 audio.g711a -vcodec h264 -x264opts
 * aud=1 video.h264
 *
 * [1] https://libevent.org/
 */

#include <smolrtsp.h>

#include <smolrtsp-libevent.h>

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>

// G.711 A-Law, 8k sample rate, mono channel.
#include "media/audio.g711a.h"

// H.264 video with AUDs, 25 FPS.
#include "media/video.h264.h"

#define AUDIO_PCMU_PAYLOAD_TYPE  0
#define AUDIO_SAMPLE_RATE        8000
#define AUDIO_SAMPLES_PER_PACKET 160

#define VIDEO_PAYLOAD_TYPE 96 // dynamic PT
#define VIDEO_SAMPLE_RATE  90000
#define VIDEO_FPS          25

#define AUDIO_STREAM_ID 0
#define VIDEO_STREAM_ID 1

#define MAX_STREAMS 2

typedef struct {
    uint64_t session_id;
    SmolRTSP_RtpTransport *transport;
} Stream;

typedef struct {
    struct event_base *base;
    struct sockaddr_storage addr;
    size_t addr_len;
    Stream streams[MAX_STREAMS];
} Client;

declImpl(SmolRTSP_Controller, Client);

static void listener_cb(
    struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *sa,
    int socklen, void *ctx);
static void on_event_cb(struct bufferevent *bev, short events, void *ctx);
static void on_sigint_cb(evutil_socket_t sig, short events, void *ctx);

static int setup_transport(
    Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req,
    SmolRTSP_Transport *t);
static int setup_tcp(
    SmolRTSP_Context *ctx, SmolRTSP_Transport *t,
    SmolRTSP_TransportConfig config);
static int setup_udp(
    const struct sockaddr *addr, SmolRTSP_Context *ctx, SmolRTSP_Transport *t,
    SmolRTSP_TransportConfig config);

static void start_audio(Stream *stream);
static void start_video(Stream *stream);
static void send_nalu(
    SmolRTSP_NalTransport *t, uint32_t *timestamp, const uint8_t *nalu_start,
    const uint8_t *nalu_end);

int main(void) {
    srand(time(NULL));

    struct event_base *base;
    if ((base = event_base_new()) == NULL) {
        fputs("event_base_new failed.\n", stderr);
        return EXIT_FAILURE;
    }

    struct sockaddr_in sin = {
        .sin_family = AF_INET,
        .sin_port = htons(SMOLRTSP_DEFAULT_PORT),
    };

    struct evconnlistener *listener;
    if ((listener = evconnlistener_new_bind(
             base, listener_cb, (void *)base,
             LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_FREE, -1,
             (struct sockaddr *)&sin, sizeof sin)) == NULL) {
        fputs("evconnlistener_new_bind failed.\n", stderr);
        return EXIT_FAILURE;
    }

    struct event *sigint_handler;
    if ((sigint_handler =
             evsignal_new(base, SIGINT, on_sigint_cb, (void *)base)) == NULL) {
        fputs("evsignal_new failed.\n", stderr);
        return EXIT_FAILURE;
    }

    if (event_add(sigint_handler, NULL) < 0) {
        fputs("event_add failed.\n", stderr);
        return EXIT_FAILURE;
    }

    event_base_dispatch(base);

    evconnlistener_free(listener);
    event_free(sigint_handler);
    event_base_free(base);

    puts("Done.");
    return EXIT_SUCCESS;
}

static void listener_cb(
    struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *sa,
    int socklen, void *arg) {
    (void)listener;
    (void)fd;
    (void)socklen;

    struct event_base *base = arg;

    struct bufferevent *bev;
    if ((bev = bufferevent_socket_new(base, fd, BEV_OPT_CLOSE_ON_FREE)) ==
        NULL) {
        fputs("bufferevent_socket_new failed.\n", stderr);
        event_base_loopbreak(base);
        return;
    }

    Client *client = calloc(1, sizeof *client);
    assert(client);
    client->base = base;
    memcpy(&client->addr, sa, socklen);
    client->addr_len = socklen;

    SmolRTSP_Controller controller = DYN(Client, SmolRTSP_Controller, client);
    void *ctx = smolrtsp_libevent_ctx(controller);

    bufferevent_setcb(bev, smolrtsp_libevent_cb, NULL, on_event_cb, ctx);
    bufferevent_enable(bev, EV_READ | EV_WRITE);
}

static void on_event_cb(struct bufferevent *bev, short events, void *ctx) {
    if (events & BEV_EVENT_EOF) {
        puts("Connection closed.");
    } else if (events & BEV_EVENT_ERROR) {
        perror("Got an error on the connection");
    }

    bufferevent_free(bev);
    smolrtsp_libevent_ctx_free(ctx);
}

static void on_sigint_cb(evutil_socket_t sig, short events, void *ctx) {
    (void)sig;
    (void)events;

    struct event_base *base = ctx;

    puts("Caught an interrupt signal; exiting cleanly in two seconds.");

    struct timeval delay = {2, 0};
    event_base_loopexit(base, &delay);
}

static void Client_drop(VSelf) {
    VSELF(Client);
    free(self);
}

impl(SmolRTSP_Droppable, Client);

static void
Client_options(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);

    (void)self;
    (void)req;

    smolrtsp_header(
        ctx, SMOLRTSP_HEADER_PUBLIC, "DESCRIBE, SETUP, TEARDOWN, PLAY");
    smolrtsp_respond_ok(ctx);
}

static void
Client_describe(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);

    (void)self;
    (void)req;

    char sdp_buf[1024] = {0};
    SmolRTSP_Writer sdp = smolrtsp_string_writer(sdp_buf);
    ssize_t ret;

    // clang-format off
    SMOLRTSP_SDP_DESCRIBE(
        ret, sdp,

        (SMOLRTSP_SDP_VERSION, "0"),
        (SMOLRTSP_SDP_ORIGIN, "SmolRTSP 3855320066 3855320129 IN IP4 0.0.0.0"),
        (SMOLRTSP_SDP_SESSION_NAME, "SmolRTSP example"),
        (SMOLRTSP_SDP_CONNECTION, "IN IP4 0.0.0.0"),
        (SMOLRTSP_SDP_TIME, "0 0"),

        (SMOLRTSP_SDP_MEDIA, "audio 0 RTP/AVP %d", AUDIO_PCMU_PAYLOAD_TYPE),
        (SMOLRTSP_SDP_ATTR, "control:audio"),

        (SMOLRTSP_SDP_MEDIA, "video 0 RTP/AVP %d", VIDEO_PAYLOAD_TYPE),
        (SMOLRTSP_SDP_ATTR, "control:video"),
        (SMOLRTSP_SDP_ATTR, "rtpmap:%d H264/%" PRIu32, VIDEO_PAYLOAD_TYPE, VIDEO_SAMPLE_RATE),
        (SMOLRTSP_SDP_ATTR, "fmtp:%d packetization-mode=1", VIDEO_PAYLOAD_TYPE),
        (SMOLRTSP_SDP_ATTR, "framerate:%d", VIDEO_FPS)
    );
    // clang-format on

    assert(ret > 0);

    smolrtsp_header(ctx, SMOLRTSP_HEADER_CONTENT_TYPE, "application/sdp");
    smolrtsp_body(ctx, CharSlice99_from_str(sdp_buf));

    smolrtsp_respond_ok(ctx);
}

static void
Client_setup(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);

    SmolRTSP_Transport transport;
    if (setup_transport(self, ctx, req, &transport) == -1) {
        return;
    }

    const size_t stream_id =
        CharSlice99_primitive_ends_with(
            req->start_line.uri, CharSlice99_from_str("/audio"))
            ? AUDIO_STREAM_ID
            : VIDEO_STREAM_ID;
    Stream *stream = &self->streams[stream_id];

    const bool aggregate_control_requested = SmolRTSP_HeaderMap_contains_key(
        &req->header_map, SMOLRTSP_HEADER_SESSION);
    if (aggregate_control_requested) {
        uint64_t session_id;
        if (smolrtsp_scanf_header(
                &req->header_map, SMOLRTSP_HEADER_SESSION, "%" SCNu64,
                &session_id) != 1) {
            smolrtsp_respond(
                ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Malformed `Session'");
            return;
        }

        stream->session_id = session_id;
    } else {
        stream->session_id = (uint64_t)rand();
    }

    if (AUDIO_STREAM_ID == stream_id) {
        stream->transport = SmolRTSP_RtpTransport_new(
            transport, AUDIO_PCMU_PAYLOAD_TYPE, AUDIO_SAMPLE_RATE);
    } else {
        stream->transport = SmolRTSP_RtpTransport_new(
            transport, VIDEO_PAYLOAD_TYPE, VIDEO_SAMPLE_RATE);
    }

    smolrtsp_header(
        ctx, SMOLRTSP_HEADER_SESSION, "%" PRIu64, stream->session_id);

    smolrtsp_respond_ok(ctx);
}

static void
Client_play(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);

    uint64_t session_id;
    if (smolrtsp_scanf_header(
            &req->header_map, SMOLRTSP_HEADER_SESSION, "%" SCNu64,
            &session_id) != 1) {
        smolrtsp_respond(
            ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Malformed `Session'");
        return;
    }

    bool played = false;
    for (size_t i = 0; i < MAX_STREAMS; i++) {
        if (self->streams[i].session_id == session_id) {
            if (AUDIO_STREAM_ID == i) {
                start_audio(&self->streams[i]);
            } else {
                start_video(&self->streams[i]);
            }

            played = true;
        }
    }

    if (played) {
        smolrtsp_header(ctx, SMOLRTSP_HEADER_RANGE, "npt=now-");
        smolrtsp_respond_ok(ctx);
    } else {
        smolrtsp_respond(
            ctx, SMOLRTSP_STATUS_SESSION_NOT_FOUND, "Invalid Session ID");
    }
}

static void
Client_teardown(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);

    (void)self;
    (void)req;

    smolrtsp_respond_ok(ctx);
}

static void
Client_unknown(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);

    (void)self;
    (void)req;

    smolrtsp_respond(ctx, SMOLRTSP_STATUS_METHOD_NOT_ALLOWED, "Unknown method");
}

static void
Client_before(VSelf, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);

    (void)self;
    (void)ctx;

    printf(
        "%s %s CSeq=%" PRIu32 ".\n",
        CharSlice99_alloca_c_str(req->start_line.method),
        CharSlice99_alloca_c_str(req->start_line.uri), req->cseq);
}

static void Client_after(
    VSelf, ssize_t ret, SmolRTSP_Context *ctx, const SmolRTSP_Request *req) {
    VSELF(Client);

    (void)self;
    (void)ctx;
    (void)req;

    if (ret < 0) {
        perror("Failed to respond");
    }
}

impl(SmolRTSP_Controller, Client);

static int setup_transport(
    Client *self, SmolRTSP_Context *ctx, const SmolRTSP_Request *req,
    SmolRTSP_Transport *t) {
    CharSlice99 transport_val;
    const bool transport_found = SmolRTSP_HeaderMap_find(
        &req->header_map, SMOLRTSP_HEADER_TRANSPORT, &transport_val);
    if (!transport_found) {
        smolrtsp_respond(
            ctx, SMOLRTSP_STATUS_BAD_REQUEST, "`Transport' not present");
        return -1;
    }

    SmolRTSP_TransportConfig config;
    if (smolrtsp_parse_transport(&config, transport_val) == -1) {
        smolrtsp_respond(
            ctx, SMOLRTSP_STATUS_BAD_REQUEST, "Malformed `Transport'");
        return -1;
    }

    switch (config.lower) {
    case SmolRTSP_LowerTransport_TCP:
        if (setup_tcp(ctx, t, config) == -1) {
            smolrtsp_respond_internal_error(ctx);
            return -1;
        }
        break;
    case SmolRTSP_LowerTransport_UDP:
        if (setup_udp((const struct sockaddr *)&self->addr, ctx, t, config) ==
            -1) {
            smolrtsp_respond_internal_error(ctx);
            return -1;
        }
        break;
    }

    return 0;
}

static int setup_tcp(
    SmolRTSP_Context *ctx, SmolRTSP_Transport *t,
    SmolRTSP_TransportConfig config) {
    ifLet(config.interleaved, SmolRTSP_ChannelPair_Some, interleaved) {
        *t = smolrtsp_transport_tcp(
            SmolRTSP_Context_get_writer(ctx), interleaved->rtp_channel);

        smolrtsp_header(
            ctx, SMOLRTSP_HEADER_TRANSPORT,
            "RTP/AVP/TCP;unicast;interleaved=%" PRIu8 "-%" PRIu8,
            interleaved->rtp_channel, interleaved->rtcp_channel);
        return 0;
    }

    smolrtsp_respond(
        ctx, SMOLRTSP_STATUS_BAD_REQUEST, "`interleaved' not found");
    return -1;
}

static int setup_udp(
    const struct sockaddr *addr, SmolRTSP_Context *ctx, SmolRTSP_Transport *t,
    SmolRTSP_TransportConfig config) {

    ifLet(config.client_port, SmolRTSP_PortPair_Some, client_port) {
        int fd;
        if ((fd = smolrtsp_dgram_socket(
                 addr->sa_family, smolrtsp_sockaddr_get_ip(addr),
                 client_port->rtp_port)) == -1) {
            return -1;
        }

        *t = smolrtsp_transport_udp(fd);

        smolrtsp_header(
            ctx, SMOLRTSP_HEADER_TRANSPORT,
            "RTP/AVP/UDP;unicast;client_port=%" PRIu16 "-%" PRIu16,
            client_port->rtp_port, client_port->rtcp_port);
        return 0;
    }

    smolrtsp_respond(
        ctx, SMOLRTSP_STATUS_BAD_REQUEST, "`client_port' not found");
    return -1;
}

static void start_audio(Stream *stream) {
    for (size_t i = 0; i * AUDIO_SAMPLES_PER_PACKET < ___media_audio_g711a_len;
         i++) {
        const SmolRTSP_RtpTimestamp ts =
            SmolRTSP_RtpTimestamp_Raw(i * AUDIO_SAMPLES_PER_PACKET);
        const bool marker = false;
        const size_t samples_count =
            ___media_audio_g711a_len <
                    i * AUDIO_SAMPLES_PER_PACKET + AUDIO_SAMPLES_PER_PACKET
                ? ___media_audio_g711a_len % AUDIO_SAMPLES_PER_PACKET
                : AUDIO_SAMPLES_PER_PACKET;
        const U8Slice99 header = U8Slice99_empty(),
                        payload = U8Slice99_new(
                            ___media_audio_g711a + i * AUDIO_SAMPLES_PER_PACKET,
                            samples_count);

        if (SmolRTSP_RtpTransport_send_packet(
                stream->transport, ts, marker, header, payload) == -1) {
            perror("Failed to send RTP/PCMU");
        }
    }

    VTABLE(SmolRTSP_RtpTransport, SmolRTSP_Droppable).drop(stream->transport);
}

static void start_video(Stream *stream) {
    SmolRTSP_NalTransport *transport =
        SmolRTSP_NalTransport_new(stream->transport);

    U8Slice99 video = Slice99_typed_from_array(___media_video_h264);
    uint8_t *nalu_start = NULL;

    uint32_t timestamp = 0;

    SmolRTSP_NalStartCodeTester start_code_tester;
    if ((start_code_tester = smolrtsp_determine_start_code(video)) == NULL) {
        fputs("Invalid video file.\n", stderr);
        abort();
    }

    while (!U8Slice99_is_empty(video)) {
        const size_t start_code_len = start_code_tester(video);
        if (0 == start_code_len) {
            video = U8Slice99_advance(video, 1);
            continue;
        }

        if (NULL != nalu_start) {
            send_nalu(transport, &timestamp, nalu_start, video.ptr);
        }

        video = U8Slice99_advance(video, start_code_len);
        nalu_start = video.ptr;
    }

    send_nalu(transport, &timestamp, nalu_start, video.ptr);

    VTABLE(SmolRTSP_NalTransport, SmolRTSP_Droppable).drop(transport);
}

static void send_nalu(
    SmolRTSP_NalTransport *t, uint32_t *timestamp, const uint8_t *nalu_start,
    const uint8_t *nalu_end) {
    const SmolRTSP_NalUnit nalu = {
        .header = SmolRTSP_NalHeader_H264(
            SmolRTSP_H264NalHeader_parse(nalu_start[0])),
        .payload = U8Slice99_from_ptrdiff(
            (uint8_t *)nalu_start + 1, (uint8_t *)nalu_end),
    };

    if (SmolRTSP_NalHeader_unit_type(nalu.header) ==
        SMOLRTSP_H264_NAL_UNIT_AUD) {
        *timestamp += VIDEO_SAMPLE_RATE / VIDEO_FPS;
    }

    if (SmolRTSP_NalTransport_send_packet(
            t, SmolRTSP_RtpTimestamp_Raw(*timestamp), nalu) == -1) {
        perror("Failed to send RTP/NAL");
    }
}
