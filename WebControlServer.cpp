#include <glib.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <string>
#include <fstream>
#include <sstream>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

#ifdef G_OS_UNIX
#include <glib-unix.h>
#endif

#define SOUP_HTTP_PORT 8082
#define STREAMING_WEBSOCKET_PORT 8082  // StreamingProgram will use same port for /ws

// g++ WebControlServer.cpp -o WebControlServer `pkg-config --cflags --libs glib-2.0 libsoup-2.4 json-glib-1.0` -std=c++17

extern "C" {

// Process management
typedef struct {
    GPid streaming_pid;
    GPid turn_pid;
    gboolean streaming_running;
    gboolean turn_running;
    
    // Current streaming parameters
    gint bitrate;
    gint fps;
    gint width;
    gint height;
    gchar *codec;
    gchar *acodec;        // Audio codec (optional: aac or opus)
    gint abitrate;        // Audio bitrate in kbps
    gchar *turn_url;
    gchar *stun_url;
    gchar *client_ip;
    gint client_port;
} ServerState;

ServerState server_state = {
    .streaming_pid = 0,
    .turn_pid = 0,
    .streaming_running = FALSE,
    .turn_running = FALSE,
    .bitrate = 1000,
    .fps = 15,
    .width = 1920,
    .height = 1080,
    .codec = g_strdup("h265"),
    .acodec = NULL,       // Optional audio codec
    .abitrate = 128,      // Default audio bitrate
    .turn_url = g_strdup("turn://ab:ab@192.168.25.90:3478"),
    .stun_url = g_strdup("stun:stun.l.google.com:19302"),
    .client_ip = g_strdup("192.168.25.90"),
    .client_port = 5004
};

// Function to check if a process is running
gboolean is_process_running(GPid pid) {
    if (pid <= 0) return FALSE;
    
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    
    if (result == 0) {
        // Process is still running
        return TRUE;
    } else if (result == pid) {
        // Process has terminated
        return FALSE;
    } else {
        // Error or process doesn't exist
        return FALSE;
    }
}

// Start TURN server
gboolean start_turn_server() {
    if (server_state.turn_running && is_process_running(server_state.turn_pid)) {
        g_print("TURN server already running (PID: %d)\n", server_state.turn_pid);
        return TRUE;
    }
    
    g_print("Starting TURN server...\n");
    
    GError *error = NULL;
    gchar *argv[] = {
        (gchar*)"./turnserver",
        (gchar*)"-c",
        (gchar*)"temp.conf",
        NULL
    };
    
    gboolean success = g_spawn_async(
        NULL,                    // working directory
        argv,                    // argv
        NULL,                    // envp
        G_SPAWN_DO_NOT_REAP_CHILD, // flags
        NULL,                    // child_setup
        NULL,                    // user_data
        &server_state.turn_pid,  // child_pid
        &error
    );
    
    if (!success) {
        g_printerr("Failed to start TURN server: %s\n", error->message);
        g_error_free(error);
        return FALSE;
    }
    
    server_state.turn_running = TRUE;
    g_print("TURN server started (PID: %d)\n", server_state.turn_pid);
    return TRUE;
}

// Stop TURN server
gboolean stop_turn_server() {
    if (!server_state.turn_running || !is_process_running(server_state.turn_pid)) {
        g_print("TURN server not running\n");
        server_state.turn_running = FALSE;
        return TRUE;
    }
    
    g_print("Stopping TURN server (PID: %d)...\n", server_state.turn_pid);
    
    if (kill(server_state.turn_pid, SIGTERM) == 0) {
        // Wait for process to terminate (with timeout)
        for (int i = 0; i < 50; i++) {
            if (!is_process_running(server_state.turn_pid)) {
                server_state.turn_running = FALSE;
                g_print("TURN server stopped\n");
                return TRUE;
            }
            g_usleep(100000); // 100ms
        }
        
        // Force kill if still running
        g_print("TURN server did not stop gracefully, forcing...\n");
        kill(server_state.turn_pid, SIGKILL);
        g_usleep(500000); // 500ms
    }
    
    server_state.turn_running = FALSE;
    g_print("TURN server stopped\n");
    return TRUE;
}

// Start streaming program
gboolean start_streaming() {
    if (server_state.streaming_running && is_process_running(server_state.streaming_pid)) {
        g_print("Streaming program already running (PID: %d)\n", server_state.streaming_pid);
        return TRUE;
    }
    
    g_print("Starting streaming program...\n");
    g_print("  Bitrate: %d kbps\n", server_state.bitrate);
    g_print("  FPS: %d\n", server_state.fps);
    g_print("  Resolution: %dx%d\n", server_state.width, server_state.height);
    g_print("  Codec: %s\n", server_state.codec);
    g_print("  TURN: %s\n", server_state.turn_url);
    g_print("  STUN: %s\n", server_state.stun_url);
    
    GError *error = NULL;
    
    // Build command line arguments
    gchar *bitrate_arg = g_strdup_printf("--bitrate=%d", server_state.bitrate);
    gchar *fps_arg = g_strdup_printf("--fps=%d", server_state.fps);
    gchar *width_arg = g_strdup_printf("--width=%d", server_state.width);
    gchar *height_arg = g_strdup_printf("--height=%d", server_state.height);
    gchar *codec_arg = g_strdup_printf("--codec=%s", server_state.codec);
    gchar *turn_arg = g_strdup_printf("--turn=%s", server_state.turn_url);
    gchar *stun_arg = g_strdup_printf("--stun=%s", server_state.stun_url);
    gchar *ip_arg = g_strdup_printf("--client-ip=%s", server_state.client_ip);
    gchar *port_arg = g_strdup_printf("--client-port=%d", server_state.client_port);
    gchar *audio_device_arg = g_strdup_printf("--audio-device=hw:1,1");
    
    // Build argument array
    GPtrArray *argv_array = g_ptr_array_new();
    g_ptr_array_add(argv_array, (gchar*)"./StreamingProgram");
    g_ptr_array_add(argv_array, bitrate_arg);
    g_ptr_array_add(argv_array, fps_arg);
    g_ptr_array_add(argv_array, width_arg);
    g_ptr_array_add(argv_array, height_arg);
    g_ptr_array_add(argv_array, codec_arg);
    g_ptr_array_add(argv_array, turn_arg);
    g_ptr_array_add(argv_array, stun_arg);
    g_ptr_array_add(argv_array, ip_arg);
    g_ptr_array_add(argv_array, port_arg);
    g_ptr_array_add(argv_array, audio_device_arg);
    
    // Add audio codec and bitrate if specified
    gchar *acodec_arg = NULL;
    gchar *abitrate_arg = NULL;
    if (server_state.acodec != NULL && strlen(server_state.acodec) > 0) {
        acodec_arg = g_strdup_printf("--acodec=%s", server_state.acodec);
        abitrate_arg = g_strdup_printf("--abitrate=%d", server_state.abitrate);
        g_ptr_array_add(argv_array, acodec_arg);
        g_ptr_array_add(argv_array, abitrate_arg);
        g_print("  Audio Codec: %s\n", server_state.acodec);
        g_print("  Audio Bitrate: %d kbps\n", server_state.abitrate);
    } else {
        g_print("  Audio: Disabled\n");
    }
    
    g_ptr_array_add(argv_array, NULL);
    gchar **argv = (gchar**)argv_array->pdata;
    
    gboolean success = g_spawn_async(
        NULL,                           // working directory
        argv,                           // argv
        NULL,                           // envp
        G_SPAWN_DO_NOT_REAP_CHILD,     // flags
        NULL,                           // child_setup
        NULL,                           // user_data
        &server_state.streaming_pid,    // child_pid
        &error
    );
    
    // Free argument strings
    g_free(bitrate_arg);
    g_free(fps_arg);
    g_free(width_arg);
    g_free(height_arg);
    g_free(codec_arg);
    g_free(turn_arg);
    g_free(stun_arg);
    g_free(ip_arg);
    g_free(port_arg);
    g_free(audio_device_arg);
    if (acodec_arg) g_free(acodec_arg);
    if (abitrate_arg) g_free(abitrate_arg);
    g_ptr_array_free(argv_array, FALSE);
    
    if (!success) {
        g_printerr("Failed to start streaming program: %s\n", error->message);
        g_error_free(error);
        return FALSE;
    }
    
    server_state.streaming_running = TRUE;
    g_print("Streaming program started (PID: %d)\n", server_state.streaming_pid);
    return TRUE;
}

// Stop streaming program
gboolean stop_streaming() {
    if (!server_state.streaming_running || !is_process_running(server_state.streaming_pid)) {
        g_print("Streaming program not running\n");
        server_state.streaming_running = FALSE;
        return TRUE;
    }
    
    g_print("Stopping streaming program (PID: %d)...\n", server_state.streaming_pid);
    
    if (kill(server_state.streaming_pid, SIGINT) == 0) {
        // Wait for process to terminate (with timeout)
        for (int i = 0; i < 50; i++) {
            if (!is_process_running(server_state.streaming_pid)) {
                server_state.streaming_running = FALSE;
                g_print("Streaming program stopped\n");
                return TRUE;
            }
            g_usleep(100000); // 100ms
        }
        
        // Force kill if still running
        g_print("Streaming program did not stop gracefully, forcing...\n");
        kill(server_state.streaming_pid, SIGKILL);
        g_usleep(500000); // 500ms
    }
    
    server_state.streaming_running = FALSE;
    g_print("Streaming program stopped\n");
    return TRUE;
}

// HTTP handler for serving the control panel HTML
void soup_http_handler(G_GNUC_UNUSED SoupServer *soup_server,
                       SoupMessage *message, const char *path,
                       G_GNUC_UNUSED GHashTable *query,
                       G_GNUC_UNUSED SoupClientContext *client_context,
                       G_GNUC_UNUSED gpointer user_data)
{
    if ((g_strcmp0(path, "/") != 0) && (g_strcmp0(path, "/index.html") != 0)) {
        soup_message_set_status(message, SOUP_STATUS_NOT_FOUND);
        return;
    }
    
    std::ifstream html_file("index.html");
    if (!html_file) {
        g_printerr("Failed to open index.html\n");
        soup_message_set_status(message, SOUP_STATUS_INTERNAL_SERVER_ERROR);
        return;
    }
    
    std::stringstream buffer;
    buffer << html_file.rdbuf();
    std::string html_content = buffer.str();
    
    SoupBuffer *soup_buffer = soup_buffer_new(SOUP_MEMORY_COPY, 
                                               html_content.c_str(), 
                                               html_content.size());
    
    soup_message_headers_set_content_type(message->response_headers, "text/html", NULL);
    soup_message_body_append_buffer(message->response_body, soup_buffer);
    soup_buffer_free(soup_buffer);
    
    soup_message_set_status(message, SOUP_STATUS_OK);
}

// REST API: Get status
void api_status_handler(G_GNUC_UNUSED SoupServer *soup_server,
                       SoupMessage *message, const char *path,
                       G_GNUC_UNUSED GHashTable *query,
                       G_GNUC_UNUSED SoupClientContext *client_context,
                       G_GNUC_UNUSED gpointer user_data)
{
    // Update running status by checking processes
    if (server_state.streaming_running) {
        server_state.streaming_running = is_process_running(server_state.streaming_pid);
    }
    if (server_state.turn_running) {
        server_state.turn_running = is_process_running(server_state.turn_pid);
    }
    
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "streaming_running");
    json_builder_add_boolean_value(builder, server_state.streaming_running);
    
    json_builder_set_member_name(builder, "turn_running");
    json_builder_add_boolean_value(builder, server_state.turn_running);
    
    json_builder_set_member_name(builder, "parameters");
    json_builder_begin_object(builder);
    
    json_builder_set_member_name(builder, "bitrate");
    json_builder_add_int_value(builder, server_state.bitrate);
    
    json_builder_set_member_name(builder, "fps");
    json_builder_add_int_value(builder, server_state.fps);
    
    json_builder_set_member_name(builder, "width");
    json_builder_add_int_value(builder, server_state.width);
    
    json_builder_set_member_name(builder, "height");
    json_builder_add_int_value(builder, server_state.height);
    
    json_builder_set_member_name(builder, "codec");
    json_builder_add_string_value(builder, server_state.codec);
    
    json_builder_set_member_name(builder, "acodec");
    json_builder_add_string_value(builder, server_state.acodec ? server_state.acodec : "");
    
    json_builder_set_member_name(builder, "abitrate");
    json_builder_add_int_value(builder, server_state.abitrate);
    
    json_builder_set_member_name(builder, "turn_url");
    json_builder_add_string_value(builder, server_state.turn_url);
    
    json_builder_set_member_name(builder, "stun_url");
    json_builder_add_string_value(builder, server_state.stun_url);
    
    json_builder_set_member_name(builder, "client_ip");
    json_builder_add_string_value(builder, server_state.client_ip);
    
    json_builder_set_member_name(builder, "client_port");
    json_builder_add_int_value(builder, server_state.client_port);
    
    json_builder_end_object(builder);
    json_builder_end_object(builder);
    
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gchar *json_str = json_generator_to_data(generator, NULL);
    
    SoupBuffer *soup_buffer = soup_buffer_new(SOUP_MEMORY_COPY, json_str, strlen(json_str));
    soup_message_headers_set_content_type(message->response_headers, "application/json", NULL);
    soup_message_body_append_buffer(message->response_body, soup_buffer);
    soup_buffer_free(soup_buffer);
    
    g_free(json_str);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    
    soup_message_set_status(message, SOUP_STATUS_OK);
}

