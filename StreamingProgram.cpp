#include <locale.h>
#include <glib.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <string>
#include <algorithm>
#include <thread>
#include <regex>

#define RTP_PAYLOAD_TYPE "96"
#define RTP_AUDIO_PAYLOAD_TYPE "97"
#define SOUP_HTTP_PORT 8081  // WebSocket signaling port (different from WebControlServer:8080)

// g++ StreamingProgram.cpp -o StreamingProgram `pkg-config --cflags --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4 json-glib-1.0` -std=c++17

extern "C"
{
  gchar *encoding = NULL;
  static gchar *codec = NULL;
  static int bitrate = 6000;
  static int fps = 60;
  static int width = 1920;
  static int height = 1080;
  static gchar *turn = NULL;
  static gchar *stun = NULL;
  static gchar *d_ip = NULL;
  static int d_port = 5004;
  static gchar *audio_device = NULL;  // Audio device (e.g., hw:1,1)
  static gchar *acodec = NULL;        // Audio codec (aac or opus)
  static int abitrate = 128;          // Audio bitrate in kbps

  typedef struct _ReceiverEntry ReceiverEntry;

  ReceiverEntry *create_receiver_entry(SoupWebsocketConnection *connection, gchar *ip);
  void destroy_receiver_entry(gpointer receiver_entry_ptr);

  void on_offer_created_cb(GstPromise *promise, gpointer user_data);
  void on_negotiation_needed_cb(GstElement *webrtcbin, gpointer user_data);
  void on_ice_candidate_cb(GstElement *webrtcbin, guint mline_index,
                           gchar *candidate, gpointer user_data);

  void soup_websocket_message_cb(SoupWebsocketConnection *connection,
                                 SoupWebsocketDataType data_type, GBytes *message, gpointer user_data);
  void soup_websocket_closed_cb(SoupWebsocketConnection *connection,
                                gpointer user_data);

  // soup_http_handler removed - now handled by WebControlServer
  void soup_websocket_handler(G_GNUC_UNUSED SoupServer *server,
                              SoupWebsocketConnection *connection, const char *path,
                              SoupClientContext *client_context, gpointer user_data);

  static gchar *get_string_from_json_object(JsonObject *object);

  GstElement *webrtc_pipeline;
  GstElement *video_tee;
  GstElement *audio_tee;  // Audio tee for multiple audio outputs

  bool available = true;
  int waiting_period = 5; // the waiting period before server can accept new client.

  struct _ReceiverEntry
  {
    SoupWebsocketConnection *connection;
    GHashTable *r_table;

    GstElement *pipeline;
    GstElement *webrtcbin;
    GstElement *queue;
    gchar *client_ip;
    GstPad *tee_src_pad;
    GstPad *sink_pad;
  };

  ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  static GstPadProbeReturn
  event_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
  {
    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_DATA(info)) != GST_EVENT_EOS)
      return GST_PAD_PROBE_OK;

    ReceiverEntry *receiver_entry = (ReceiverEntry *)user_data;

    g_print("\nWebrtcbin received EOS\n");
    g_print("\nStart tearing down Webrtcbin sub-pipeline\n");

    if (receiver_entry != NULL)
    {

      /* remove the probe first */
      gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID(info));

      gst_pad_unlink(receiver_entry->tee_src_pad, receiver_entry->sink_pad);
      gst_element_release_request_pad(video_tee, receiver_entry->tee_src_pad);
      gst_object_unref(receiver_entry->tee_src_pad);
      gst_object_unref(receiver_entry->sink_pad);

      GstState state, pending;
      GstStateChangeReturn ret;

      ret = gst_element_set_state(receiver_entry->pipeline, GST_STATE_NULL);
      if (ret == GST_STATE_CHANGE_FAILURE)
      {
        g_warning("Failed to set WebRTC sub-pipeline to NULL state");
      }
      else
      {
        // Wait for the state change to complete (timeout: 5 seconds)
        ret = gst_element_get_state(receiver_entry->pipeline, &state, &pending, 0);
        if (ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL)
        {
          g_print("Pipeline reached NULL state\n");
          /* remove unlinks automatically */
          g_print("\nremoving %p\n", receiver_entry->pipeline);
          gst_bin_remove(GST_BIN(webrtc_pipeline), receiver_entry->pipeline);

          if (GST_IS_OBJECT(receiver_entry->webrtcbin))
            gst_object_unref(receiver_entry->webrtcbin);
          if (GST_IS_OBJECT(receiver_entry->queue))
            gst_object_unref(receiver_entry->queue);
          if (GST_IS_OBJECT(receiver_entry->pipeline))
            gst_object_unref(receiver_entry->pipeline);
        }
        else
        {
          g_warning("WebRTC sub-pipelin failed to reach NULL state properly");
        }
      }
    }

    if (receiver_entry->connection != NULL && receiver_entry->r_table != NULL)
    {
      g_hash_table_remove(receiver_entry->r_table, receiver_entry->connection);
    }
    gst_print("Closed websocket connection %p\n", (gpointer)receiver_entry->connection);

    return GST_PAD_PROBE_DROP;
  }

  static GstPadProbeReturn
  pad_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
  {
    ReceiverEntry *receiver_entry = (ReceiverEntry *)user_data;

    g_print("\ntee src pad is blocked now\n");

    // remove the probe so that it will not fire this funciton again.
    gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID(info));

    gst_pad_add_probe(receiver_entry->sink_pad, (GstPadProbeType)(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), event_probe_cb, user_data, NULL);

    gst_pad_send_event(receiver_entry->sink_pad, gst_event_new_eos());

    return GST_PAD_PROBE_OK;
  }

  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

  void update_availability()
  {
    while (true)
    {
      std::this_thread::sleep_for(std::chrono::seconds(waiting_period));
      available = true;
      // std::unique_lock<std::mutex> lock(map_mutex);
      // for (auto it = unavailable.begin(); it != unavailable.end(); ++it)
      // {
      //   unavailable[it->first] = false;
      // }
      // lock.unlock();
    }
  }

  static gboolean
  bus_watch_cb(GstBus *bus, GstMessage *message, gpointer user_data)
  {
    switch (GST_MESSAGE_TYPE(message))
    {
    case GST_MESSAGE_ERROR:
    {
      GError *error = NULL;
      gchar *debug = NULL;

      gst_message_parse_error(message, &error, &debug);
      g_error("Error on bus: %s (debug: %s)", error->message, debug);
      g_error_free(error);
      g_free(debug);
      break;
    }
    case GST_MESSAGE_WARNING:
    {
      GError *error = NULL;
      gchar *debug = NULL;

      gst_message_parse_warning(message, &error, &debug);
      g_warning("Warning on bus: %s (debug: %s)", error->message, debug);
      g_error_free(error);
      g_free(debug);
      break;
    }
    default:
      break;
    }

    return G_SOURCE_CONTINUE;
  }

  ReceiverEntry *
  create_receiver_entry(SoupWebsocketConnection *connection, gchar *client_ip)
  {
    GError *error;
    ReceiverEntry *receiver_entry;
    GstWebRTCRTPTransceiver *trans;
    GArray *transceivers;
    GstBus *bus;

    receiver_entry = (ReceiverEntry *)g_slice_alloc0(sizeof(ReceiverEntry));
    receiver_entry->connection = connection;

    g_object_ref(G_OBJECT(connection));

    g_signal_connect(G_OBJECT(connection), "message", G_CALLBACK(soup_websocket_message_cb), (gpointer)receiver_entry);

    error = NULL;

    GstElement *client_bin = gst_bin_new(NULL);
    receiver_entry->pipeline = client_bin;

    GstElement *queue = gst_element_factory_make("queue", "client_queue");
    GstElement *webrtcbin = gst_element_factory_make("webrtcbin", "webrtc");

    g_object_set(queue, "max-size-buffers", 100, "leaky", 2, // downstream
                 "flush-on-eos", TRUE, NULL);

    g_object_set(webrtcbin, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,"stun-server", stun, NULL);
    if (turn != NULL)
    {
      g_object_set(webrtcbin, "turn-server", turn, NULL);
    }

    // Set properties, add to bin
    gst_bin_add_many(GST_BIN(client_bin), queue, webrtcbin, NULL);
    gst_element_link_many(queue, webrtcbin, NULL);

    gst_bin_add(GST_BIN(webrtc_pipeline), receiver_entry->pipeline);

    // Add ghost pad to bin so we can link into it
    GstPad *sink_pad = gst_element_get_static_pad(queue, "sink");
    gst_element_add_pad(client_bin, gst_ghost_pad_new("sink", sink_pad));

    GstPadTemplate *tee_pad_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(video_tee), "src_%u");
    GstPad *tee_src_pad = gst_element_request_pad(video_tee, tee_pad_template, NULL, NULL);
    GstPad *queue_sink_pad = gst_element_get_static_pad(client_bin, "sink");
    GstPad *queue_src_pad = gst_element_get_static_pad(queue, "src");

    gst_pad_link(tee_src_pad, queue_sink_pad);
    gst_object_unref(queue_sink_pad);

    receiver_entry->webrtcbin = webrtcbin;
    receiver_entry->queue = queue;
    receiver_entry->client_ip = client_ip;
    receiver_entry->tee_src_pad = tee_src_pad;
    receiver_entry->sink_pad = sink_pad;

    if (error != NULL)
    {
      g_error("Could not create WebRTC sub-pipeline: %s\n", error->message);
      g_error_free(error);
      goto cleanup;
    }

    g_signal_connect(receiver_entry->webrtcbin, "on-negotiation-needed",
                     G_CALLBACK(on_negotiation_needed_cb), (gpointer)receiver_entry);

    g_signal_connect(receiver_entry->webrtcbin, "on-ice-candidate",
                     G_CALLBACK(on_ice_candidate_cb), (gpointer)receiver_entry);

    GstState state, pending;
    GstStateChangeReturn ret;
    ret = gst_element_set_state(receiver_entry->pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
      g_error("Could not start WebRTC sub-pipeline");
      gst_element_set_state(receiver_entry->pipeline, GST_STATE_NULL);
      gst_bin_remove(GST_BIN(webrtc_pipeline), receiver_entry->pipeline);
      g_object_unref(webrtcbin);
      g_object_unref(queue);
      g_object_unref(client_bin);
      goto cleanup;
    }
    else
    {
      // Wait for the state change to complete (timeout: 5 seconds)
      ret = gst_element_get_state(receiver_entry->pipeline, &state, &pending, 5 * GST_SECOND);
      if (ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL)
      {
        // g_print("WebRTC sub-pipeline reached PLAY state\n");
      }
      else
      {
        g_warning("Pipeline failed to reach NULL state properly");
      }
    }

    return receiver_entry;

  cleanup:
    destroy_receiver_entry((gpointer)receiver_entry);
    return NULL;
  }

  void destroy_receiver_entry(gpointer receiver_entry_ptr)
  {
    ReceiverEntry *receiver_entry = (ReceiverEntry *)receiver_entry_ptr;

    g_assert(receiver_entry != NULL);

    if (receiver_entry->connection != NULL)
      g_object_unref(G_OBJECT(receiver_entry->connection));

    g_slice_free1(sizeof(ReceiverEntry), receiver_entry);
  }

  void on_offer_created_cb(GstPromise *promise, gpointer user_data)
  {
    gchar *sdp_string;
    gchar *json_string;
    JsonObject *sdp_json;
    JsonObject *sdp_data_json;
    GstStructure const *reply;
    GstPromise *local_desc_promise;
    GstWebRTCSessionDescription *offer = NULL;
    ReceiverEntry *receiver_entry = (ReceiverEntry *)user_data;

    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                      &offer, NULL);
    gst_promise_unref(promise);

    local_desc_promise = gst_promise_new();
    g_signal_emit_by_name(receiver_entry->webrtcbin, "set-local-description",
                          offer, local_desc_promise);
    gst_promise_interrupt(local_desc_promise);
    gst_promise_unref(local_desc_promise);

    sdp_string = gst_sdp_message_as_text(offer->sdp);
    gst_print("Negotiation offer created:\n%s\n", sdp_string);

    sdp_json = json_object_new();
    json_object_set_string_member(sdp_json, "type", "sdp");

    sdp_data_json = json_object_new();
    json_object_set_string_member(sdp_data_json, "type", "offer");
    json_object_set_string_member(sdp_data_json, "sdp", sdp_string);
    json_object_set_object_member(sdp_json, "data", sdp_data_json);

    json_string = get_string_from_json_object(sdp_json);
    json_object_unref(sdp_json);

    soup_websocket_connection_send_text(receiver_entry->connection, json_string);
    g_free(json_string);
    g_free(sdp_string);

    gst_webrtc_session_description_free(offer);
  }

  void on_negotiation_needed_cb(GstElement *webrtcbin, gpointer user_data)
  {
    GstPromise *promise;
    ReceiverEntry *receiver_entry = (ReceiverEntry *)user_data;

    if (receiver_entry != NULL)
    {
      gst_print("Creating negotiation offer\n");

      promise = gst_promise_new_with_change_func(on_offer_created_cb, (gpointer)receiver_entry, NULL);
      g_signal_emit_by_name(G_OBJECT(webrtcbin), "create-offer", NULL, promise);
    }
    else
    {
      gst_print("\n Cannot create negotiation offer due to invalid connection! \n");
    }
  }

  void on_ice_candidate_cb(G_GNUC_UNUSED GstElement *webrtcbin, guint mline_index,
                           gchar *candidate, gpointer user_data)
  {
    JsonObject *ice_json;
    JsonObject *ice_data_json;
    gchar *json_string;
    ReceiverEntry *receiver_entry = (ReceiverEntry *)user_data;

    ice_json = json_object_new();
    json_object_set_string_member(ice_json, "type", "ice");

    ice_data_json = json_object_new();
    json_object_set_int_member(ice_data_json, "sdpMLineIndex", mline_index);
    json_object_set_string_member(ice_data_json, "candidate", candidate);
    json_object_set_string_member(ice_data_json, "sdpMid", "video0");
    json_object_set_object_member(ice_json, "data", ice_data_json);

    json_string = get_string_from_json_object(ice_json);
    json_object_unref(ice_json);

    soup_websocket_connection_send_text(receiver_entry->connection, json_string);
    g_free(json_string);
  }

  void soup_websocket_message_cb(G_GNUC_UNUSED SoupWebsocketConnection *connection,
                                 SoupWebsocketDataType data_type, GBytes *message, gpointer user_data)
  {
    gsize size;
    gchar *data;
    gchar *data_string;
    const gchar *type_string;
    JsonNode *root_json;
    JsonObject *root_json_object;
    JsonObject *data_json_object;
    JsonParser *json_parser = NULL;
    ReceiverEntry *receiver_entry = (ReceiverEntry *)user_data;

    switch (data_type)
    {
    case SOUP_WEBSOCKET_DATA_BINARY:
      g_error("Received unknown binary message, ignoring\n");
      g_bytes_unref(message);
      return;

    case SOUP_WEBSOCKET_DATA_TEXT:
      data = (gchar *)g_bytes_unref_to_data(message, &size);
      /* Convert to NULL-terminated string */
      data_string = g_strndup(data, size);
      g_free(data);
      break;

    default:
      g_assert_not_reached();
    }

    json_parser = json_parser_new();
    if (!json_parser_load_from_data(json_parser, data_string, -1, NULL))
      goto unknown_message;

    root_json = json_parser_get_root(json_parser);
    if (!JSON_NODE_HOLDS_OBJECT(root_json))
      goto unknown_message;

    root_json_object = json_node_get_object(root_json);

    if (!json_object_has_member(root_json_object, "type"))
    {
      g_error("Received message without type field\n");
      goto cleanup;
    }
    type_string = json_object_get_string_member(root_json_object, "type");

    if (!json_object_has_member(root_json_object, "data"))
    {
      g_error("Received message without data field\n");
      goto cleanup;
    }
    data_json_object = json_object_get_object_member(root_json_object, "data");

    if (g_strcmp0(type_string, "sdp") == 0)
    {
      const gchar *sdp_type_string;
      const gchar *sdp_string;
      GstPromise *promise;
      GstSDPMessage *sdp;
      GstWebRTCSessionDescription *answer;
      int ret;

      if (!json_object_has_member(data_json_object, "type"))
      {
        g_error("Received SDP message without type field\n");
        goto cleanup;
      }
      sdp_type_string = json_object_get_string_member(data_json_object, "type");

      if (g_strcmp0(sdp_type_string, "answer") != 0)
      {
        g_error("Expected SDP message type \"answer\", got \"%s\"\n",
                sdp_type_string);
        goto cleanup;
      }

      if (!json_object_has_member(data_json_object, "sdp"))
      {
        g_error("Received SDP message without SDP string\n");
        goto cleanup;
      }
      sdp_string = json_object_get_string_member(data_json_object, "sdp");

      gst_print("Received SDP:\n%s\n", sdp_string);

      ret = gst_sdp_message_new(&sdp);
      g_assert_cmphex(ret, ==, GST_SDP_OK);

      ret = gst_sdp_message_parse_buffer((guint8 *)sdp_string, strlen(sdp_string), sdp);
      if (ret != GST_SDP_OK)
      {
        g_error("Could not parse SDP string\n");
        goto cleanup;
      }

      answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
      g_assert_nonnull(answer);

      promise = gst_promise_new();
      g_signal_emit_by_name(receiver_entry->webrtcbin, "set-remote-description",
                            answer, promise);
      gst_promise_interrupt(promise);
      gst_promise_unref(promise);
      gst_webrtc_session_description_free(answer);
    }
    else if (g_strcmp0(type_string, "ice") == 0)
    {
      guint mline_index;
      const gchar *candidate_string;

      if (!json_object_has_member(data_json_object, "sdpMLineIndex"))
      {
        g_error("Received ICE message without mline index\n");
        goto cleanup;
      }
      mline_index =
          json_object_get_int_member(data_json_object, "sdpMLineIndex");

      if (!json_object_has_member(data_json_object, "candidate"))
      {
        g_error("Received ICE message without ICE candidate string\n");
        goto cleanup;
      }
      candidate_string = json_object_get_string_member(data_json_object, "candidate");

      // change abc.local to proper ip
      std::regex local_pattern(R"(\S+\.local)");
      std::string modified = std::regex_replace(std::string(candidate_string), local_pattern, std::string(receiver_entry->client_ip));
      candidate_string = modified.data();

      if (!candidate_string || strlen(candidate_string) == 0)
      {
        // Empty candidate, No need to add.
        goto cleanup;
      }

      gst_print("Received ICE candidate with mline index %u; candidate: %s\n", mline_index, candidate_string);

      g_signal_emit_by_name(receiver_entry->webrtcbin, "add-ice-candidate", mline_index, candidate_string);
    }
    else
      goto unknown_message;

  cleanup:
    if (json_parser != NULL)
      g_object_unref(G_OBJECT(json_parser));
    if (data_string != NULL)
      g_free(data_string);
    return;

  unknown_message:
    // g_error("Unknown message \"%s\", ignoring", data_string);
    g_warning("Unknown message \"%s\", ignoring", data_string);
    goto cleanup;
  }

  void soup_websocket_closed_cb(SoupWebsocketConnection *connection, gpointer user_data)
  {
    GHashTable *receiver_entry_table = (GHashTable *)user_data;
    ReceiverEntry *receiver_entry = (ReceiverEntry *)g_hash_table_lookup(receiver_entry_table, connection);

    receiver_entry->r_table = receiver_entry_table;

    gst_pad_add_probe(receiver_entry->tee_src_pad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, pad_probe_cb, (gpointer)receiver_entry, NULL);
  }

  // HTTP handler removed - now handled by WebControlServer
  
  void soup_websocket_handler(G_GNUC_UNUSED SoupServer *server,
                              SoupWebsocketConnection *connection, G_GNUC_UNUSED const char *path,
                              G_GNUC_UNUSED SoupClientContext *client_context, gpointer user_data)
  {
    ReceiverEntry *receiver_entry;
    GHashTable *receiver_entry_table = (GHashTable *)user_data;
    gchar *ip_str;

    gst_print("\nProcessing new websocket connection %p\n", (gpointer)connection);
    g_signal_connect(G_OBJECT(connection), "closed",
                     G_CALLBACK(soup_websocket_closed_cb), (gpointer)receiver_entry_table);

    if (available)
    {
      available = false;
    }
    else
    {
      g_print("\nSever still not available yet! \n");
      return;
    }

    GSocketAddress *sock_addr = soup_client_context_get_remote_address(client_context);
    if (G_IS_INET_SOCKET_ADDRESS(sock_addr))
    {
      GInetSocketAddress *inet_addr = G_INET_SOCKET_ADDRESS(sock_addr);
      GInetAddress *gaddr = g_inet_socket_address_get_address(inet_addr);
      ip_str = g_inet_address_to_string(gaddr);

      gst_print("\n Serving client with ip: %s \n", ip_str);
    }
    else
    {
      if (GST_IS_OBJECT(sock_addr))
        g_object_unref(sock_addr);
      gst_print("\nConnection did not established due to IP issue! \n");
      return;
    }

    gchar *temp = g_strdup(ip_str);
    receiver_entry = create_receiver_entry(connection, temp);

    // std::string hello_msg = "HELLO";
    // soup_websocket_connection_send_text(connection, hello_msg.c_str());

    g_hash_table_replace(receiver_entry_table, connection, receiver_entry);
    if (GST_IS_OBJECT(sock_addr))
      g_object_unref(sock_addr);
    if (ip_str != NULL)
      g_free(ip_str);
  }

  static gchar *
  get_string_from_json_object(JsonObject *object)
  {
    JsonNode *root;
    JsonGenerator *generator;
    gchar *text;

    /* Make it the root node */
    root = json_node_init_object(json_node_alloc(), object);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    text = json_generator_to_data(generator, NULL);

    /* Release everything */
    g_object_unref(generator);
    json_node_free(root);
    return text;
  }

