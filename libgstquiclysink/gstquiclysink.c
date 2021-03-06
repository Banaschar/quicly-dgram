/* GStreamer
 * Copyright (C) 2019 FIXME <fixme@example.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstquiclysink
 *
 * The quiclysink element is a server sink using the quic protocol
 * over the underlying modified quicly implementation.
 * Quicly is modified to support unreliable datagrams according to
 * "https://tools.ietf.org/html/draft-pauly-quic-datagram-05"
 * It supports both regular quic streams and unreliable datagrams
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v fakesrc ! quiclysink cert=Path-to-certificate key=Path-to-key
 * ]|
 * Creates a server pipeline to connect to. There is no certificate validation
 *
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>
//#include <gst/net/gstnetaddressmeta.h>

#include <sys/socket.h>
#include <sys/types.h>
#include "quicly.h"
#include "quicly/defaults.h"
#include "quicly/streambuf.h"
#include "../deps/picotls/t/util.h"

#include "gstquiclysink.h"

// quicly stuff
GST_DEBUG_CATEGORY_STATIC (gst_quiclysink_debug_category);
#define GST_CAT_DEFAULT gst_quiclysink_debug_category

/* rtp header */
typedef struct {
    uint8_t ver_p_x_cc;
    uint8_t m_pt;
    uint16_t seq_nr;
    uint32_t timestamp;
    uint32_t ssrc;
} rtp_hdr;

/* prototypes */
static void gst_quiclysink_set_property (GObject * object,
    guint property_id, const GValue * value, GParamSpec * pspec);
static void gst_quiclysink_get_property (GObject * object,
    guint property_id, GValue * value, GParamSpec * pspec);
static void gst_quiclysink_dispose (GObject * object);
static void gst_quiclysink_finalize (GObject * object);
static gboolean gst_quiclysink_sched_cbs(GstQuiclysink *quiclysink, GstClock *clock);
gboolean receive_async_cb(GstClock *clock, GstClockTime t, GstClockID id, gpointer data);
gboolean emit_feedback_signal_cb(GstClock *clock, GstClockTime t, GstClockID id, gpointer data);
gboolean gst_quiclysink_set_clock(GstElement *element, GstClock *clock);

static gboolean gst_quiclysink_set_caps (GstBaseSink * sink, GstCaps * caps);
static gboolean gst_quiclysink_start (GstBaseSink * sink);
static gboolean gst_quiclysink_stop (GstBaseSink * sink);
static gboolean gst_quiclysink_event (GstBaseSink * sink, GstEvent * event);
static GstFlowReturn gst_quiclysink_render (GstBaseSink * sink,
    GstBuffer * buffer);
static GstFlowReturn gst_quiclysink_render_list (GstBaseSink * bsink,
    GstBufferList * buffer_list);

static int save_ticket_cb(ptls_save_ticket_t *_self, ptls_t *tls, ptls_iovec_t src);
static int on_client_hello_cb(ptls_on_client_hello_t *_self, ptls_t *tls, ptls_on_client_hello_parameters_t *params);

static void on_closed_by_peer(quicly_closed_by_peer_t *self, quicly_conn_t *conn, int err, uint64_t frame_type, const char *reason,
                              size_t reason_len);
static int on_dgram_open(quicly_dgram_open_t *self, quicly_dgram_t *dgram);
static int on_stream_open(quicly_stream_open_t *self, quicly_stream_t *stream);
static int on_stop_sending(quicly_stream_t *stream, int err);
static int on_receive_dgram(quicly_dgram_t *dgram, const void *src, size_t len);
static int on_receive_stream(quicly_stream_t *stream, size_t off, const void *src, size_t len);
static int on_receive_reset(quicly_stream_t *stream, int err);
static int send_pending(GstQuiclysink *quiclysink, guint num);
static int receive_packet(GstQuiclysink *quiclysink);
static void write_dgram_buffer(quicly_dgram_t *dgram, const void *src, size_t len, gint64 max_time);
static GstStructure *gst_quiclysink_create_stats(GstQuiclysink *quiclysink);

static const char *session_file = NULL;

/* Quicly features we don't really need */
/*
static struct {
    ptls_aead_context_t *enc, *dec;
} address_token_aead;
static int on_generate_resumption_token(quicly_generate_resumption_token_t *self, quicly_conn_t *conn, ptls_buffer_t *buf,
                                        quicly_address_token_plaintext_t *token)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK (*quicly_get_data(conn));
  return quicly_encrypt_address_token(quiclysink->tlsctx.random_bytes, address_token_aead.enc, buf, buf->off, token);
}

static int save_resumption_token_cb(quicly_save_resumption_token_t *_self, quicly_conn_t *conn, ptls_iovec_t token)
{
  free(session_info.address_token.base);
  session_info.address_token = ptls_iovec_init(malloc(token.len), token.len);
  memcpy(session_info.address_token.base, token.base, token.len);

  return save_session(quicly_get_peer_transport_parameters(conn));
}

static quicly_generate_resumption_token_t generate_resumption_token = {&on_generate_resumption_token};
static quicly_save_resumption_token_t save_resumption_token = {save_resumption_token_cb};
*/

/* cb */
static ptls_save_ticket_t save_ticket = {save_ticket_cb};
static ptls_on_client_hello_t on_client_hello = {on_client_hello_cb};


static quicly_dgram_open_t dgram_open = {&on_dgram_open};
static quicly_stream_open_t stream_open = {&on_stream_open};
static quicly_closed_by_peer_t closed_by_peer = {&on_closed_by_peer};

static const quicly_stream_callbacks_t stream_callbacks = {quicly_streambuf_destroy,
                                                           quicly_streambuf_egress_shift,
                                                           quicly_streambuf_egress_emit,
                                                           on_stop_sending,
                                                           on_receive_stream,
                                                           on_receive_reset};