// REST API: Start streaming
void api_start_handler(G_GNUC_UNUSED SoupServer *soup_server,
                      SoupMessage *message, const char *path,
                      G_GNUC_UNUSED GHashTable *query,
                      G_GNUC_UNUSED SoupClientContext *client_context,
                      G_GNUC_UNUSED gpointer user_data)
{
    if (message->method != SOUP_METHOD_POST) {
        soup_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED);
        return;
    }
    
    // Parse JSON body if present
    if (message->request_body->length > 0) {
        JsonParser *parser = json_parser_new();
        GError *error = NULL;
        
        if (json_parser_load_from_data(parser, message->request_body->data, 
                                       message->request_body->length, &error)) {
            JsonNode *root = json_parser_get_root(parser);
            JsonObject *obj = json_node_get_object(root);
            
            if (json_object_has_member(obj, "bitrate"))
                server_state.bitrate = json_object_get_int_member(obj, "bitrate");
            if (json_object_has_member(obj, "fps"))
                server_state.fps = json_object_get_int_member(obj, "fps");
            if (json_object_has_member(obj, "width"))
                server_state.width = json_object_get_int_member(obj, "width");
            if (json_object_has_member(obj, "height"))
                server_state.height = json_object_get_int_member(obj, "height");
            if (json_object_has_member(obj, "codec")) {
                g_free(server_state.codec);
                server_state.codec = g_strdup(json_object_get_string_member(obj, "codec"));
            }
            if (json_object_has_member(obj, "acodec")) {
                g_free(server_state.acodec);
                const gchar *acodec_value = json_object_get_string_member(obj, "acodec");
                // Only set if not empty
                if (acodec_value && strlen(acodec_value) > 0) {
                    server_state.acodec = g_strdup(acodec_value);
                } else {
                    server_state.acodec = NULL;
                }
            }
            if (json_object_has_member(obj, "abitrate"))
                server_state.abitrate = json_object_get_int_member(obj, "abitrate");
            if (json_object_has_member(obj, "turn_url")) {
                g_free(server_state.turn_url);
                server_state.turn_url = g_strdup(json_object_get_string_member(obj, "turn_url"));
            }
            if (json_object_has_member(obj, "stun_url")) {
                g_free(server_state.stun_url);
                server_state.stun_url = g_strdup(json_object_get_string_member(obj, "stun_url"));
            }
            if (json_object_has_member(obj, "client_ip")) {
                g_free(server_state.client_ip);
                server_state.client_ip = g_strdup(json_object_get_string_member(obj, "client_ip"));
            }
            if (json_object_has_member(obj, "client_port"))
                server_state.client_port = json_object_get_int_member(obj, "client_port");
        }
        
        g_object_unref(parser);
    }
    
    gboolean success = start_streaming();
    
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "success");
    json_builder_add_boolean_value(builder, success);
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, success ? "Streaming started" : "Failed to start streaming");
    json_builder_end_object(builder);
    
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gchar *json_str = json_generator_to_data(generator, NULL);
    
    SoupBuffer *soup_buffer = soup_buffer_new(SOUP_MEMORY_COPY, json_str, strlen(json_str));
    soup_message_headers_set_content_type(message->response_headers, "application/json", NULL);
    soup_message_body_append_buffer(message->response_body, soup_buffer);
    soup_buffer_free(soup_buffer);
    
    g_free(json_str);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    
    soup_message_set_status(message, SOUP_STATUS_OK);
}

