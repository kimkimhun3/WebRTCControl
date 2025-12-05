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
#include <vector>

// Socket headers for TURN reachability check
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#define RTP_PAYLOAD_TYPE "96"
#define RTP_AUDIO_PAYLOAD_TYPE "97"
#define SOUP_HTTP_PORT 8081  // WebSocket signaling port (different from WebControlServer:8080)
#define MAX_WEBRTC_CLIENTS 4  // Maximum concurrent WebRTC viewers (UDP client is separate)

// g++ avpf.cpp -o avpf `pkg-config --cflags --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0 libsoup-2.4 json-glib-1.0` -std=c++17

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
  typedef struct _PendingIceCandidate PendingIceCandidate;

  // Structure to hold pending ICE candidates
  struct _PendingIceCandidate
  {
    guint mline_index;
    gchar *candidate;
  };

  static void enable_rtp_retransmission(GstElement *webrtcbin);
  static void enable_nack_on_transceivers(GstElement *webrtcbin);

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

  // Client limit tracking
  static int current_webrtc_clients = 0;

  struct _ReceiverEntry
  {
    SoupWebsocketConnection *connection;
    GHashTable *r_table;

    GstElement *pipeline;
    GstElement *webrtcbin;
    GstElement *queue;
    GstElement *audio_queue;  // Separate queue for audio
    gchar *client_ip;
    GstPad *tee_video_src_pad;
    GstPad *video_sink_pad;
    GstPad *tee_audio_src_pad;  // Audio tee source pad
    GstPad *audio_sink_pad;     // Audio sink pad
    
    // ICE candidate buffering for race condition prevention
    std::vector<PendingIceCandidate*> *pending_ice_candidates;
    gboolean remote_description_set;
    
    // Prevent double negotiation
    gboolean offer_created;
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

      // Unlink video pads
      gst_pad_unlink(receiver_entry->tee_video_src_pad, receiver_entry->video_sink_pad);
      gst_element_release_request_pad(video_tee, receiver_entry->tee_video_src_pad);
      gst_object_unref(receiver_entry->tee_video_src_pad);
      gst_object_unref(receiver_entry->video_sink_pad);

      // Unlink audio pads if audio is enabled
      if (audio_tee && receiver_entry->tee_audio_src_pad && receiver_entry->audio_sink_pad)
      {
        gst_pad_unlink(receiver_entry->tee_audio_src_pad, receiver_entry->audio_sink_pad);
        gst_element_release_request_pad(audio_tee, receiver_entry->tee_audio_src_pad);
        gst_object_unref(receiver_entry->tee_audio_src_pad);
        gst_object_unref(receiver_entry->audio_sink_pad);
      }

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
          if (receiver_entry->audio_queue && GST_IS_OBJECT(receiver_entry->audio_queue))
            gst_object_unref(receiver_entry->audio_queue);
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

    gst_pad_add_probe(receiver_entry->video_sink_pad, (GstPadProbeType)(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), event_probe_cb, user_data, NULL);
    
    // Also add probe to audio if it exists
    if (audio_tee && receiver_entry->audio_sink_pad)
    {
      gst_pad_add_probe(receiver_entry->audio_sink_pad, (GstPadProbeType)(GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), event_probe_cb, user_data, NULL);
    }

    gst_pad_send_event(receiver_entry->video_sink_pad, gst_event_new_eos());

    return GST_PAD_PROBE_OK;
  }

  //////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

  void update_availability()
  {
    static int stats_counter = 0;
    
    while (true)
    {
      std::this_thread::sleep_for(std::chrono::seconds(waiting_period));
      available = true;
      
      // Every 30 seconds, print RTX statistics if available
      stats_counter++;
      if (stats_counter >= 6) { // 6 * 5 seconds = 30 seconds
        stats_counter = 0;
        g_print("\n📊 Periodic RTX Stats Check (every 30s)\n");
        g_print("   Check chrome://webrtc-internals for:\n");
        g_print("   - nackCount (should be > 0 if packet loss)\n");
        g_print("   - retransmittedPacketsReceived (should be > 0 if RTX works)\n\n");
      }
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

  // Function to check if TURN server is reachable via TCP
  bool isTurnReachableTCP(const std::string &host, int port = 3478, int timeoutMs = 1000)
  {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
      return false;

    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0)
    {
      close(sock);
      return false;
    }

    bool ok = (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    close(sock);
    return ok;
  }

  // Process buffered ICE candidates after remote description is set
  void process_pending_ice_candidates(ReceiverEntry *receiver_entry)
  {
    if (!receiver_entry->pending_ice_candidates || receiver_entry->pending_ice_candidates->empty())
    {
      return;
    }

    g_print("📦 Processing %zu buffered ICE candidates\n", receiver_entry->pending_ice_candidates->size());

    for (auto *pending : *receiver_entry->pending_ice_candidates)
    {
      g_print("✓ Adding buffered ICE candidate: mline=%u, candidate=%s\n", 
              pending->mline_index, pending->candidate);
      
      g_signal_emit_by_name(receiver_entry->webrtcbin, "add-ice-candidate", 
                           pending->mline_index, pending->candidate);
      
      g_free(pending->candidate);
      delete pending;
    }

    receiver_entry->pending_ice_candidates->clear();
    g_print("✓ All buffered ICE candidates processed\n");
  }





  static void enable_rtp_retransmission (GstElement *webrtcbin)
  {
    GstElement *rtpbin = NULL;

    /* webrtcbin exposes the internal rtpbin via the "rtpbin" property */
    /* This property may not exist in older GStreamer versions */
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(webrtcbin), "rtpbin")) {
      g_object_get (webrtcbin, "rtpbin", &rtpbin, NULL);
      if (rtpbin) {
        /* This makes rtpjitterbuffer/rtpbin actually respond to RTCP NACKs */
        g_object_set (rtpbin, "do-retransmission", TRUE, NULL);
        g_print ("✓ Enabled RTP retransmission on rtpbin\n");
        gst_object_unref (rtpbin);
      }
    } else {
      g_print ("⚠️  rtpbin property not available, will try alternative method\n");
    }
  }

  // NEW: Monitor RTX packets being sent
  static GstPadProbeReturn rtx_probe_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
  {
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
      static guint64 rtx_count = 0;
      static guint64 last_log_count = 0;
      
      rtx_count++;
      
      // Log every 100 RTX packets
      if (rtx_count - last_log_count >= 100) {
        g_print("📤 RTX: %lu retransmission packets sent (total)\n", rtx_count);
        last_log_count = rtx_count;
      }
    }
    
    return GST_PAD_PROBE_OK;
  }

  // NEW: Callback to configure RTP session when it's created
  static void on_rtpbin_new_session_cb(GstElement *rtpbin, guint session_id, GstElement *session, gpointer user_data)
  {
    g_print("📡 RTP session %u created, enabling retransmission\n", session_id);
    
    // Enable retransmission on this session
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(session), "do-retransmission")) {
      g_object_set(session, "do-retransmission", TRUE, NULL);
      g_print("✓ Enabled do-retransmission on RTP session %u\n", session_id);
    }
    
    // Also enable NACK feedback
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(session), "rtcp-nack-mode")) {
      g_object_set(session, "rtcp-nack-mode", 1, NULL); // 1 = always send NACKs
      g_print("✓ Enabled RTCP NACK mode on session %u\n", session_id);
    }
  }

  // NEW: Callback to configure rtpbin when webrtcbin creates it
  static void on_deep_element_added_cb(GstBin *bin, GstBin *sub_bin, GstElement *element, gpointer user_data)
  {
    gchar *name = gst_element_get_name(element);
    
    if (g_str_has_prefix(name, "rtpbin")) {
      g_print("🔧 Found rtpbin: %s, configuring for retransmission...\n", name);
      
      // Enable retransmission on rtpbin
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(element), "do-retransmission")) {
        g_object_set(element, "do-retransmission", TRUE, NULL);
        g_print("✓ Enabled do-retransmission on %s\n", name);
      } else {
        g_print("❌ do-retransmission property NOT FOUND on rtpbin!\n");
        g_print("   This GStreamer version may not support NACK retransmission\n");
        g_print("   Try: export GST_DEBUG=rtprtxsend:5,rtpsession:5\n");
      }
      
      // Connect to new-session signal to configure each RTP session
      g_signal_connect(element, "on-new-session", 
                      G_CALLBACK(on_rtpbin_new_session_cb), user_data);
      g_print("✓ Connected to on-new-session signal for %s\n", name);
      
      // Try to enable RTX send debug
      g_print("💡 To debug RTX: GST_DEBUG=rtprtxsend:6,rtpsession:5 ./avpf2 ...\n");
    }
    
    // Also monitor for rtprtxsend element (the actual RTX sender)
    if (g_str_has_prefix(name, "rtprtxsend")) {
      g_print("🎯 Found RTX sender: %s\n", name);
      
      // Get statistics from rtprtxsend
      GstStructure *stats = NULL;
      g_object_get(element, "stats", &stats, NULL);
      if (stats) {
        gchar *stats_str = gst_structure_to_string(stats);
        g_print("   RTX stats: %s\n", stats_str);
        g_free(stats_str);
        gst_structure_free(stats);
      }
    }
    
    g_free(name);
  }

  static void enable_nack_on_transceivers (GstElement *webrtcbin)
  {
    GArray *transceivers = NULL;

    /* Ask webrtcbin for all transceivers */
    g_signal_emit_by_name (webrtcbin, "get-transceivers", &transceivers);
    if (!transceivers)
      return;

    for (guint i = 0; i < transceivers->len; i++) {
      GstWebRTCRTPTransceiver *tr =
        g_array_index (transceivers, GstWebRTCRTPTransceiver *, i);
      GstWebRTCRTPTransceiverDirection dir;

      g_object_get (tr, "direction", &dir, NULL);
      if (dir == GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY ||
          dir == GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV) {
        /* This turns on NACK (and RTX internally) for sender side */
        g_object_set (tr, "do-nack", TRUE, NULL);
        g_print ("✓ Enabled NACK on transceiver %u\n", i);
      }
    }

    g_array_unref (transceivers);
  }

  // NEW: Callback when a transceiver is added dynamically
  static void on_transceiver_added_cb(GstElement *webrtcbin, GstWebRTCRTPTransceiver *trans, gpointer user_data)
  {
    g_print("📡 Transceiver added dynamically\n");
    
    // Check if direction property exists
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(trans), "direction")) {
      GstWebRTCRTPTransceiverDirection dir;
      g_object_get(trans, "direction", &dir, NULL);
      g_print("   Direction: %d\n", dir);
      
      if (dir == GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY ||
          dir == GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDRECV) {
          
        // Enable NACK
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(trans), "do-nack")) {
          g_object_set(trans, "do-nack", TRUE, NULL);
          g_print("✓ Enabled NACK on dynamically added transceiver\n");
        }
      }
    } else {
      // For older GStreamer, just try to enable NACK directly
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(trans), "do-nack")) {
        g_object_set(trans, "do-nack", TRUE, NULL);
        g_print("✓ Enabled NACK on transceiver (no direction property)\n");
      }
    }
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

    // Initialize ICE candidate buffering
    receiver_entry->pending_ice_candidates = new std::vector<PendingIceCandidate*>();
    receiver_entry->remote_description_set = FALSE;
    receiver_entry->offer_created = FALSE;  // Initialize to prevent double negotiation

    g_object_ref(G_OBJECT(connection));

    g_signal_connect(G_OBJECT(connection), "message", G_CALLBACK(soup_websocket_message_cb), (gpointer)receiver_entry);

    error = NULL;

    GstElement *client_bin = gst_bin_new(NULL);
    receiver_entry->pipeline = client_bin;

    GstElement *queue = gst_element_factory_make("queue", "video_queue");
    GstElement *webrtcbin = gst_element_factory_make("webrtcbin", "webrtc");

    g_object_set(queue, "max-size-buffers", 100, "leaky", 2, // downstream
                 "flush-on-eos", TRUE, NULL);

    // Check if audio is enabled
    gboolean audio_enabled = (audio_tee != NULL);
    GstElement *audio_queue = NULL;
    
    // ============================================================================
    // CRITICAL SECTION: Enable retransmission BEFORE any transceiver creation
    // ============================================================================
    g_print("\n🔧 Configuring WebRTC for NACK/RTX...\n");
    
    // Try direct rtpbin access first
    enable_rtp_retransmission(webrtcbin);
    
    // Also connect to deep-element-added to catch rtpbin when it's created internally
    // This is crucial for GStreamer versions where rtpbin property isn't exposed
    g_signal_connect(webrtcbin, "deep-element-added", 
                     G_CALLBACK(on_deep_element_added_cb), receiver_entry);
    g_print("✓ Connected to deep-element-added signal\n");
    
    // Connect to transceiver signal BEFORE any other setup
    g_signal_connect(webrtcbin, "on-new-transceiver", 
                     G_CALLBACK(on_transceiver_added_cb), receiver_entry);
    
    if (audio_enabled)
    {
      audio_queue = gst_element_factory_make("queue", "audio_queue");
      g_object_set(audio_queue, "max-size-buffers", 100, "leaky", 2, // downstream
                   "flush-on-eos", TRUE, NULL);
      receiver_entry->audio_queue = audio_queue;
    }

    g_object_set(webrtcbin, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, NULL);
    
    // Only set STUN server if provided
    if (stun != NULL)
    {
      g_print("Setting STUN server: %s\n", stun);
      g_object_set(webrtcbin, "stun-server", stun, NULL);
    }
    else
    {
      g_print("No STUN server provided, skipping STUN configuration\n");
    }
    
    // Only set TURN server if provided AND reachable
    try
    {
      if (turn != NULL)
      {
        g_print("Checking TURN server reachability: %s\n", turn);
        
        // Parse TURN URL to extract IP and port
        std::string temp_url(turn);
        std::string temp_ip;
        std::string temp_port;
        
        // Extract IP/hostname using regex
        std::regex ip_pattern(R"((\b\d{1,3}(?:\.\d{1,3}){3}\b|[a-zA-Z0-9.-]+\.[a-zA-Z]{2,}))");
        std::regex port_pattern(R"(:(\d{1,6})\b)");
        std::smatch match;
        
        if (std::regex_search(temp_url, match, ip_pattern))
        {
          temp_ip = match[1];
        }
        if (std::regex_search(temp_url, match, port_pattern))
        {
          temp_port = match[1];
        }
        
        // Check if we successfully parsed IP and port
        if (!temp_ip.empty() && !temp_port.empty())
        {
          if (isTurnReachableTCP(temp_ip, std::stoi(temp_port)))
          {
            g_print("✓ TURN server is reachable, configuring: %s\n", turn);
            g_object_set(webrtcbin, "turn-server", turn, NULL);
          }
          else
          {
            g_print("✗ TURN server is NOT reachable, skipping TURN configuration!\n");
            g_print("  WebRTC will attempt direct connection or use STUN if available.\n");
          }
        }
        else
        {
          g_print("✗ Failed to parse TURN URL, skipping TURN configuration!\n");
        }
      }
      else
      {
        g_print("No TURN server provided, skipping TURN configuration\n");
      }
    }
    catch (const std::exception& e)
    {
      g_print("✗ TURN server configuration error: %s\n", e.what());
      g_print("  Skipping TURN configuration, will use direct connection or STUN.\n");
    }
    catch (...)
    {
      g_print("✗ Unknown error parsing TURN server URL, skipping TURN configuration!\n");
    }

    // ============================================================================
    // CRITICAL: Create transceivers BEFORE adding to pipeline
    // This ensures NACK is configured before SDP negotiation
    // ============================================================================
    
    g_print("📡 Creating video transceiver with NACK enabled...\n");
    
    // Create video transceiver explicitly
    GstWebRTCRTPTransceiver *video_trans = NULL;
    g_signal_emit_by_name(webrtcbin, "add-transceiver", 
                         GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
                         NULL, &video_trans);
    
    if (video_trans) {
        g_print("✓ Video transceiver created\n");
        
        // Enable NACK on the transceiver if property exists
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(video_trans), "do-nack")) {
          g_object_set(video_trans, "do-nack", TRUE, NULL);
          g_print("✓ Enabled do-nack=TRUE on video transceiver\n");
        } else {
          g_print("⚠️  do-nack property not available\n");
        }
        
        // Try to set codec preferences if property exists
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(video_trans), "codec-preferences")) {
          GstCaps *video_caps = gst_caps_from_string(
              "application/x-rtp,media=video,encoding-name=H264,payload=96,"
              "rtcp-fb-nack=1,rtcp-fb-nack-pli=1"
          );
          
          if (video_caps) {
              g_object_set(video_trans, "codec-preferences", video_caps, NULL);
              gchar *caps_str = gst_caps_to_string(video_caps);
              g_print("✓ Set codec preferences: %s\n", caps_str);
              g_free(caps_str);
              gst_caps_unref(video_caps);
          }
        } else {
          g_print("⚠️  codec-preferences property not available (will use SDP modification)\n");
        }
        
        gst_object_unref(video_trans);
    } else {
        g_print("✗ Failed to create video transceiver!\n");
    }
    
    // Create audio transceiver if audio is enabled
    if (audio_enabled) {
        g_print("📡 Creating audio transceiver...\n");
        
        GstWebRTCRTPTransceiver *audio_trans = NULL;
        g_signal_emit_by_name(webrtcbin, "add-transceiver", 
                             GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
                             NULL, &audio_trans);
        
        if (audio_trans) {
            g_print("✓ Audio transceiver created\n");
            
            // Audio also benefits from NACK if property exists
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(audio_trans), "do-nack")) {
              g_object_set(audio_trans, "do-nack", TRUE, NULL);
              g_print("✓ Enabled NACK on audio transceiver\n");
            }
            
            // Try to set audio codec preferences if property exists
            if (g_object_class_find_property(G_OBJECT_GET_CLASS(audio_trans), "codec-preferences")) {
              const gchar *audio_codec_name = (acodec && g_strcmp0(acodec, "opus") == 0) ? "OPUS" : "MP4A-LATM";
              gchar *audio_caps_str = g_strdup_printf(
                  "application/x-rtp,media=audio,encoding-name=%s,payload=97",
                  audio_codec_name
              );
              GstCaps *audio_caps = gst_caps_from_string(audio_caps_str);
              
              if (audio_caps) {
                  g_object_set(audio_trans, "codec-preferences", audio_caps, NULL);
                  g_print("✓ Set audio codec preferences: %s\n", audio_caps_str);
                  gst_caps_unref(audio_caps);
              }
              g_free(audio_caps_str);
            }
            
            gst_object_unref(audio_trans);
        }
    }
    
    g_print("✅ Transceivers configured with NACK support\n\n");

    // Add elements to bin and link
    if (audio_enabled)
    {
      gst_bin_add_many(GST_BIN(client_bin), queue, audio_queue, webrtcbin, NULL);
      gst_element_link_many(queue, webrtcbin, NULL);
      gst_element_link_many(audio_queue, webrtcbin, NULL);
    }
    else
    {
      gst_bin_add_many(GST_BIN(client_bin), queue, webrtcbin, NULL);
      gst_element_link_many(queue, webrtcbin, NULL);
    }

    gst_bin_add(GST_BIN(webrtc_pipeline), receiver_entry->pipeline);

    // Add ghost pad for video
    GstPad *video_sink_pad = gst_element_get_static_pad(queue, "sink");
    gst_element_add_pad(client_bin, gst_ghost_pad_new("video_sink", video_sink_pad));

    GstPadTemplate *tee_pad_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(video_tee), "src_%u");
    GstPad *tee_video_src_pad = gst_element_request_pad(video_tee, tee_pad_template, NULL, NULL);
    GstPad *queue_video_sink_pad = gst_element_get_static_pad(client_bin, "video_sink");

    gst_pad_link(tee_video_src_pad, queue_video_sink_pad);
    gst_object_unref(queue_video_sink_pad);

    // Add ghost pad for audio if enabled
    if (audio_enabled)
    {
      GstPad *audio_sink_pad = gst_element_get_static_pad(audio_queue, "sink");
      gst_element_add_pad(client_bin, gst_ghost_pad_new("audio_sink", audio_sink_pad));

      tee_pad_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(audio_tee), "src_%u");
      GstPad *tee_audio_src_pad = gst_element_request_pad(audio_tee, tee_pad_template, NULL, NULL);
      GstPad *queue_audio_sink_pad = gst_element_get_static_pad(client_bin, "audio_sink");

      gst_pad_link(tee_audio_src_pad, queue_audio_sink_pad);
      gst_object_unref(queue_audio_sink_pad);

      receiver_entry->tee_audio_src_pad = tee_audio_src_pad;
      receiver_entry->audio_sink_pad = audio_sink_pad;
      
      g_print("✓ Audio linked to webrtcbin\n");
    }

    receiver_entry->webrtcbin = webrtcbin;
    receiver_entry->queue = queue;
    receiver_entry->client_ip = client_ip;
    receiver_entry->tee_video_src_pad = tee_video_src_pad;
    receiver_entry->video_sink_pad = video_sink_pad;

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
      if (audio_queue)
        g_object_unref(audio_queue);
      g_object_unref(client_bin);
      goto cleanup;
    }
    else
    {
      // Wait for the state change to complete (timeout: 5 seconds)
      ret = gst_element_get_state(receiver_entry->pipeline, &state, &pending, 5 * GST_SECOND);
      if (ret == GST_STATE_CHANGE_SUCCESS || ret == GST_STATE_CHANGE_NO_PREROLL)
      {
        g_print("WebRTC sub-pipeline reached PLAY state\n");
      }
      else
      {
        g_warning("Pipeline failed to reach PLAY state properly");
      }
    }

    return receiver_entry;

  cleanup:
    if (receiver_entry->pending_ice_candidates)
    {
      for (auto *pending : *receiver_entry->pending_ice_candidates)
      {
        g_free(pending->candidate);
        delete pending;
      }
      delete receiver_entry->pending_ice_candidates;
    }
    g_slice_free(ReceiverEntry, receiver_entry);
    return NULL;
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

    // ============================================================================
    // CRITICAL: Manually modify SDP to add NACK and RTX support
    // GStreamer's webrtcbin doesn't always generate these automatically
    // ============================================================================
    
    g_print("\n🔧 Manually modifying SDP to add NACK and RTX...\n");
    
    // Create a COPY of the SDP message (the original is const/immutable)
    GstSDPMessage *sdp_copy = NULL;
    gst_sdp_message_new(&sdp_copy);
    gst_sdp_message_copy(offer->sdp, &sdp_copy);
    
    // Find the video media section (usually index 0)
    for (guint i = 0; i < gst_sdp_message_medias_len(sdp_copy); i++) {
        const GstSDPMedia *media = gst_sdp_message_get_media(sdp_copy, i);
        
        if (g_strcmp0(gst_sdp_media_get_media(media), "video") == 0) {
            g_print("✓ Found video media section at index %u\n", i);
            
            // We need to modify the media - cast away const (safe because we copied)
            GstSDPMedia *writable_media = (GstSDPMedia *)media;
            
            // Check if "nack" (without pli) already exists
            gboolean has_nack = FALSE;
            gboolean has_nack_pli = FALSE;
            gboolean has_rtx = FALSE;
            
            for (guint j = 0; j < gst_sdp_media_attributes_len(media); j++) {
                const GstSDPAttribute *attr = gst_sdp_media_get_attribute(media, j);
                if (g_strcmp0(attr->key, "rtcp-fb") == 0) {
                    if (g_strcmp0(attr->value, "96 nack") == 0) {
                        has_nack = TRUE;
                    }
                    if (g_strcmp0(attr->value, "96 nack pli") == 0) {
                        has_nack_pli = TRUE;
                    }
                }
                if (g_strcmp0(attr->key, "rtpmap") == 0) {
                    if (strstr(attr->value, "rtx") != NULL) {
                        has_rtx = TRUE;
                    }
                }
            }
            
            // Add missing NACK attribute (generic, not just pli)
            if (!has_nack) {
                gst_sdp_media_add_attribute(writable_media, "rtcp-fb", "96 nack");
                g_print("✓ Added 'a=rtcp-fb:96 nack'\n");
            }
            
            // Ensure nack pli exists
            if (!has_nack_pli) {
                gst_sdp_media_add_attribute(writable_media, "rtcp-fb", "96 nack pli");
                g_print("✓ Added 'a=rtcp-fb:96 nack pli'\n");
            }
            
            // Add RTX payload type if not present
            if (!has_rtx) {
                // Add RTX payload type 97 to the m= line
                gst_sdp_media_add_format(writable_media, "97");
                
                // Add RTX mapping
                gst_sdp_media_add_attribute(writable_media, "rtpmap", "97 rtx/90000");
                g_print("✓ Added 'a=rtpmap:97 rtx/90000'\n");
                
                // Add RTX association with main payload type
                gst_sdp_media_add_attribute(writable_media, "fmtp", "97 apt=96");
                g_print("✓ Added 'a=fmtp:97 apt=96'\n");
            }
            
            break;
        }
    }
    
    g_print("✅ SDP modification complete\n\n");
    
    // Create a new offer with the modified SDP
    GstWebRTCSessionDescription *modified_offer = 
        gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp_copy);
    
    // Now set the modified SDP as local description
    local_desc_promise = gst_promise_new();
    g_signal_emit_by_name(G_OBJECT(receiver_entry->webrtcbin),
                          "set-local-description", modified_offer, local_desc_promise);
    gst_promise_interrupt(local_desc_promise);
    gst_promise_unref(local_desc_promise);

    sdp_string = gst_sdp_message_as_text(modified_offer->sdp);
    gst_print("Sending offer (after modification):\n%s\n", sdp_string);
    
    // ============================================================================
    // VERIFICATION: Check if NACK and RTX are in the SDP
    // ============================================================================
    g_print("\n🔍 SDP Verification:\n");
    
    if (strstr(sdp_string, "a=rtcp-fb:96 nack\n") || strstr(sdp_string, "a=rtcp-fb:96 nack ")) {
        g_print("✅ SDP contains 'a=rtcp-fb:96 nack' (generic NACK)\n");
    } else {
        g_print("❌ SDP STILL missing 'a=rtcp-fb:96 nack'\n");
    }
    
    if (strstr(sdp_string, "a=rtcp-fb:96 nack pli")) {
        g_print("✅ SDP contains 'a=rtcp-fb:96 nack pli'\n");
    }
    
    if (strstr(sdp_string, "rtx")) {
        g_print("✅ SDP contains RTX payload type\n");
    } else {
        g_print("❌ SDP STILL missing RTX payload type\n");
    }
    
    if (strstr(sdp_string, "a=rtpmap:97 rtx")) {
        g_print("✅ SDP contains 'a=rtpmap:97 rtx/90000'\n");
    } else {
        g_print("⚠️  SDP missing 'a=rtpmap:97 rtx/90000'\n");
    }
    
    if (strstr(sdp_string, "a=fmtp:97 apt=96")) {
        g_print("✅ SDP contains 'a=fmtp:97 apt=96' (RTX association)\n");
    } else {
        g_print("⚠️  SDP missing 'a=fmtp:97 apt=96'\n");
    }
    g_print("\n");

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

    gst_webrtc_session_description_free(modified_offer);
    gst_webrtc_session_description_free(offer);
  }

  // MODIFIED: Prevent double negotiation!
  void on_negotiation_needed_cb(GstElement *webrtcbin, gpointer user_data)
  {
    GstPromise *promise;
    ReceiverEntry *receiver_entry = (ReceiverEntry *)user_data;
    
    // CRITICAL: Prevent double negotiation
    if (receiver_entry->offer_created) {
      g_print("⚠️  Negotiation already in progress, ignoring duplicate negotiation-needed signal\n");
      return;
    }
    
    receiver_entry->offer_created = TRUE;
    
    // DON'T call enable_nack_on_transceivers here - transceivers are configured earlier!

    gst_print("Creating offer\n");
    promise = gst_promise_new_with_change_func(on_offer_created_cb,
                                               (gpointer)receiver_entry, NULL);
    g_signal_emit_by_name(G_OBJECT(receiver_entry->webrtcbin),
                          "create-offer", NULL, promise);
  }

  void on_ice_candidate_cb(GstElement *webrtcbin, guint mline_index,
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
    json_object_set_object_member(ice_json, "data", ice_data_json);

    json_string = get_string_from_json_object(ice_json);
    json_object_unref(ice_json);

    soup_websocket_connection_send_text(receiver_entry->connection, json_string);
    g_free(json_string);
  }

  void soup_websocket_message_cb(SoupWebsocketConnection *connection,
                                 SoupWebsocketDataType data_type, GBytes *message, gpointer user_data)
  {
    gsize size;
    const gchar *data;
    gchar *data_string;
    const gchar *type_string;
    const gchar *sdp_type_string;
    JsonNode *root_json;
    JsonObject *root_json_object;
    JsonObject *data_json_object;
    JsonParser *json_parser = NULL;
    ReceiverEntry *receiver_entry = (ReceiverEntry *)user_data;

    switch (data_type)
    {
    case SOUP_WEBSOCKET_DATA_BINARY:
      g_error("Received unknown binary message, ignoring\n");
      return;

    case SOUP_WEBSOCKET_DATA_TEXT:
      data = (const gchar *)g_bytes_get_data(message, &size);
      /* Convert to NULL-terminated string */
      data_string = g_strndup(data, size);
      break;

    default:
      g_assert_not_reached();
    }

    json_parser = json_parser_new();
    if (!json_parser_load_from_data(json_parser, data_string, -1, NULL))
    {
      goto unknown_message;
    }

    root_json = json_parser_get_root(json_parser);
    if (!JSON_NODE_HOLDS_OBJECT(root_json))
    {
      goto unknown_message;
    }

    root_json_object = json_node_get_object(root_json);

    if (!json_object_has_member(root_json_object, "type"))
    {
      goto unknown_message;
    }

    type_string = json_object_get_string_member(root_json_object, "type");
    if (!type_string)
    {
      goto unknown_message;
    }

    if (!json_object_has_member(root_json_object, "data"))
    {
      g_error("Received message without data field\n");
      goto cleanup;
    }
    data_json_object = json_object_get_object_member(root_json_object, "data");

    if (g_strcmp0(type_string, "sdp") == 0)
    {
      int ret;
      GstSDPMessage *sdp;
      const gchar *sdp_string;
      GstWebRTCSessionDescription *answer;
      GstPromise *promise;

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

      // CRITICAL: Mark remote description as set
      receiver_entry->remote_description_set = TRUE;
      g_print("✓ Remote description set, processing pending ICE candidates\n");

      // Process any pending ICE candidates
      process_pending_ice_candidates(receiver_entry);
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

      // CRITICAL: Buffer ICE candidate if remote description not yet set
      if (!receiver_entry->remote_description_set)
      {
        g_print("📦 Buffering ICE candidate (remote description not set yet)\n");
        
        PendingIceCandidate *pending = new PendingIceCandidate;
        pending->mline_index = mline_index;
        pending->candidate = g_strdup(candidate_string);
        
        receiver_entry->pending_ice_candidates->push_back(pending);
        
        g_print("   Queue size: %zu\n", receiver_entry->pending_ice_candidates->size());
      }
      else
      {
        // Remote description is set, add immediately
        g_signal_emit_by_name(receiver_entry->webrtcbin, "add-ice-candidate", 
                             mline_index, candidate_string);
        g_print("✓ ICE candidate added immediately\n");
      }
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

  // Helper function: Send JSON status message
  static void send_status_message(SoupWebsocketConnection *connection, 
                                  const gchar *status, 
                                  const gchar *message,
                                  int queue_position)
  {
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "type");
    json_builder_add_string_value(builder, "status");
    
    json_builder_set_member_name(builder, "status");
    json_builder_add_string_value(builder, status);
    
    if (message) {
      json_builder_set_member_name(builder, "message");
      json_builder_add_string_value(builder, message);
    }
    
    if (queue_position > 0) {
      json_builder_set_member_name(builder, "queue_position");
      json_builder_add_int_value(builder, queue_position);
    }
    
    json_builder_end_object(builder);
    
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gchar *json_str = json_generator_to_data(generator, NULL);
    
    soup_websocket_connection_send_text(connection, json_str);
    
    g_free(json_str);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
  }

  // Helper function: Get client IP from context
  static gchar *get_client_ip_from_context(SoupClientContext *client_context)
  {
    if (!client_context) return g_strdup("unknown");
    
    GSocketAddress *addr = soup_client_context_get_remote_address(client_context);
    if (!addr) return g_strdup("unknown");
    
    if (G_IS_INET_SOCKET_ADDRESS(addr)) {
      GInetAddress *inet_addr = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(addr));
      return g_inet_address_to_string(inet_addr);
    }
    
    return g_strdup("unknown");
  }

  void soup_websocket_closed_cb(SoupWebsocketConnection *connection, gpointer user_data)
  {
    GHashTable *receiver_entry_table = (GHashTable *)user_data;
    ReceiverEntry *receiver_entry = (ReceiverEntry *)g_hash_table_lookup(receiver_entry_table, connection);

    receiver_entry->r_table = receiver_entry_table;

    gst_pad_add_probe(receiver_entry->tee_video_src_pad, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM, pad_probe_cb, (gpointer)receiver_entry, NULL);
  }

  // HTTP handler removed - now handled by WebControlServer
  
  void soup_websocket_handler(G_GNUC_UNUSED SoupServer *server,
                              SoupWebsocketConnection *connection, G_GNUC_UNUSED const char *path,
                              G_GNUC_UNUSED SoupClientContext *client_context, gpointer user_data)
  {
    GHashTable *receiver_entry_table = (GHashTable *)user_data;
    gchar *client_ip = get_client_ip_from_context(client_context);
    
    g_print("\n🔌 New WebSocket connection from: %s\n", client_ip);
    g_print("   Current clients: %d/%d\n", current_webrtc_clients, MAX_WEBRTC_CLIENTS);
    
    // Check if we've reached the limit
    if (current_webrtc_clients >= MAX_WEBRTC_CLIENTS) {
      g_print("🚫 Client limit reached! Rejecting %s\n", client_ip);
      
      // Send busy status and close connection
      send_status_message(connection, "busy", 
                         "Server busy. Please try again later.", 0);
      
      // Close the connection after a short delay to ensure message is sent
      g_timeout_add(100, [](gpointer data) -> gboolean {
        SoupWebsocketConnection *conn = (SoupWebsocketConnection *)data;
        soup_websocket_connection_close(conn, SOUP_WEBSOCKET_CLOSE_NORMAL, "Server busy");
        return G_SOURCE_REMOVE;
      }, connection);
      
      g_free(client_ip);
      return;
    }
    
    // Accept the connection
    current_webrtc_clients++;
    g_print("✅ Accepting client %s (%d/%d)\n", 
            client_ip, current_webrtc_clients, MAX_WEBRTC_CLIENTS);

    gst_print("\nProcessing new websocket connection %p\n", (gpointer)connection);
    g_signal_connect(G_OBJECT(connection), "closed",
                     G_CALLBACK(soup_websocket_closed_cb), (gpointer)receiver_entry_table);

    if (available)
    {
      available = false;
    }
    else
    {
      g_print("\nServer still not available yet! \n");
      current_webrtc_clients--;
      g_free(client_ip);
      return;
    }

    gchar *temp = g_strdup(client_ip);
    ReceiverEntry *receiver_entry = create_receiver_entry(connection, temp);

    if (receiver_entry == NULL) {
      g_printerr("Failed to create receiver entry\n");
      current_webrtc_clients--;
      g_free(client_ip);
      return;
    }

    g_hash_table_replace(receiver_entry_table, connection, receiver_entry);
    g_free(client_ip);
  }

  void destroy_receiver_entry(gpointer receiver_entry_ptr)
  {
    ReceiverEntry *receiver_entry = (ReceiverEntry *)receiver_entry_ptr;

    g_assert(receiver_entry != NULL);

    // Decrement client count
    current_webrtc_clients--;
    g_print("Client disconnected. Current clients: %d/%d\n", 
            current_webrtc_clients, MAX_WEBRTC_CLIENTS);

    if (receiver_entry->connection != NULL)
    {
      g_signal_handlers_disconnect_by_func(receiver_entry->connection,
                                           (gpointer)soup_websocket_message_cb, receiver_entry);
      g_object_unref(G_OBJECT(receiver_entry->connection));
    }

    if (receiver_entry->tee_video_src_pad != NULL)
    {
      gst_pad_add_probe(receiver_entry->tee_video_src_pad, GST_PAD_PROBE_TYPE_IDLE,
                        pad_probe_cb, receiver_entry, NULL);
    }
    
    // Clean up pending ICE candidates
    if (receiver_entry->pending_ice_candidates)
    {
      for (auto *pending : *receiver_entry->pending_ice_candidates)
      {
        g_free(pending->candidate);
        delete pending;
      }
      delete receiver_entry->pending_ice_candidates;
    }
    
    if (receiver_entry->client_ip != NULL)
      g_free(receiver_entry->client_ip);

    g_slice_free(ReceiverEntry, receiver_entry);
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
    
    // IMPORTANT: Inform user about RTX debugging
    g_print("\n" );
    g_print("═══════════════════════════════════════════════════════════════\n");
    g_print("  💡 NACK/RTX Debugging Tips:\n");
    g_print("═══════════════════════════════════════════════════════════════\n");
    g_print("  If retransmissions aren't working, run with:\n");
    g_print("  GST_DEBUG=rtprtxsend:6,rtpsession:5,webrtcbin:5 ./avpf2 ...\n");
    g_print("  \n");
    g_print("  This will show:\n");
    g_print("  - rtprtxsend:6  → RTX packet sending activity\n");
    g_print("  - rtpsession:5  → NACK requests received\n");
    g_print("  - webrtcbin:5   → WebRTC negotiation details\n");
    g_print("═══════════════════════════════════════════════════════════════\n\n");
    
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
    g_print(" Audio UDP port: %d (if audio enabled)\n", d_port + 2);
    
    g_print(" STUN server: ");
    if (stun != NULL)
    {
      g_print("%s\n", stun);
    }
    else
    {
      g_print("not provided, will run without STUN server!\n");
    }
    
    g_print(" TURN server: ");
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
        // AAC encoding pipeline (using faac encoder)
        audio_encoding = g_strdup_printf(
          "alsasrc device=%s latency-time=5000 buffer-time=10000 provide-clock=false ! "
          "audioconvert ! audioresample ! "
          "audio/x-raw,channels=2,rate=48000,format=S16LE ! "
          "volume volume=1.0 ! "
          "faac bitrate=%d midside=false rate-control=ABR shortctl=2 ! "
          "rtpmp4apay pt=97 ! "
          "queue leaky=2 max-size-buffers=1 ! "
          "tee name=at at. ! udpsink clients=%s:%d auto-multicast=false",
          audio_device, abitrate * 1000, d_ip, d_port + 2);
        g_print("✓ Audio enabled: AAC codec @ %d kbps\n", abitrate);
        
      } else if (g_strcmp0(acodec, "opus") == 0) {
        // Opus encoding pipeline
        audio_encoding = g_strdup_printf(
          "alsasrc device=%s latency-time=5000 buffer-time=10000 provide-clock=false ! "
          "audioconvert ! audioresample ! "
          "audio/x-raw,channels=2,rate=48000,format=S16LE ! "
          "volume volume=1.0 ! "
          "opusenc frame-size=60 bitrate=%d ! "
          "rtpopuspay pt=97 ! "
          "queue leaky=2 max-size-buffers=1 ! "
          "tee name=at at. ! udpsink clients=%s:%d auto-multicast=false",
          audio_device, abitrate * 1000, d_ip, d_port + 2);
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

    // Client limit enabled
    g_print("✅ Client limit: %d concurrent WebRTC viewers (UDP separate)\n", MAX_WEBRTC_CLIENTS);
    g_print("✅ Server will reject clients when limit is reached\n\n");

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