static const quicly_dgram_callbacks_t dgram_callbacks = {quicly_dgrambuf_destroy,
                                                         quicly_dgrambuf_egress_shift,
                                                         quicly_dgrambuf_egress_emit,
                                                         on_receive_dgram};

#define UDP_DEFAULT_BIND_ADDRESS  "0.0.0.0"
#define UDP_DEFAULT_BIND_PORT     5000
#define QUICLY_DEFAULT_MTU        1280
#define DEFAULT_CERTIFICATE       NULL
#define DEFAULT_PRIVATE_KEY       NULL
#define DEFAULT_STREAM_MODE       FALSE
#define DEFAULT_AUTO_CAPS_EXCHANGE FALSE
#define RECEIVE_CLOCK_TIME_NS     2000000
#define FEEDBACK_TIME_INTERVAL_NS 20000000
#define DEFAULT_APPLICATION_CC    FALSE
#define DEFAULT_FEEDBACK          FALSE
#define DEFAULT_DROP_LATE         -1
#define DEFAULT_SEND_BUFFER       16

/* properties */
enum
{
  PROP_0,
  PROP_BIND_ADDRESS,
  PROP_BIND_PORT,
  PROP_CERTIFICATE,
  PROP_PRIVATE_KEY,
  PROP_QUICLY_MTU,
  PROP_STREAM_MODE,
  PROP_MULTI_STREAM_MODE,
  PROP_STATS,
  PROP_AUTO_CAPS_EXCHANGE,
  PROP_APPLICATION_CC,
  PROP_FEEDBACK,
  PROP_DROP_LATE
};

/* signals */
enum
{
  SIGNAL_ON_FEEDBACK_REPORT,
  LAST_SIGNAL
};

static guint quiclysink_signals[LAST_SIGNAL] = {0}; 

/* pad templates */

static GstStaticPadTemplate gst_quiclysink_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstQuiclysink, gst_quiclysink, GST_TYPE_BASE_SINK,
  GST_DEBUG_CATEGORY_INIT (gst_quiclysink_debug_category, "quiclysink", 0,
  "debug category for quiclysink element"));