// REST API: Stop streaming
void api_stop_handler(G_GNUC_UNUSED SoupServer *soup_server,
                     SoupMessage *message, const char *path,
                     G_GNUC_UNUSED GHashTable *query,
                     G_GNUC_UNUSED SoupClientContext *client_context,
                     G_GNUC_UNUSED gpointer user_data)
{
    if (message->method != SOUP_METHOD_POST) {
        soup_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED);
        return;
    }
    
    gboolean success = stop_streaming();
    
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "success");
    json_builder_add_boolean_value(builder, success);
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, success ? "Streaming stopped" : "Failed to stop streaming");
    json_builder_end_object(builder);
    
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gchar *json_str = json_generator_to_data(generator, NULL);
    
    SoupBuffer *soup_buffer = soup_buffer_new(SOUP_MEMORY_COPY, json_str, strlen(json_str));
    soup_message_headers_set_content_type(message->response_headers, "application/json", NULL);
    soup_message_body_append_buffer(message->response_body, soup_buffer);
    soup_buffer_free(soup_buffer);
    
    g_free(json_str);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    
    soup_message_set_status(message, SOUP_STATUS_OK);
}

// REST API: Start TURN server
void api_turn_start_handler(G_GNUC_UNUSED SoupServer *soup_server,
                           SoupMessage *message, const char *path,
                           G_GNUC_UNUSED GHashTable *query,
                           G_GNUC_UNUSED SoupClientContext *client_context,
                           G_GNUC_UNUSED gpointer user_data)
{
    if (message->method != SOUP_METHOD_POST) {
        soup_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED);
        return;
    }
    
    gboolean success = start_turn_server();
    
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "success");
    json_builder_add_boolean_value(builder, success);
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, success ? "TURN server started" : "Failed to start TURN server");
    json_builder_end_object(builder);
    
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gchar *json_str = json_generator_to_data(generator, NULL);
    
    SoupBuffer *soup_buffer = soup_buffer_new(SOUP_MEMORY_COPY, json_str, strlen(json_str));
    soup_message_headers_set_content_type(message->response_headers, "application/json", NULL);
    soup_message_body_append_buffer(message->response_body, soup_buffer);
    soup_buffer_free(soup_buffer);
    
    g_free(json_str);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    
    soup_message_set_status(message, SOUP_STATUS_OK);
}