#ifdef G_OS_UNIX
  gboolean
  exit_sighandler(gpointer user_data)
  {
    gst_print("Caught signal, stopping mainloop\n");

    if (webrtc_pipeline != NULL)
    {
      gst_element_set_state(webrtc_pipeline, GST_STATE_NULL);
      gst_object_unref(GST_OBJECT(webrtc_pipeline));
    }

    GMainLoop *mainloop = (GMainLoop *)user_data;
    g_main_loop_quit(mainloop);
    return TRUE;
  }
#endif

  static GOptionEntry entries[] = {
      // {"device", 0, 0, G_OPTION_ARG_STRING, &device,
      //  "Video device path (e.g., /dev/video0)",
      //  "DEVICE"},
      {"bitrate", 0, 0, G_OPTION_ARG_INT, &bitrate,
       "Bitrate of the output stream in kbps",
       "BITRATE"},
      {"fps", 0, 0, G_OPTION_ARG_INT, &fps,
       "Frame per second of the input stream",
       "FPS"},
      {"height", 0, 0, G_OPTION_ARG_INT, &height,
       "Height of the input video stream",
       "HEIGHT"},
      {"width", 0, 0, G_OPTION_ARG_INT, &width,
       "Width of the input video stream",
       "WIDTH"},
      {"codec", 0, 0, G_OPTION_ARG_STRING, &codec,
       "Video codec to use (h264 or h265)",
       "CODEC"},
      {"turn", 0, 0, G_OPTION_ARG_STRING, &turn,
       "TURN server to be used. ex: turn://username:password@1.2.3.4:1234",
       "TURN"},
      {"stun", 0, 0, G_OPTION_ARG_STRING, &stun,
       "STUN server to be used. ex: stun://stun.l.google.com:19302",
       "STUN"},
      {"client-ip", 0, 0, G_OPTION_ARG_STRING, &d_ip,
       "Client ip address for UDP sink",
       "CLIENT-IP"},
      {"client-port", 0, 0, G_OPTION_ARG_INT, &d_port,
       "Client port to use with the ip for UDP sink",
       "CLIENT-PORT"},
      {"audio-device", 0, 0, G_OPTION_ARG_STRING, &audio_device,
       "Audio device to use (e.g., hw:1,1). Leave empty to disable audio.",
       "AUDIO-DEVICE"},
      {"acodec", 0, 0, G_OPTION_ARG_STRING, &acodec,
       "Audio codec to use (aac or opus). Optional.",
       "ACODEC"},
      {"abitrate", 0, 0, G_OPTION_ARG_INT, &abitrate,
       "Audio bitrate in kbps. Default: 128",
       "ABITRATE"},
      {NULL},
  };


  int main(int argc, char *argv[])
  {
    GMainLoop *mainloop;
    SoupServer *soup_server;
    GHashTable *receiver_entry_table;
    GOptionContext *context;
    GError *error = NULL;

    // default client ip and port
    d_ip = g_strdup("192.168.25.90");
    d_port = 5004;

    setlocale(LC_ALL, "");

    context = g_option_context_new("- gstreamer webrtc sendonly demo");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_add_group(context, gst_init_get_option_group());
    if (!g_option_context_parse(context, &argc, &argv, &error))
    {
      g_printerr("Error initializing: %s\n", error->message);
      return -1;
    }

    g_print("Input Resolution: %dx%d\n", width, height);
     if (g_strcmp0(codec, "h265") == 0)
    {
      encoding = g_strdup_printf("omxh265enc target-bitrate=%d num-slices=1 "
                                 "control-rate=constant qp-mode=auto prefetch-buffer=true "
                                 "cpb-size=200 initial-delay=200 "
                                 "gdr-mode=disabled periodicity-idr=10 gop-length=10 filler-data=false "
                                 "! h265parse ! rtph265pay mtu=1400 ! "
                                 "application/x-rtp,media=video,encoding-name=H265,payload=96",
                                 bitrate);
      g_print("Output encoding: H265\n  Output bitrate: %d\n", bitrate);
    }
    else
    {
      encoding = g_strdup_printf("omxh264enc target-bitrate=%d num-slices=1 "
                                 "control-rate=constant qp-mode=auto prefetch-buffer=true "
                                 "cpb-size=200 initial-delay=200 "
                                 "gdr-mode=disabled periodicity-idr=10 gop-length=10 filler-data=false "
                                 "! h264parse ! rtph264pay mtu=1400 ! "
                                 "application/x-rtp,media=video,encoding-name=H264,payload=96",
                                 bitrate);
      g_print("Output encoding: H264\n Output bitrate: %d\n", bitrate);
    }
    // create a udpsink pipeline
    gchar *pipeline_string = NULL;

    g_print(" Input fps: %d\n", fps);
    g_print(" Client ip and port: %s:%d\n", d_ip, d_port);
    g_print(" Turn server: ");
    if (turn != NULL)
    {
      g_print("%s\n", turn);
    }
    else
    {
      g_print("not provided, will run without TURN server!\n");
    }

    // Set default audio device if not provided
    if (audio_device == NULL) {
      audio_device = g_strdup("hw:1,1");
    }
    
    g_print(" Audio device: %s\n", audio_device);

    // Create pipeline with video and optionally audio
    if (acodec != NULL && g_strcmp0(acodec, "none") != 0 && 
        audio_device && g_strcmp0(audio_device, "none") != 0) {
      
      // Determine audio encoding pipeline based on codec
      gchar *audio_encoding = NULL;
      
      if (g_strcmp0(acodec, "aac") == 0) {
        // AAC encoding pipeline
        audio_encoding = g_strdup_printf(
          "alsasrc device=%s ! "
          "audio/x-raw,rate=48000,channels=2,format=S16LE ! "
          "audioconvert ! audioresample ! "
          "queue max-size-buffers=10 leaky=downstream ! "
          "avenc_aac bitrate=%d compliance=-2 ! "
          "rtpmp4apay pt=97 ! "
          "application/x-rtp,media=audio,encoding-name=MPEG4-GENERIC,payload=97 ! "
          "tee name=at at. ! queue ! fakesink",
          audio_device, abitrate * 1000);
        g_print("✓ Audio enabled: AAC codec @ %d kbps\n", abitrate);
        
      } else if (g_strcmp0(acodec, "opus") == 0) {
        // Opus encoding pipeline
        audio_encoding = g_strdup_printf(
          "alsasrc device=%s ! "
          "audio/x-raw,rate=48000,channels=2,format=S16LE ! "
          "audioconvert ! audioresample ! "
          "queue max-size-buffers=10 leaky=downstream ! "
          "opusenc bitrate=%d frame-size=20 complexity=5 inband-fec=true ! "
          "rtpopuspay pt=97 ! "
          "application/x-rtp,media=audio,encoding-name=OPUS,payload=97 ! "
          "tee name=at at. ! queue ! fakesink",
          audio_device, abitrate * 1000);
        g_print("✓ Audio enabled: Opus codec @ %d kbps\n", abitrate);
        
      } else {
        g_printerr("⚠ Unknown audio codec '%s', audio disabled\n", acodec);
        audio_encoding = NULL;
      }
      
      if (audio_encoding) {
        // Pipeline with audio
        pipeline_string =
            g_strdup_printf("v4l2src device=/dev/video0 do-timestamp=false io-mode=4 ! video/x-raw, format=NV12, width=%d,height=%d,framerate=60/1! "
                      "videorate drop-only=true max-rate=%d ! " 
                            "queue ! %s ! tee name=t t. ! queue ! "
                            "udpsink clients=%s:%d auto-multicast=false "
                            "%s",
                            width, height, fps, encoding, d_ip, d_port, audio_encoding);
        g_free(audio_encoding);
      } else {
        // Fallback to video-only
        pipeline_string =
            g_strdup_printf("v4l2src device=/dev/video0 do-timestamp=false io-mode=4 ! video/x-raw, format=NV12, width=%d,height=%d,framerate=60/1! "
                      "videorate drop-only=true max-rate=%d ! " 
                            "queue ! %s ! tee name=t t. ! queue ! "
                            "udpsink clients=%s:%d auto-multicast=false",
                            width, height, fps, encoding, d_ip, d_port);
        g_print("⚠ Audio disabled (invalid codec)\n");
      }
      
    } else {
      // Pipeline without audio (video only)
      pipeline_string =
          g_strdup_printf("v4l2src device=/dev/video0 do-timestamp=false io-mode=4 ! video/x-raw, format=NV12, width=%d,height=%d,framerate=60/1! "
                    "videorate drop-only=true max-rate=%d ! " 
                          "queue ! %s ! tee name=t t. ! queue ! "
                          "udpsink clients=%s:%d auto-multicast=false",
                          width, height, fps, encoding, d_ip, d_port);
      g_print("⚠ Audio disabled (no codec specified)\n");
    }

    webrtc_pipeline = gst_parse_launch(pipeline_string, &error);
    g_free(pipeline_string);

    video_tee = gst_bin_get_by_name(GST_BIN(webrtc_pipeline), "t");
    g_assert(video_tee != NULL);
    
    // Get audio tee if audio is enabled
    audio_tee = gst_bin_get_by_name(GST_BIN(webrtc_pipeline), "at");
    if (audio_tee) {
      g_print("✓ Audio tee found\n");
    } else {
      g_print("⚠ Audio tee not found (audio disabled)\n");
    }

    if (error != NULL)
    {
      g_error("Could not create udpsink pipeline: %s\n", error->message);
      g_error_free(error);
      if (webrtc_pipeline != NULL)
      {
        gst_object_unref(webrtc_pipeline);
      }
      return -1;
    }

    GstBus *bus = gst_pipeline_get_bus(GST_PIPELINE(webrtc_pipeline));
    gst_bus_add_watch(bus, bus_watch_cb, NULL);
    gst_object_unref(bus);

    if (gst_element_set_state(webrtc_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
    {
      g_error("Could not start updsink pipeline");
      gst_element_set_state(GST_ELEMENT(webrtc_pipeline), GST_STATE_NULL);
      g_error_free(error);
      g_object_unref(webrtc_pipeline);
      return -1;
    }

    receiver_entry_table = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, destroy_receiver_entry);

    mainloop = g_main_loop_new(NULL, FALSE);
    g_assert(mainloop != NULL);

#ifdef G_OS_UNIX
    g_unix_signal_add(SIGINT, exit_sighandler, mainloop);
    g_unix_signal_add(SIGTERM, exit_sighandler, mainloop);
#endif

    soup_server = soup_server_new(SOUP_SERVER_SERVER_HEADER, "webrtc-soup-server", NULL);
    // Only WebSocket handler - HTTP is handled by WebControlServer
    soup_server_add_websocket_handler(soup_server, "/ws", NULL, NULL,
                                      soup_websocket_handler, (gpointer)receiver_entry_table, NULL);
    soup_server_listen_all(soup_server, SOUP_HTTP_PORT, (SoupServerListenOptions)0, NULL);

    gst_print("WebRTC Signaling Server (WebSocket only): ws://127.0.0.1:%d/ws\n", (gint)SOUP_HTTP_PORT);

    std::thread async_thread(update_availability);

    g_main_loop_run(mainloop);

    g_object_unref(G_OBJECT(soup_server));
    g_hash_table_destroy(receiver_entry_table);
    g_main_loop_unref(mainloop);

    gst_deinit();

    return 0;
  }
}