static void
gst_quiclysink_class_init (GstQuiclysinkClass * klass)
{
  //GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GObjectClass *gobject_class = (GObjectClass *)klass;
  GstBaseSinkClass *base_sink_class = GST_BASE_SINK_CLASS (klass);
  GstElementClass *gstelement_class = (GstElementClass *) klass;

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS(klass),
      gst_static_pad_template_get (&gst_quiclysink_sink_template));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS(klass),
      "Quic dgram server", "Sink/Network", "Send data over the network via quic",
      "Christoph Eifert <christoph.eifert@tum.de>");

  /**
   * GstQuiclysink::on-feedback-report
   * @quiclysink: the object sending the signal
   * @lrtt: uint32_t containing the latest rtt
   * @srtt: uint32_t containing the smoothed rtt
   * @sent: uin64_t containing num packets sent
   * @lost: uin64_t containing num packets lost
   */
  quiclysink_signals[SIGNAL_ON_FEEDBACK_REPORT] =
    g_signal_new("on-feedback-report", G_TYPE_FROM_CLASS(klass),
    G_SIGNAL_RUN_LAST, G_STRUCT_OFFSET(GstQuiclysinkClass, on_feedback_report),
    NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 11,
    G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT64,
    G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT64, G_TYPE_UINT64,
    G_TYPE_UINT64, G_TYPE_UINT, G_TYPE_INT64);

  gobject_class->set_property = gst_quiclysink_set_property;
  gobject_class->get_property = gst_quiclysink_get_property;
  gobject_class->dispose = gst_quiclysink_dispose;
  gobject_class->finalize = gst_quiclysink_finalize;

  base_sink_class->start = GST_DEBUG_FUNCPTR (gst_quiclysink_start);
  base_sink_class->stop = GST_DEBUG_FUNCPTR (gst_quiclysink_stop);
  base_sink_class->event = GST_DEBUG_FUNCPTR (gst_quiclysink_event);
  
  base_sink_class->set_caps = GST_DEBUG_FUNCPTR (gst_quiclysink_set_caps);
  base_sink_class->render = GST_DEBUG_FUNCPTR (gst_quiclysink_render);
  base_sink_class->render_list = GST_DEBUG_FUNCPTR (gst_quiclysink_render_list);

  gstelement_class->set_clock = GST_DEBUG_FUNCPTR(gst_quiclysink_set_clock);

  g_object_class_install_property(gobject_class, PROP_BIND_ADDRESS, 
                  g_param_spec_string("bind-addr", "BindAddr", "the host address to bind", 
                  UDP_DEFAULT_BIND_ADDRESS, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(gobject_class, PROP_BIND_PORT, 
                  g_param_spec_int("bind-port", "BindPort", "the port to bind", 1, 65535, 
                  UDP_DEFAULT_BIND_PORT, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property(gobject_class, PROP_CERTIFICATE, 
                                  g_param_spec_string("cert", "Server Cert",
                                  "The server certificate chain file",
                                  DEFAULT_CERTIFICATE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(gobject_class, PROP_PRIVATE_KEY, 
                                  g_param_spec_string("key", "Server Key",
                                  "The server private key file",
                                  DEFAULT_PRIVATE_KEY, G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_QUICLY_MTU,
                                  g_param_spec_uint ("quicly-mtu", "Quicly Maximum Transmission Unit",
                                  "Maximum packet size to send.",
                                  0, G_MAXINT, QUICLY_DEFAULT_MTU,
                                  G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(gobject_class, PROP_STREAM_MODE,
                                  g_param_spec_boolean("stream-mode", "Stream Mode",
                                  "Use streams instead of datagrams.",
                                  DEFAULT_STREAM_MODE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(gobject_class, PROP_MULTI_STREAM_MODE,
                                  g_param_spec_boolean("multi-stream-mode", "Multi Stream Mode",
                                  "Use new stream id for each packet.",
                                  DEFAULT_STREAM_MODE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(gobject_class, PROP_STATS,
                                  g_param_spec_boxed("stats", "Statistics", "Various Statistics",
                                  GST_TYPE_STRUCTURE, G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(gobject_class, PROP_AUTO_CAPS_EXCHANGE,
                                  g_param_spec_boolean("cap-exchange", "Caps Exchange", "Auto cap exchange",
                                  DEFAULT_AUTO_CAPS_EXCHANGE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(gobject_class, PROP_APPLICATION_CC,
                                g_param_spec_boolean("app-cc", "ApplicationCC", "Use application level congestion control",
                                DEFAULT_APPLICATION_CC, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(gobject_class, PROP_FEEDBACK,
                                g_param_spec_boolean("feedback", "FeedbackData", "Send feedback signal",
                                DEFAULT_FEEDBACK, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
  g_object_class_install_property(gobject_class, PROP_DROP_LATE,
                                g_param_spec_int("drop-late", "DropLate", "Drop late packets. 0: Drop immediatly, -1: Never (Default)",
                                -1, 65535, DEFAULT_DROP_LATE,
                                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
}

static void
gst_quiclysink_init (GstQuiclysink *quiclysink)
{
  /* Setup */
  quiclysink->bind_iaddr = g_strdup (UDP_DEFAULT_BIND_ADDRESS);
  quiclysink->bind_port = UDP_DEFAULT_BIND_PORT;

  quiclysink->num_packets = 0;
  quiclysink->num_bytes = 0;
  quiclysink->silent = TRUE;
  quiclysink->stream_mode = DEFAULT_STREAM_MODE;
  quiclysink->multi_stream_mode = DEFAULT_STREAM_MODE;
  quiclysink->received_caps_ack = FALSE;
  quiclysink->auto_caps_exchange = DEFAULT_AUTO_CAPS_EXCHANGE;
  quiclysink->application_cc = DEFAULT_APPLICATION_CC;
  quiclysink->feedback_active = DEFAULT_FEEDBACK;
  quiclysink->drop_late = DEFAULT_DROP_LATE;
  quiclysink->clockId = NULL;
  quiclysink->fbClockId = NULL;
  quiclysink->pipeline_clock = NULL;
  quiclysink->quicly_mtu = QUICLY_DEFAULT_MTU;

  /* Setup quicly and tls context */
  quiclysink->tlsctx.random_bytes = ptls_openssl_random_bytes;
  quiclysink->tlsctx.get_time = &ptls_get_time;
  quiclysink->tlsctx.key_exchanges = quiclysink->key_exchanges;
  quiclysink->tlsctx.cipher_suites = ptls_openssl_cipher_suites;
  quiclysink->tlsctx.require_dhe_on_psk = 1;
  quiclysink->tlsctx.save_ticket = &save_ticket;
  quiclysink->tlsctx.on_client_hello = &on_client_hello;

  quiclysink->ctx = quicly_spec_context;
  quiclysink->ctx.tls = &quiclysink->tlsctx;
  quiclysink->ctx.stream_open = &stream_open;
  quiclysink->ctx.dgram_open = &dgram_open;
  quiclysink->ctx.closed_by_peer = &closed_by_peer;
  //quiclysink->ctx.save_resumption_token = &save_resumption_token;
  //quiclysink->ctx.generate_resumption_token = &generate_resumption_token;

  quiclysink->ctx.save_resumption_token = NULL;
  quiclysink->ctx.generate_resumption_token = NULL;

  setup_session_cache(quiclysink->ctx.tls);
  quicly_amend_ptls_context(quiclysink->ctx.tls);

  /* key exchange and cid */
  quiclysink->key_exchanges[0] = &ptls_openssl_secp256r1;
  quiclysink->cid_key = malloc(sizeof(gchar) * 17);
  quiclysink->tlsctx.random_bytes(quiclysink->cid_key, sizeof(*quiclysink->cid_key) - 1);

  quiclysink->ctx.cid_encryptor = quicly_new_default_cid_encryptor(
                                  &ptls_openssl_bfecb, &ptls_openssl_aes128ecb, &ptls_openssl_sha256,
                                  ptls_iovec_init(quiclysink->cid_key,
                                  strlen(quiclysink->cid_key)));
  
  quiclysink->conn = NULL;
  quiclysink->conn_addr = NULL;
  quiclysink->dgram = NULL;
  /* -------- end context init --------------*/

  quiclysink->recv_buf = malloc(sizeof(gchar) * (2048 + 1));
  quiclysink->recv_buf_size = 2048;
  quiclysink->previousPts = 0;
}

void
gst_quiclysink_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK (object);

  GST_DEBUG_OBJECT (quiclysink, "set_property");

  switch (property_id) {
    case PROP_BIND_ADDRESS:
      if (quiclysink->bind_iaddr != NULL)
        g_free(quiclysink->bind_iaddr);
      quiclysink->bind_iaddr = g_value_dup_object(value);
      break;
    case PROP_BIND_PORT:
      quiclysink->bind_port = g_value_get_int(value);
      break;
    case PROP_QUICLY_MTU: {
      /* TODO: Remove the max limit of 1280, after removing the macro in quicly */
      guint tmp = g_value_get_uint(value);
      quiclysink->quicly_mtu = (tmp + 28 > 1280) ? 1252 : tmp + 28;
      break;
    }
    case PROP_CERTIFICATE:
      g_free(quiclysink->cert);
      if (g_value_get_string(value) == NULL)
        quiclysink->cert = g_strdup(DEFAULT_CERTIFICATE);
      else
        quiclysink->cert = g_value_dup_string(value);
      break;
    case PROP_PRIVATE_KEY:
      g_free(quiclysink->key);
      if (g_value_get_string(value) == NULL)
        quiclysink->key = g_strdup(DEFAULT_PRIVATE_KEY);
      else
        quiclysink->key = g_value_dup_string(value);
      break;
    case PROP_STREAM_MODE:
      quiclysink->stream_mode = g_value_get_boolean(value);
      break;
    case PROP_MULTI_STREAM_MODE:
      quiclysink->multi_stream_mode = g_value_get_boolean(value);
      break;
    case PROP_AUTO_CAPS_EXCHANGE:
      quiclysink->auto_caps_exchange = g_value_get_boolean(value);
      break;
    case PROP_APPLICATION_CC:
      quiclysink->application_cc = g_value_get_boolean(value);
      break;
    case PROP_FEEDBACK:
      quiclysink->feedback_active = g_value_get_boolean(value);
      break;
    case PROP_DROP_LATE:
      quiclysink->drop_late = g_value_get_int(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_quiclysink_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK (object);

  GST_DEBUG_OBJECT (quiclysink, "get_property");

  switch (property_id) {
    case PROP_STATS:
      g_value_take_boxed(value, gst_quiclysink_create_stats(quiclysink));
      break;
    case PROP_AUTO_CAPS_EXCHANGE:
      g_value_set_boolean(value, quiclysink->auto_caps_exchange);
      break;
    case PROP_DROP_LATE:
      g_value_set_int(value, quiclysink->drop_late);
      break;
    case PROP_MULTI_STREAM_MODE:
      g_value_set_boolean(value, quiclysink->multi_stream_mode);
      break;
    case PROP_APPLICATION_CC:
      g_value_set_boolean(value, quiclysink->application_cc);
      break;
    case PROP_FEEDBACK:
      g_value_set_boolean(value, quiclysink->feedback_active);
      break;
    case PROP_STREAM_MODE:
      g_value_set_boolean(value, quiclysink->stream_mode);
      break;
    case PROP_BIND_PORT:
      g_value_set_int(value, quiclysink->bind_port);
      break;
    case PROP_BIND_ADDRESS:
      g_value_set_string(value, quiclysink->bind_iaddr);
      break;
    case PROP_QUICLY_MTU:
      g_value_set_uint(value, quiclysink->quicly_mtu);
      break;
    case PROP_CERTIFICATE:
      g_value_set_string(value, quiclysink->cert);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
      break;
  }
}

void
gst_quiclysink_dispose (GObject * object)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK (object);

  GST_DEBUG_OBJECT (quiclysink, "dispose");

  /* clean up as possible.  may be called multiple times */

  G_OBJECT_CLASS (gst_quiclysink_parent_class)->dispose (object);
}

void
gst_quiclysink_finalize (GObject * object)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK (object);
  GST_DEBUG_OBJECT (quiclysink, "finalize");

  /* clean up object here */
  if (quiclysink->clockId != NULL) {
    gst_clock_id_unschedule(quiclysink->clockId);
    gst_clock_id_unref(quiclysink->clockId);
  }
  if (quiclysink->fbClockId != NULL) {
    gst_clock_id_unschedule(quiclysink->fbClockId);
    gst_clock_id_unref(quiclysink->fbClockId);
  }

  /* clean up gst ressources */
  if (quiclysink->socket)
    g_object_unref(quiclysink->socket);
  if (quiclysink->bind_addr)
    g_object_unref(quiclysink->bind_addr);
  /* TODO: Why is conn_addr sometimes not NULL but not a G_OBJECT? */
  if (quiclysink->conn_addr && G_IS_OBJECT(quiclysink->conn_addr))
    g_object_unref(quiclysink->conn_addr);
  if (quiclysink->bind_iaddr != NULL) {
    g_free(quiclysink->bind_iaddr);
    quiclysink->bind_iaddr = NULL;
  }

  /* clean up quicly ressources */
  if (quiclysink->recv_buf != NULL) {
    free(quiclysink->recv_buf);
    quiclysink->recv_buf = NULL;
  }
  if (quiclysink->conn != NULL) {
    free(quiclysink->conn);
    quiclysink->conn = NULL;
  }
  if (quiclysink->dgram != NULL) {
    free(quiclysink->dgram);
    quiclysink->dgram = NULL;
  }
  G_OBJECT_CLASS (gst_quiclysink_parent_class)->finalize (object);
}

/* start and stop processing, ideal for opening/closing the resource */
static gboolean
gst_quiclysink_start (GstBaseSink * sink)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK (sink);
  GST_DEBUG_OBJECT (quiclysink, "start");

  if (quiclysink->cert != NULL && quiclysink->key != NULL) {
    load_certificate_chain(quiclysink->ctx.tls, quiclysink->cert);
    load_private_key(quiclysink->ctx.tls, quiclysink->key);
  } else {
    g_printerr("Failed to load certificate and key files\n");
    return FALSE;
  }

  GError *err = NULL;
  GInetAddress *iaddr;

  iaddr = g_inet_address_new_from_string(quiclysink->bind_iaddr);
  if (!iaddr) {
    g_printerr("Could not resolve host address\n");
    return FALSE;
  }
  
  quiclysink->bind_addr = g_inet_socket_address_new(iaddr, quiclysink->bind_port);
  g_object_unref(iaddr);

  if ((quiclysink->socket = g_socket_new(G_SOCKET_FAMILY_IPV4, 
                           G_SOCKET_TYPE_DATAGRAM, 
                           G_SOCKET_PROTOCOL_UDP, &err)) == NULL) {
    g_printerr("Could not create socket\n");
    return FALSE;
  }

  if (!g_socket_bind(quiclysink->socket, quiclysink->bind_addr, TRUE, &err)) {
    g_printerr("Could not bind socket\n");
    return FALSE;
  }

  int64_t timeout_at;
  int64_t delta;
  int64_t wait = 0;
  err = NULL;

  g_print("Waiting for client...");
  while(1) {
    if (quiclysink->conn != NULL) {
      if (quicly_connection_is_ready(quiclysink->conn))
        break;
    }
    timeout_at = quiclysink->conn != NULL ? quicly_get_first_timeout(quiclysink->conn) : INT64_MAX;
    if (timeout_at != INT64_MAX) {
      delta = timeout_at - quiclysink->ctx.now->cb(quiclysink->ctx.now);
      if (delta > 0) {
        wait = delta * 1000;
      } else {
        wait = 0;
      }
    } else {
      wait = timeout_at;
    }
    if (g_socket_condition_timed_wait(quiclysink->socket, G_IO_IN | G_IO_PRI, wait, NULL, &err)) {
      if (receive_packet(quiclysink) != 0) {
        g_printerr("Error in receive_packet\n");
      }
    }
    err = NULL;
    if ((quiclysink->conn != NULL) && 
         (quicly_get_first_timeout(quiclysink->conn) <= quiclysink->ctx.now->cb(quiclysink->ctx.now))) {
      if (send_pending(quiclysink, DEFAULT_SEND_BUFFER) != 0) {
        quicly_free(quiclysink->conn);
        g_print("Connection closed while sending\n");
        quiclysink->conn = NULL;
      }
    }
  }

  /* init dgram or streams */
  if (quiclysink->stream_mode) {
    if (quicly_open_stream(quiclysink->conn, &quiclysink->stream, 0) != 0) {
      g_printerr("Could not open stream\n");
      return FALSE;
    }
  } else {
    if (quicly_open_dgram(quiclysink->conn, &quiclysink->dgram) != 0) {
      g_printerr("Can't open quicly_dgram\n");
      return FALSE;
    }
  }

  g_print("Connected!\n");
  /* set application context for stream callbacks */
  quicly_set_data(quiclysink->conn, (void*) quiclysink);
  /* set application level cc */
  if (quiclysink->application_cc) {
    quicly_set_application_cc(quiclysink->conn, 1);
  }
  /* Schedule async callback to receive acks and send feedback*/
  GstClock *clock = gst_system_clock_obtain();
  if (!gst_quiclysink_sched_cbs(quiclysink, clock))
    return FALSE;
  gst_object_unref(clock);

  return TRUE;
}

static GstStructure *gst_quiclysink_create_stats(GstQuiclysink *quiclysink)
{
  GstStructure *s;
  
  /* Stats could be 20ms out of date...*/
  if (!quiclysink->feedback_active) {
    GST_OBJECT_LOCK(quiclysink);
    quicly_get_stats(quiclysink->conn, &quiclysink->stats);
    GST_OBJECT_UNLOCK(quiclysink);
  }
  s = gst_structure_new("quiclysink-stats",
      "packets-received", G_TYPE_UINT64, quiclysink->stats.num_packets.received,
      "packets-sent", G_TYPE_UINT64, quiclysink->stats.num_packets.sent,
      "packets-lost", G_TYPE_UINT64, quiclysink->stats.num_packets.lost,
      "acks-received", G_TYPE_UINT64, quiclysink->stats.num_packets.acked,
      "bytes-received", G_TYPE_UINT64, quiclysink->stats.num_bytes.received,
      "bytes-sent", G_TYPE_UINT64, quiclysink->stats.num_bytes.sent,
      "bytes-sent-media", G_TYPE_UINT64, quiclysink->num_bytes,
      "rtt-smoothed", G_TYPE_UINT, quiclysink->stats.rtt.smoothed,
      "rtt-latest", G_TYPE_UINT, quiclysink->stats.rtt.latest,
      "rtt-minimum", G_TYPE_UINT, quiclysink->stats.rtt.minimum,
      "rtt-variance", G_TYPE_UINT, quiclysink->stats.rtt.variance,
      "bytes-in-flight", G_TYPE_UINT64, quiclysink->stats.num_bytes.bytes_in_flight,
      "dropped-late", G_TYPE_UINT64, quiclysink->stats.num_packets.dropped_late,
      "cwnd", G_TYPE_UINT, quiclysink->stats.cc.cwnd, NULL);
  return s;
}

static gboolean
gst_quiclysink_stop (GstBaseSink * sink)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK (sink);

  GST_DEBUG_OBJECT(quiclysink, 
          "Stop. Num Packets sent: %lu. Kilobytes sent: %lu. Packets left in buffer: %lu\n", 
          quiclysink->num_packets, quiclysink->num_bytes / 1000,
          quicly_dgram_debug(quiclysink->dgram));

  if (quicly_close(quiclysink->conn, 0, "") != 0)
    g_printerr("Error on close. Unclean shutdown\n");

  GIOCondition con;
  do {
    if (send_pending(quiclysink, DEFAULT_SEND_BUFFER) != 0) {
      g_print("In STOP: sending connection close packet failed\n");
      break;
    }
    if ((con = g_socket_condition_check(quiclysink->socket, G_IO_IN)) & G_IO_IN) {
      if (receive_packet(quiclysink) != 0)
        break;
    }
  } while ((quiclysink->conn != NULL) && 
         (quicly_get_first_timeout(quiclysink->conn) <= quiclysink->ctx.now->cb(quiclysink->ctx.now)));

  return TRUE;
}

/* 
 * Emit feedback signal to application
 */
gboolean emit_feedback_signal_cb(GstClock *clock, GstClockTime t, GstClockID id, gpointer data)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK(data);

  GST_OBJECT_LOCK(quiclysink);
  quicly_get_stats(quiclysink->conn, &quiclysink->stats);
  GST_OBJECT_UNLOCK(quiclysink);

  g_signal_emit(quiclysink, quiclysink_signals[SIGNAL_ON_FEEDBACK_REPORT], 0,
    quiclysink->stats.num_packets.sent, quiclysink->stats.num_packets.lost, quiclysink->stats.num_packets.acked, 
    quiclysink->stats.num_bytes.sent, quiclysink->stats.num_bytes.lost, quiclysink->stats.num_bytes.acked, 
    quiclysink->stats.timestamp.latest_ack_send_time, quiclysink->stats.timestamp.latest_ack_recv_time, 
    quiclysink->stats.num_bytes.bytes_in_flight, quiclysink->stats.cc.cwnd, quiclysink->stats.timestamp.now);

  return TRUE;
}

/* TODO: Use only one function. e.g. call the same function from render and
 *   render list, compare MultiUDPsink.c
 */
static GstFlowReturn
gst_quiclysink_render (GstBaseSink * sink, GstBuffer * buffer)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK (sink);

  GST_LOG_OBJECT (quiclysink, "render");

  GstMapInfo map;
  int ret;

  gst_buffer_map(buffer, &map, GST_MAP_READ);

  /* write buffer to quicly dgram buffer */
  if (!quiclysink->stream_mode){
    /* Check if payload size fits in one quicly datagram frame */
    if (map.size > quiclysink->quicly_mtu) {
      g_printerr("Max payload size exceeded: %lu. MTU: %u\n", map.size, quiclysink->quicly_mtu);
      return GST_FLOW_ERROR;
    }
    write_dgram_buffer(quiclysink->dgram, map.data, map.size, 
                       quiclysink->drop_late > 0 ? (quiclysink->ctx.now->cb(quiclysink->ctx.now) + 2) : quiclysink->drop_late);
  } else {
    quicly_streambuf_egress_write_rtp_framing(quiclysink->stream, map.data, map.size);
  }
  if ((ret = send_pending(quiclysink, DEFAULT_SEND_BUFFER)) != 0) {
    g_printerr("Send failed in render\n");
  }
  ++quiclysink->num_packets;
  quiclysink->num_bytes += map.size;
  gst_buffer_unmap(buffer, &map);

  return GST_FLOW_OK;
}

static GstFlowReturn 
gst_quiclysink_render_list (GstBaseSink * sink, GstBufferList * buffer_list)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK (sink);
  GstBuffer *buffer;
  GstFlowReturn flow;
  guint num_buffers, i;
  GstMapInfo map;
  gint64 now = quiclysink->ctx.now->cb(quiclysink->ctx.now);
  int ret;

  GST_LOG_OBJECT(quiclysink, "render_list");

  num_buffers = gst_buffer_list_length(buffer_list);
  if (num_buffers == 0) {
    GST_LOG_OBJECT(quiclysink, "empty buffer list");
    return GST_FLOW_OK;
  }

  /* write buffers to quicly dgram buffer */
  for (i = 0; i < num_buffers; ++i) {
    buffer = gst_buffer_list_get(buffer_list, i);
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
      if (!quiclysink->stream_mode) {
        /* Check if payload size fits in one quicly datagram frame */
        if (map.size > quiclysink->quicly_mtu) {
          g_printerr("Max payload size exceeded: %lu\n", map.size);
          return GST_FLOW_ERROR;
        }
        write_dgram_buffer(quiclysink->dgram, map.data, map.size, 
                           quiclysink->drop_late > 0 ? (now + 2 * i) : quiclysink->drop_late);
      } else {
        /* TODO: Move rtp framing to quiclysink.c */
        quicly_streambuf_egress_write_rtp_framing(quiclysink->stream, map.data, map.size);
      }
      ++quiclysink->num_packets;
      quiclysink->num_bytes += map.size;
    }
    gst_buffer_unmap(buffer, &map);
  }

  /* SEND */
  if (num_buffers < DEFAULT_SEND_BUFFER)
    num_buffers = DEFAULT_SEND_BUFFER;
  if ((ret = send_pending(quiclysink, num_buffers)) != 0) {
    g_printerr("Send failed in render lists\n");
    flow = GST_FLOW_ERROR;
  } else {
    flow = GST_FLOW_OK;
  }

  return flow;
}

/*
 * Receive ACKs async
 */
gboolean receive_async_cb(GstClock *clock, GstClockTime t, GstClockID id, gpointer data)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK(data);

  /* Try to receive one packet */
  GIOCondition con;
  if ((con = g_socket_condition_check(quiclysink->socket, G_IO_IN | G_IO_PRI)) & 
                                      (G_IO_IN | G_IO_PRI)) {
      GST_OBJECT_LOCK(quiclysink);
      if (receive_packet(quiclysink) != 0)
        g_printerr("Receive failed\n");
      GST_OBJECT_UNLOCK(quiclysink);
  }

  return TRUE;
}

static int receive_packet(GstQuiclysink *quiclysink)
{
  GError *err = NULL;
  GSocketAddress *in_addr;
  size_t off, plen;
  gssize rret;
  if ((rret = g_socket_receive_from(quiclysink->socket,
                                    &in_addr,
                                    quiclysink->recv_buf,
                                    quiclysink->recv_buf_size,
                                    NULL,
                                    &err)) < 0) {
    g_printerr("Socket receive failed. Code: %s\n", err->message);
    return -1;
  }
  off = 0;
  /* TODO: Remove sockaddr cast by using recvfrom */
  /* Convert GSocketAddress to native */
  struct sockaddr native_sa;
  gssize len = g_socket_address_get_native_size(in_addr);
  if (!g_socket_address_to_native(in_addr, &native_sa, len, &err)) {
    g_printerr("Could not convert GSocketAddress to native. Error: %s\n", err->message);
    goto error;
  }
  while (off != rret) {
    quicly_decoded_packet_t packet;
    plen = quicly_decode_packet(&quiclysink->ctx, &packet, 
                               (uint8_t *)quiclysink->recv_buf + off,
                                rret - off);
    if (plen == SIZE_MAX)
      break;
    if (quiclysink->conn != NULL) {
      quicly_receive(quiclysink->conn, NULL, &native_sa, &packet);
    } else if (QUICLY_PACKET_IS_LONG_HEADER(packet.octets.base[0])) {
      
      /* TODO: handle unbound connection */
      /* TODO: handle packet.token in quicly_accept */

      quicly_address_token_plaintext_t *token = NULL;
      if (quicly_accept(&quiclysink->conn, &quiclysink->ctx, NULL,
                          &native_sa, &packet, token,
                          &quiclysink->next_cid, NULL) == 0) {
        if (quiclysink->conn == NULL) {
          g_printerr("Quicly accept returned success but conn is NULL\n");
          goto error;
        }
        quiclysink->conn_addr = in_addr;
        ++quiclysink->next_cid.master_id;
      } else {
        if (quiclysink->conn == NULL) {
          g_printerr("Failed to accept connection\n");
          goto error;
        }
      }
    } else {
      g_print("Server: received short header packet but conn == NULL\n");
      goto error;
    }
    off += plen;
  }
  g_object_unref(in_addr);
  return 0;

  error:
    {
      g_object_unref(in_addr);
      return -1;
    }
}

/* 
 * write packet to send buffer.
 * Set max_time to -1 to disable dropping.
 */
static void write_dgram_buffer(quicly_dgram_t *dgram, const void *src, size_t len, gint64 max_time) 
{
  int ret;
  if ((ret = quicly_dgrambuf_egress_write(dgram, src, len, max_time)) != 0)
    g_printerr("quicly_dgrambuf_egress_write returns: %i\n", ret);
}

/*
 * Send all committed buffers as fast as possible
 * Does not return until everythin is sent
 */
static int send_pending(GstQuiclysink *quiclysink, guint num)
{
  quicly_datagram_t *packets[num];
  size_t num_packets, i;
  gssize rret;
  int ret;
  GError *err = NULL;
  gssize all = 0;

  do {
      num_packets = sizeof(packets) / sizeof(packets[0]);
      GST_OBJECT_LOCK(quiclysink);
      ret = quicly_send(quiclysink->conn, packets, &num_packets);
      GST_OBJECT_UNLOCK(quiclysink);
      if (ret == 0) {
        for (i = 0; i != num_packets; ++i) {
          if ((rret = g_socket_send_to(quiclysink->socket, quiclysink->conn_addr, 
                                       (gchar *)packets[i]->data.base, 
                                       packets[i]->data.len,
                                       NULL, &err)) < 0) {
            g_printerr("g_socket_send_to returned error\n");
            if (err != NULL) {
              g_printerr("g_socket_send_to returned error. Message: %s\n", err->message);
              err = NULL;
            }
            break;
          }
          all += rret;
          ret = 0;
          quicly_packet_allocator_t *pa = quiclysink->ctx.packet_allocator;
          pa->free_packet(pa, packets[i]);
        }
      } else {
        g_printerr("Send returned %i.\n", ret);
      }

  } while ((ret == 0) && 
    (quicly_dgram_can_send(quiclysink->dgram) || 
    quiclysink->ctx.stream_scheduler->can_send(quiclysink->ctx.stream_scheduler, quiclysink->conn, 0)));

  return ret;
}

static int send_caps(GstQuiclysink *quiclysink)
{
  gchar *cp = gst_caps_to_string(quiclysink->caps);
  quicly_stream_t *stream;
  int ret;
  if ((ret = quicly_open_stream(quiclysink->conn, &stream, 0)) == 0) {
    gchar send[strlen(cp) + 20];
    sprintf(send, "MSG:CAPS;DATA:%s\n", cp);
    quicly_streambuf_egress_write(stream, send, strlen(send));
    quicly_streambuf_egress_shutdown(stream);
  }
  g_free(cp);
  return ret;
}

static gboolean gst_quiclysink_set_caps (GstBaseSink *sink, GstCaps *caps)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK (sink);
  GST_LOG_OBJECT (quiclysink, "Caps set: %s", gst_caps_to_string(caps));
  
  if (!quiclysink->auto_caps_exchange)
    return TRUE;

  int ret = -1;
  if (quiclysink->caps)
    gst_caps_unref(caps);
  quiclysink->caps = gst_caps_copy (caps);
  if (quiclysink->conn != NULL) {
    if (send_caps(quiclysink) == 0) {
      do {
        ret = send_pending(quiclysink, DEFAULT_SEND_BUFFER);
      } while ((ret == 0) && (!quiclysink->received_caps_ack));
      GST_INFO_OBJECT(quiclysink, "Send caps and received ack");
    }
  }
  if (ret != 0)
    GST_ERROR_OBJECT(quiclysink, "Send caps failed");

  return (ret == 0) ? TRUE : FALSE;
}

static int on_receive_dgram(quicly_dgram_t *dgram, const void *src, size_t len)
{
  /* Don't care */
  return 0;
}

static int on_receive_stream(quicly_stream_t *stream, size_t off, const void *src, size_t len)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK (*quicly_get_data(stream->conn));
  ptls_iovec_t input;
  int ret;

  if ((ret = quicly_streambuf_ingress_receive(stream, off, src, len)) != 0)
    return ret;

  if ((input = quicly_streambuf_ingress_get(stream)).len != 0) {
    char head[4] = {input.base[0], input.base[1], input.base[2], '\0'};
    g_print("STREAM MESSAGE: %s\n", head);
    if (strcmp(head, "MSG") == 0) {
      /* TODO: Read all of the message. For now I only have the caps ack */
      /* Set received_caps_ack */
      GST_LOG_OBJECT(quiclysink, "RECEIVED CAPS ACK");
      quiclysink->received_caps_ack = TRUE;
    } 
  }
  return 0;
}

static int on_dgram_open(quicly_dgram_open_t *self, quicly_dgram_t *dgram)
{
    int ret;
    if ((ret = quicly_dgrambuf_create(dgram, sizeof(quicly_dgrambuf_t))) != 0)
        return ret;
    dgram->callbacks = &dgram_callbacks;
    return 0;
}

static int on_stream_open(quicly_stream_open_t *self, quicly_stream_t *stream)
{
    int ret;

    if ((ret = quicly_streambuf_create(stream, sizeof(quicly_streambuf_t))) != 0)
        return ret;
    stream->callbacks = &stream_callbacks;
    return 0;
}

/* TODO: Check */
static void on_closed_by_peer(quicly_closed_by_peer_t *self, quicly_conn_t *conn, int err, uint64_t frame_type, const char *reason,
                              size_t reason_len)
{
    if (QUICLY_ERROR_IS_QUIC_TRANSPORT(err)) {
        fprintf(stderr, "transport close: code=%d\n", err);
    } else if (QUICLY_ERROR_IS_QUIC_APPLICATION(err)) {
        g_printerr("application close: code=%d\n", err);
    } else if (err == QUICLY_ERROR_RECEIVED_STATELESS_RESET) {
        g_printerr("stateless reset\n");
    } else {
        g_printerr("unexpected close:code=%d\n", err);
    }
}

static int on_stop_sending(quicly_stream_t *stream, int err)
{
    assert(QUICLY_ERROR_IS_QUIC_APPLICATION(err));
    g_printerr("received STOP_SENDING\n");
    return 0;
}

static int on_receive_reset(quicly_stream_t *stream, int err)
{
    assert(QUICLY_ERROR_IS_QUIC_APPLICATION(err));
    g_printerr("received RESET_STREAM\n");
    return 0;
}

/* Negotiate features on client hello */
static int on_client_hello_cb(ptls_on_client_hello_t *_self, ptls_t *tls, ptls_on_client_hello_parameters_t *params)
{
  return 0;
}

int save_ticket_cb(ptls_save_ticket_t *_self, ptls_t *tls, ptls_iovec_t src)
{
    quicly_conn_t *conn = *ptls_get_data_ptr(tls);
    ptls_buffer_t buf;
    FILE *fp = NULL;
    int ret;

    if (session_file == NULL)
        return 0;

    ptls_buffer_init(&buf, "", 0);

    /* build data (session ticket and transport parameters) */
    ptls_buffer_push_block(&buf, 2, { ptls_buffer_pushv(&buf, src.base, src.len); });
    ptls_buffer_push_block(&buf, 2, {
        if ((ret = quicly_encode_transport_parameter_list(&buf, 1, quicly_get_peer_transport_parameters(conn), NULL, NULL, 0)) != 0)
            goto Exit;
    });

    /* write file */
    if ((fp = fopen(session_file, "wb")) == NULL) {
        fprintf(stderr, "failed to open file:%s:%s\n", session_file, strerror(errno));
        ret = PTLS_ERROR_LIBRARY;
        goto Exit;
    }
    fwrite(buf.base, 1, buf.off, fp);

    ret = 0;
Exit:
    if (fp != NULL)
        fclose(fp);
    ptls_buffer_dispose(&buf);
    return 0;
}

/* Obtain the pipeline clock and schedule a callback for receving quic packets */
static gboolean gst_quiclysink_sched_cbs(GstQuiclysink *quiclysink, GstClock *clock)
{
  gboolean ret = TRUE;

  if ((quiclysink->clockId = gst_clock_new_periodic_id(clock, 
                               gst_clock_get_internal_time(clock), 
                               RECEIVE_CLOCK_TIME_NS)) == NULL)
    ret = FALSE;

  if (gst_clock_id_wait_async(quiclysink->clockId, receive_async_cb, quiclysink, NULL) != GST_CLOCK_OK)
    ret = FALSE;

  if (quiclysink->feedback_active) {
    if ((quiclysink->fbClockId = gst_clock_new_periodic_id(clock, 
                                gst_clock_get_internal_time(clock), 
                                FEEDBACK_TIME_INTERVAL_NS)) == NULL)
      ret = FALSE;

    if (gst_clock_id_wait_async(quiclysink->fbClockId, emit_feedback_signal_cb, quiclysink, NULL) != GST_CLOCK_OK)
      ret = FALSE;
  }

  return ret;
}

gboolean gst_quiclysink_set_clock(GstElement *element, GstClock *clock)
{
  GstQuiclysink *quiclysink = GST_QUICLYSINK(element);

  if (GST_IS_CLOCK(clock)) {
    quiclysink->pipeline_clock = clock;
  }

  return GST_ELEMENT_CLASS(gst_quiclysink_parent_class)->set_clock(element, clock);
}

static void send_remaining_before_close(GstQuiclysink *quiclysink)
{
  GST_DEBUG_OBJECT(quiclysink, "Sending final packets\n");
  send_pending(quiclysink, DEFAULT_SEND_BUFFER);
}

static gboolean gst_quiclysink_event (GstBaseSink * sink, GstEvent * event)
{
  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_EOS:
      send_remaining_before_close(GST_QUICLYSINK(sink));
    default:
      return GST_BASE_SINK_CLASS(gst_quiclysink_parent_class)->event(sink, event);
      //return GST_BASE_SINK_CLASS(sink)->event(sink, event);
  }
}

static gboolean
plugin_init (GstPlugin * plugin)
{

  /* FIXME Remember to set the rank if it's an element that is meant
     to be autoplugged by decodebin. */
  return gst_element_register (plugin, "quiclysink", GST_RANK_NONE,
      GST_TYPE_QUICLYSINK);
}

/* FIXME: these are normally defined by the GStreamer build system.
   If you are creating an element to be included in gst-plugins-*,
   remove these, as they're always defined.  Otherwise, edit as
   appropriate for your external plugin package. */
#ifndef VERSION
#define VERSION "0.1"
#endif
#ifndef PACKAGE
#define PACKAGE "quicly-dgram"
#endif
#ifndef PACKAGE_NAME
#define PACKAGE_NAME "none"
#endif
#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/Banaschar/quicly-dgram"
#endif

GST_PLUGIN_DEFINE (GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    quiclysink,
    "Quic server plugin",
    plugin_init, VERSION, "LGPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)