// Helper function to trim whitespace
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// REST API: Configuration handler (GET to load, POST to save)
void api_config_handler(G_GNUC_UNUSED SoupServer *soup_server,
                       SoupMessage *message, const char *path,
                       G_GNUC_UNUSED GHashTable *query,
                       G_GNUC_UNUSED SoupClientContext *client_context,
                       G_GNUC_UNUSED gpointer user_data)
{
    g_print("→ Received %s request to /api/config\n", message->method);
    
    if (g_strcmp0(message->method, "GET") == 0) {
        // ========== GET: Load configuration from file ==========
        
        g_print("  Processing GET request...\n");
        
        // Read config file
        std::ifstream config_file("webrtc_parameters.conf");
        JsonBuilder *builder = json_builder_new();
        json_builder_begin_object(builder);
        
        if (config_file.is_open()) {
            std::string line;
            while (std::getline(config_file, line)) {
                // Skip comments and empty lines
                line = trim(line);
                if (line.empty() || line[0] == '#') continue;
                
                // Parse key=value
                size_t eq_pos = line.find('=');
                if (eq_pos != std::string::npos) {
                    std::string key = trim(line.substr(0, eq_pos));
                    std::string value = trim(line.substr(eq_pos + 1));
                    
                    json_builder_set_member_name(builder, key.c_str());
                    
                    // Try to parse as integer
                    char* endptr;
                    long int_val = strtol(value.c_str(), &endptr, 10);
                    if (*endptr == '\0' && endptr != value.c_str()) {
                        json_builder_add_int_value(builder, int_val);
                    } else {
                        json_builder_add_string_value(builder, value.c_str());
                    }
                }
            }
            config_file.close();
            g_print("✓ Configuration loaded from webrtc_parameters.conf\n");
        } else {
            // Return default values if file doesn't exist
            g_print("⚠ Config file not found, returning defaults\n");
            json_builder_set_member_name(builder, "codec");
            json_builder_add_string_value(builder, "h264");
            json_builder_set_member_name(builder, "bitrate");
            json_builder_add_int_value(builder, 6000);
            json_builder_set_member_name(builder, "width");
            json_builder_add_int_value(builder, 1920);
            json_builder_set_member_name(builder, "height");
            json_builder_add_int_value(builder, 1080);
            json_builder_set_member_name(builder, "fps");
            json_builder_add_int_value(builder, 60);
            json_builder_set_member_name(builder, "acodec");
            json_builder_add_string_value(builder, "opus");
            json_builder_set_member_name(builder, "abitrate");
            json_builder_add_int_value(builder, 128);
            json_builder_set_member_name(builder, "audio_device");
            json_builder_add_string_value(builder, "hw:1,1");
            json_builder_set_member_name(builder, "client_ip");
            json_builder_add_string_value(builder, "192.168.25.90");
            json_builder_set_member_name(builder, "client_port");
            json_builder_add_int_value(builder, 5004);
            json_builder_set_member_name(builder, "audio_port");
            json_builder_add_int_value(builder, 5006);
            json_builder_set_member_name(builder, "turn_url");
            json_builder_add_string_value(builder, "turn://ab:ab@192.168.25.90:3478");
            json_builder_set_member_name(builder, "stun_url");
            json_builder_add_string_value(builder, "stun:stun.l.google.com:19302");
        }
        
        json_builder_end_object(builder);
        
        JsonGenerator *generator = json_generator_new();
        JsonNode *root = json_builder_get_root(builder);
        json_generator_set_root(generator, root);
        gchar *json_str = json_generator_to_data(generator, NULL);
        
        SoupBuffer *soup_buffer = soup_buffer_new(SOUP_MEMORY_COPY, json_str, strlen(json_str));
        soup_message_headers_set_content_type(message->response_headers, "application/json", NULL);
        soup_message_body_append_buffer(message->response_body, soup_buffer);
        soup_buffer_free(soup_buffer);
        
        g_free(json_str);
        json_node_free(root);
        g_object_unref(generator);
        g_object_unref(builder);
        
        soup_message_set_status(message, SOUP_STATUS_OK);
        
    } else if (g_strcmp0(message->method, "POST") == 0) {
        // ========== POST: Save configuration to file ==========
        
        g_print("  Processing POST request...\n");
        
        // Parse JSON body
        JsonParser *parser = json_parser_new();
        GError *error = NULL;
        
        if (!json_parser_load_from_data(parser, message->request_body->data, 
                                         message->request_body->length, &error)) {
            g_printerr("Failed to parse JSON: %s\n", error->message);
            g_error_free(error);
            g_object_unref(parser);
            soup_message_set_status(message, SOUP_STATUS_BAD_REQUEST);
            return;
        }
        
        JsonObject *root = json_node_get_object(json_parser_get_root(parser));
        
        // Write to config file
        std::ofstream config_file("webrtc_parameters.conf");
        
        if (config_file.is_open()) {
            config_file << "# WebRTC Streaming Parameters Configuration\n";
            config_file << "# This file is automatically updated by the web control panel\n\n";
            
            config_file << "# Video Settings\n";
            if (json_object_has_member(root, "codec")) {
                config_file << "codec=" << json_object_get_string_member(root, "codec") << "\n";
            }
            if (json_object_has_member(root, "bitrate")) {
                config_file << "bitrate=" << json_object_get_int_member(root, "bitrate") << "\n";
            }
            if (json_object_has_member(root, "width")) {
                config_file << "width=" << json_object_get_int_member(root, "width") << "\n";
            }
            if (json_object_has_member(root, "height")) {
                config_file << "height=" << json_object_get_int_member(root, "height") << "\n";
            }
            if (json_object_has_member(root, "fps")) {
                config_file << "fps=" << json_object_get_int_member(root, "fps") << "\n";
            }
            
            config_file << "\n# Audio Settings\n";
            if (json_object_has_member(root, "acodec")) {
                config_file << "acodec=" << json_object_get_string_member(root, "acodec") << "\n";
            }
            if (json_object_has_member(root, "abitrate")) {
                config_file << "abitrate=" << json_object_get_int_member(root, "abitrate") << "\n";
            }
            if (json_object_has_member(root, "audio_device")) {
                config_file << "audio_device=" << json_object_get_string_member(root, "audio_device") << "\n";
            }
            
            config_file << "\n# Network Settings\n";
            if (json_object_has_member(root, "client_ip")) {
                config_file << "client_ip=" << json_object_get_string_member(root, "client_ip") << "\n";
            }
            if (json_object_has_member(root, "client_port")) {
                config_file << "client_port=" << json_object_get_int_member(root, "client_port") << "\n";
            }
            if (json_object_has_member(root, "audio_port")) {
                config_file << "audio_port=" << json_object_get_int_member(root, "audio_port") << "\n";
            }
            if (json_object_has_member(root, "turn_url")) {
                config_file << "turn_url=" << json_object_get_string_member(root, "turn_url") << "\n";
            }
            if (json_object_has_member(root, "stun_url")) {
                config_file << "stun_url=" << json_object_get_string_member(root, "stun_url") << "\n";
            }
            
            config_file.close();
            g_print("✓ Configuration saved to webrtc_parameters.conf\n");
        } else {
            g_printerr("✗ Failed to open config file for writing\n");
        }
        
        g_object_unref(parser);
        
        // Return success response
        JsonBuilder *builder = json_builder_new();
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "success");
        json_builder_add_boolean_value(builder, TRUE);
        json_builder_set_member_name(builder, "message");
        json_builder_add_string_value(builder, "Configuration saved");
        json_builder_end_object(builder);
        
        JsonGenerator *generator = json_generator_new();
        JsonNode *response_root = json_builder_get_root(builder);
        json_generator_set_root(generator, response_root);
        gchar *json_str = json_generator_to_data(generator, NULL);
        
        SoupBuffer *soup_buffer = soup_buffer_new(SOUP_MEMORY_COPY, json_str, strlen(json_str));
        soup_message_headers_set_content_type(message->response_headers, "application/json", NULL);
        soup_message_body_append_buffer(message->response_body, soup_buffer);
        soup_buffer_free(soup_buffer);
        
        g_free(json_str);
        json_node_free(response_root);
        g_object_unref(generator);
        g_object_unref(builder);
        
        soup_message_set_status(message, SOUP_STATUS_OK);
        
    } else {
        // Method not allowed
        g_print("✗ Method %s not allowed for /api/config\n", message->method);
        soup_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED);
    }
}

// REST API: Stop TURN server
void api_turn_stop_handler(G_GNUC_UNUSED SoupServer *soup_server,
                          SoupMessage *message, const char *path,
                          G_GNUC_UNUSED GHashTable *query,
                          G_GNUC_UNUSED SoupClientContext *client_context,
                          G_GNUC_UNUSED gpointer user_data)
{
    if (message->method != SOUP_METHOD_POST) {
        soup_message_set_status(message, SOUP_STATUS_METHOD_NOT_ALLOWED);
        return;
    }
    
    gboolean success = stop_turn_server();
    
    JsonBuilder *builder = json_builder_new();
    json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "success");
    json_builder_add_boolean_value(builder, success);
    json_builder_set_member_name(builder, "message");
    json_builder_add_string_value(builder, success ? "TURN server stopped" : "Failed to stop TURN server");
    json_builder_end_object(builder);
    
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    gchar *json_str = json_generator_to_data(generator, NULL);
    
    SoupBuffer *soup_buffer = soup_buffer_new(SOUP_MEMORY_COPY, json_str, strlen(json_str));
    soup_message_headers_set_content_type(message->response_headers, "application/json", NULL);
    soup_message_body_append_buffer(message->response_body, soup_buffer);
    soup_buffer_free(soup_buffer);
    
    g_free(json_str);
    json_node_free(root);
    g_object_unref(generator);
    g_object_unref(builder);
    
    soup_message_set_status(message, SOUP_STATUS_OK);
}

#ifdef G_OS_UNIX
gboolean exit_sighandler(gpointer user_data) {
    g_print("Caught signal, stopping all processes...\n");
    
    stop_streaming();
    stop_turn_server();
    
    GMainLoop *mainloop = (GMainLoop *)user_data;
    g_main_loop_quit(mainloop);
    return TRUE;
}
#endif

int main(int argc, char *argv[]) {
    GMainLoop *mainloop;
    SoupServer *soup_server;
    
    g_print("╔════════════════════════════════════════════════╗\n");
    g_print("║   WebRTC Streaming Control Server             ║\n");
    g_print("╚════════════════════════════════════════════════╝\n\n");
    
    mainloop = g_main_loop_new(NULL, FALSE);
    g_assert(mainloop != NULL);
    
#ifdef G_OS_UNIX
    g_unix_signal_add(SIGINT, exit_sighandler, mainloop);
    g_unix_signal_add(SIGTERM, exit_sighandler, mainloop);
#endif
    
    // Create soup server
    soup_server = soup_server_new(SOUP_SERVER_SERVER_HEADER, "webrtc-control-server", NULL);
    
    // Add HTTP handlers
    soup_server_add_handler(soup_server, "/", soup_http_handler, NULL, NULL);
    soup_server_add_handler(soup_server, "/api/status", api_status_handler, NULL, NULL);
    soup_server_add_handler(soup_server, "/api/config", api_config_handler, NULL, NULL);
    soup_server_add_handler(soup_server, "/api/start", api_start_handler, NULL, NULL);
    soup_server_add_handler(soup_server, "/api/stop", api_stop_handler, NULL, NULL);
    soup_server_add_handler(soup_server, "/api/turn/start", api_turn_start_handler, NULL, NULL);
    soup_server_add_handler(soup_server, "/api/turn/stop", api_turn_stop_handler, NULL, NULL);
    
    // Start listening
    GError *error = NULL;
    if (!soup_server_listen_all(soup_server, SOUP_HTTP_PORT, (SoupServerListenOptions)0, &error)) {
        g_printerr("Failed to start server: %s\n", error->message);
        g_error_free(error);
        return -1;
    }
    
    g_print("✓ Control server started\n");
    g_print("✓ Access control panel: http://127.0.0.1:%d/\n\n", SOUP_HTTP_PORT);
    g_print("API Endpoints:\n");
    g_print("  GET  /api/status        - Get current status\n");
    g_print("  GET  /api/config        - Load configuration\n");
    g_print("  POST /api/config        - Save configuration\n");
    g_print("  POST /api/start         - Start streaming\n");
    g_print("  POST /api/stop          - Stop streaming\n");
    g_print("  POST /api/turn/start    - Start TURN server\n");
    g_print("  POST /api/turn/stop     - Stop TURN server\n\n");
    g_print("Press Ctrl+C to stop\n");
    g_print("════════════════════════════════════════════════\n\n");
    
    // Run main loop
    g_main_loop_run(mainloop);
    
    // Cleanup
    g_print("\nShutting down...\n");
    g_object_unref(G_OBJECT(soup_server));
    g_main_loop_unref(mainloop);
    
    g_free(server_state.codec);
    g_free(server_state.turn_url);
    g_free(server_state.stun_url);
    g_free(server_state.client_ip);
    
    g_print("Goodbye!\n");
    return 0;
}

} // extern "C"
