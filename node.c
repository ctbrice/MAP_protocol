#define _DEFAULT_SOURCE

#include "map.h"

#include <netdb.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

enum {
    APPLICATION_MESSAGE = 1,
    MARKER_MESSAGE = 2,
    REPORT_MESSAGE = 3,
    HALT_MESSAGE = 4
};

typedef struct {
    uint32_t type;
    uint32_t snapshot_id;
    uint32_t sender;
    uint32_t value;
    uint32_t auxiliary;
} WireMessage;

static void debug_log(const Node *node, const char *format, ...)
{
    if (getenv("MAP_DEBUG") == NULL) {
        return;
    }

    fprintf(stderr, "[node %d] ", node->id);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputc('\n', stderr);
    fflush(stderr);
}

static int send_all(int socket_fd, const void *buffer, size_t length)
{
    const char *ptr = buffer;
    while (length > 0) {
        ssize_t sent = send(socket_fd, ptr, length, 0);
        if (sent <= 0) return -1;
        ptr += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static int receive_all(int socket_fd, void *buffer, size_t length)
{
    char *ptr = buffer;
    while (length > 0) {
        ssize_t received = recv(socket_fd, ptr, length, 0);
        if (received <= 0) return -1;
        ptr += received;
        length -= (size_t)received;
    }
    return 0;
}

static int connect_to_node(const NodeInfo *info)
{
    /* Connect to the specified node using TCP */
    struct addrinfo hints = {0};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    /* Resolve the address of the specified node */
    struct addrinfo *res = NULL;
    char port_str[6];
    snprintf(port_str, sizeof(port_str), "%d", info->port);
    if (getaddrinfo(info->host, port_str, &hints, &res) != 0) {
        return -1;
    }

    /* Attempt to connect to one of the resolved addresses */
    int sock_fd = -1;
    for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
        sock_fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sock_fd < 0) continue;
        if (sock_fd >= 0 && connect(sock_fd, p->ai_addr, p->ai_addrlen) == 0) break;
        close(sock_fd); // Close the socket if the connection attempt failed
        sock_fd = -1;
    }

    freeaddrinfo(res);
    return sock_fd;
}

static int create_listener(int port)
{
    /* Create a TCP listener socket for incoming connections */
    int socket_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0)
    {
        perror("socket");
        return -1;
    }

    /* Allow the socket to be quickly reused after it is closed */
    int reuse = 1;
    if(setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        perror("setsockopt");
        close(socket_fd);
        return -1;
    }

    /* Bind the socket to the specified port and start listening for incoming connections */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port); 
    if (bind(socket_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(socket_fd, SOMAXCONN) < 0) {
        perror("bind or listen");
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static int send_handshake(int socket_fd, int node_id)
{
    /* Send a handshake message to the specified node */
    uint32_t handshake = htonl((uint32_t)node_id);
    return send_all(socket_fd, &handshake, sizeof(handshake));
}

static int receive_handshake(int socket_fd)
{
    uint32_t handshake;
    if (receive_all(socket_fd, &handshake, sizeof(handshake)) != 0) {
        perror("recv_handshake");
        return -1;
    }
    return (int)ntohl(handshake);
}

static int channel_index(const Node *node, int neighbor_id)
{
    for (int i = 0; i < node->channel_count; i++) {
        if (node->channels[i].neighbor_id == neighbor_id) {
            return i;
        }
    }
    return -1; // Channel not found
}

static int build_snapshot_tree(Node *node)
{
    int node_count = node->config->node_count;
    int *queue = malloc((size_t)node_count * sizeof(*queue));
    node->snapshot_tree_parent = malloc((size_t)node_count * sizeof(*node->snapshot_tree_parent));
    if (queue == NULL || node->snapshot_tree_parent == NULL) {
        perror("malloc snapshot tree");
        free(queue);
        return -1;
    }

    for (int i = 0; i < node_count; i++) {
        node->snapshot_tree_parent[i] = -2;
    }
    node->snapshot_tree_parent[0] = -1;
    int head = 0;
    int tail = 0;
    queue[tail++] = 0;

    while (head < tail) {
        int current = queue[head++];
        for (int i = 0; i < node->config->neighbor_counts[current]; i++) {
            int neighbor = node->config->neighbors[current][i];
            if (node->snapshot_tree_parent[neighbor] == -2) {
                node->snapshot_tree_parent[neighbor] = current;
                queue[tail++] = neighbor;
            }
        }
    }
    free(queue);

    for (int i = 0; i < node_count; i++) {
        if (node->snapshot_tree_parent[i] == -2) {
            fprintf(stderr, "Node %d: topology is disconnected; node %d is unreachable from node 0.\n",
                    node->id, i);
            return -1;
        }
    }
    return 0;
}

static int all_markers_seen(const Node *node)
{
    for (int i = 0; i < node->channel_count; i++) {
        if (node->snapshot_channel_markers[i] == 0) {
            return 0;
        }
    }
    return 1;
}

static int send_wire_message(Node *node, int neighbor_id, WireMessage message)
{
    int index = channel_index(node, neighbor_id);
    if (index < 0) {
        fprintf(stderr, "Node %d has no channel to node %d.\n", node->id, neighbor_id);
        return -1;
    }

    pthread_mutex_lock(&node->channels[index].send_mutex);
    int result = -1;
    if (node->channels[index].socket_fd >= 0 &&
        send_all(node->channels[index].socket_fd, &message, sizeof(message)) == 0) {
        result = 0;
    } else {
        perror("send_all");
    }
    pthread_mutex_unlock(&node->channels[index].send_mutex);
    return result;
}
 
static void send_marker(Node *node, int neighbor_id, uint32_t snapshot_id)
{
    WireMessage message;
    memset(&message, 0, sizeof(message));
    message.type = htonl(MARKER_MESSAGE);
    message.snapshot_id = htonl(snapshot_id);
    message.sender = htonl((uint32_t)node->id);
    message.value = 0; // Not used for marker messages

    if (send_wire_message(node, neighbor_id, message) == 0) {
        debug_log(node, "sent marker for snapshot %u to node %d", snapshot_id, neighbor_id);
    }
}

static void send_report(Node *node, int neighbor_id, uint32_t snapshot_id,
                        uint32_t active_count, uint32_t message_count)
{
    WireMessage message;
    memset(&message, 0, sizeof(message));
    message.type = htonl(REPORT_MESSAGE);
    message.snapshot_id = htonl(snapshot_id);
    message.sender = htonl((uint32_t)node->id);
    message.value = htonl(active_count);
    message.auxiliary = htonl(message_count);

    if (send_wire_message(node, neighbor_id, message) == 0) {
        debug_log(node, "sent snapshot %u report to node %d: active=%u in_flight=%u",
                  snapshot_id, neighbor_id, active_count, message_count);
    }
}

static void send_halt(Node *node, int neighbor_id)
{
    WireMessage message;
    memset(&message, 0, sizeof(message));
    message.type = htonl(HALT_MESSAGE);
    message.snapshot_id = htonl((uint32_t)node->snapshot_id);
    message.sender = htonl((uint32_t)node->id);
    message.value = 0; // Not used for halt messages

    if (send_wire_message(node, neighbor_id, message) == 0) {
        debug_log(node, "sent halt message to node %d", neighbor_id);
    }
}

static int mark_halted(Node *node)
{
    pthread_mutex_lock(&node->state_mutex);
    if (node->halt_requested) {
        pthread_mutex_unlock(&node->state_mutex);
        return 0;
    }
    node->halt_requested = 1;
    node->halted = 1;
    pthread_cond_broadcast(&node->state_changed);
    pthread_mutex_unlock(&node->state_mutex);

    return 1;
}

static void shutdown_channels(Node *node)
{
    for (int i = 0; i < node->channel_count; i++) {
        pthread_mutex_lock(&node->channels[i].send_mutex);
        if (node->channels[i].socket_fd >= 0) {
            shutdown(node->channels[i].socket_fd, SHUT_RDWR);
        }
        pthread_mutex_unlock(&node->channels[i].send_mutex);
    }
}

static void request_halt(Node *node)
{
    if (!mark_halted(node)) {
        return;
    }
    debug_log(node, "halting after termination was detected");
    for (int i = 0; i < node->channel_count; i++) {
        send_halt(node, node->channels[i].neighbor_id);
    }
    shutdown_channels(node);
}

static void forward_halt(Node *node, int sender_id)
{
    if (!mark_halted(node)) {
        return;
    }
    debug_log(node, "received halt message from node %d", sender_id);
    for (int i = 0; i < node->channel_count; i++) {
        int neighbor_id = node->channels[i].neighbor_id;
        if (neighbor_id != sender_id) {
            send_halt(node, neighbor_id);
        }
    }
    shutdown_channels(node);
}

static void write_vector_clock_file(Node *node)
{
    FILE *file = fopen(node->output_path, "a");
    if (file == NULL) {
        perror("fopen");
        return;
    }
    for (int i = 0; i < node->config->node_count; i++) {
        fprintf(file, "%d ", node->vector_clock[i]);
    }
    fprintf(file, "\n");
    fclose(file);
}

static void record_snapshot_vector(Node *node)
{
    if (node->vector_clock == NULL) {
        return;
    }
    write_vector_clock_file(node);
}

static void begin_snapshot(Node *node, uint32_t snapshot_id, int parent_id)
{
    if(node->snapshot_active && node->snapshot_id == (int)snapshot_id) {
        return;
    }

    node->snapshot_active = 1;
    node->snapshot_id = (int)snapshot_id;
    node->snapshot_parent = parent_id;
    node->snapshot_local_recorded = 1;
    node->snapshot_report_sent = 0;
    node->snapshot_complete = 0;
    node->snapshot_recorded_active = node->active;
    node->snapshot_report_children = 0;
    node->snapshot_reports_received = 0;
    node->snapshot_subtree_active = node->snapshot_recorded_active ? 1U : 0U;
    node->snapshot_subtree_messages = 0;
    memset(node->snapshot_child_reports_received, 0,
           (size_t)node->config->node_count * sizeof(*node->snapshot_child_reports_received));
    for (int i = 0; i < node->config->node_count; i++) {
        if (node->snapshot_tree_parent[i] == node->id) {
            node->snapshot_report_children++;
        }
    }

    /* Initialize snapshot channel markers and messages */
    for (int i = 0; i < node->channel_count; i++) {
        node->snapshot_channel_markers[i] = 0;
        node->snapshot_channels_messages[i] = 0;
    }

    /* If this node has a parent in the snapshot, mark the corresponding channel as having received a marker */
    if(parent_id >= 0) {
        int parent_index = channel_index(node, parent_id);
        if (parent_index >= 0) {
            node->snapshot_channel_markers[parent_index] = 1;
        }
    }

    record_snapshot_vector(node);
    debug_log(node, "recorded snapshot %u: active=%d sent=%d",
              snapshot_id, node->snapshot_recorded_active, node->sent_count);

    /* Markers travel independently in each direction on a bidirectional channel. */
    for (int i = 0; i < node->channel_count; i++) {
        send_marker(node, node->channels[i].neighbor_id, snapshot_id);
    }
}

static int try_finish_snapshot(Node *node, uint32_t snapshot_id)
{
    if (node->snapshot_report_sent || !all_markers_seen(node) ||
        node->snapshot_reports_received != node->snapshot_report_children) {
        return 0;
    }

    uint32_t local_messages = 0;
    for (int i = 0; i < node->channel_count; i++) {
        local_messages += (uint32_t)node->snapshot_channels_messages[i];
    }
    node->snapshot_subtree_messages += local_messages;
    node->snapshot_report_sent = 1;

    int parent = node->snapshot_tree_parent[node->id];
    if (parent >= 0) {
        send_report(node, parent, snapshot_id,
                    node->snapshot_subtree_active, node->snapshot_subtree_messages);
        return 0;
    }

    node->snapshot_complete = 1;
    int terminated = node->snapshot_subtree_active == 0 &&
                     node->snapshot_subtree_messages == 0;
    fprintf(stdout,
            "Snapshot %u complete: active_nodes=%u in_flight_messages=%u%s\n",
            snapshot_id, node->snapshot_subtree_active, node->snapshot_subtree_messages,
            terminated ? " (MAP terminated)" : "");
    fflush(stdout);
    debug_log(node, "completed snapshot %u: active=%u in_flight=%u",
              snapshot_id, node->snapshot_subtree_active, node->snapshot_subtree_messages);
    return terminated;
}

static void *snapshot_timer(void *arg)
{
    Node *node = (Node *)arg;
    while (1) {
        if (node->config->snapshot_delay > 0) {
            usleep((useconds_t)node->config->snapshot_delay * 1000);
        } else {
            usleep(1000);
        }
        pthread_mutex_lock(&node->state_mutex);
        if (node->halt_requested) {
            pthread_mutex_unlock(&node->state_mutex);
            break;
        }
        if (node->snapshot_complete) {
            int next_snapshot = node->snapshot_sequence++;
            begin_snapshot(node, (uint32_t)next_snapshot, -1);
            fprintf(stdout, "Node %d initiated snapshot %d\n", node->id, next_snapshot);
            fflush(stdout);
            int terminated = try_finish_snapshot(node, (uint32_t)next_snapshot);
            pthread_mutex_unlock(&node->state_mutex);
            if (terminated) {
                request_halt(node);
            }
            continue;
        }
        pthread_mutex_unlock(&node->state_mutex);
    }
    return NULL;
}

static void update_vector_clock_on_receive(Node *node, int sender_id,
                                           const uint32_t *remote_clock)
{
    if (sender_id < 0 || sender_id >= node->config->node_count) {
        return;
    }

    for (int i = 0; i < node->config->node_count; i++) {
        if (remote_clock[i] > (uint32_t)node->vector_clock[i]) {
            node->vector_clock[i] = (int)remote_clock[i];
        }
    }
    node->vector_clock[node->id]++;
}

static void *receive_messages(void *arg)
{
    Channel *channel = (Channel *)arg;
    Node *node = channel->node;
    uint32_t *remote_clock = malloc((size_t)node->config->node_count * sizeof(*remote_clock));
    if (remote_clock == NULL) {
        perror("malloc receive vector clock");
        return NULL;
    }

    while (1) {
        WireMessage message;
        if (receive_all(channel->socket_fd, &message, sizeof(message)) != 0) {
            debug_log(node, "channel from node %d closed", channel->neighbor_id);
            break;
        }

        uint32_t type = ntohl(message.type);
        uint32_t snapshot_id = ntohl(message.snapshot_id);
        uint32_t sender = ntohl(message.sender);
        uint32_t value = ntohl(message.value);
        uint32_t auxiliary = ntohl(message.auxiliary);

        if (type == APPLICATION_MESSAGE) {
            int read_failed = 0;
            for (int i = 0; i < node->config->node_count; i++) {
                if (receive_all(channel->socket_fd, &remote_clock[i], sizeof(remote_clock[i])) != 0) {
                    read_failed = 1;
                    break;
                }
                remote_clock[i] = ntohl(remote_clock[i]);
            }
            if (read_failed) {
                fprintf(stderr, "Node %d: incomplete vector timestamp from node %u.\n",
                        node->id, sender);
                break;
            }

            if ((int)sender != channel->neighbor_id) {
                fprintf(stderr, "Node %d received an application message with sender %u on channel %d.\n",
                        node->id, sender, channel->neighbor_id);
                continue;
            }

            pthread_mutex_lock(&node->state_mutex);
            if (node->snapshot_active && node->snapshot_id >= 0) {
                int neighbor_index = channel_index(node, (int)sender);
                if (neighbor_index >= 0 && node->snapshot_channel_markers[neighbor_index] == 0) {
                    node->snapshot_channels_messages[neighbor_index]++;
                }
            }
            update_vector_clock_on_receive(node, (int)sender, remote_clock);
            if (node->sent_count < node->config->max_number && !node->active) {
                node->active = 1;
                int range = node->config->max_per_active - node->config->min_per_active + 1;
                node->burst_remaining = node->config->min_per_active + (int)(rand_r(&node->random_seed) % (unsigned int)range);
                if (node->burst_remaining > node->config->max_number - node->sent_count) {
                    node->burst_remaining = node->config->max_number - node->sent_count;
                }
                pthread_cond_signal(&node->state_changed);
            }
            debug_log(node, "received application message from node %u (wire value=%u), clock[%d]=%d",
                      sender, value, node->id, node->vector_clock[node->id]);
            pthread_mutex_unlock(&node->state_mutex);
            continue;
        }

        if (type == MARKER_MESSAGE) {
            int incoming_index = channel_index(node, (int)sender);
            if (incoming_index < 0) {
                fprintf(stderr, "Node %d received a marker from non-neighbor %u.\n", node->id, sender);
                continue;
            }

            pthread_mutex_lock(&node->state_mutex);
            if (node->snapshot_active && (int)snapshot_id < node->snapshot_id) {
                pthread_mutex_unlock(&node->state_mutex);
                debug_log(node, "ignored stale marker for snapshot %u from node %u",
                          snapshot_id, sender);
                continue;
            }
            if (!node->snapshot_active || node->snapshot_id != (int)snapshot_id) {
                begin_snapshot(node, snapshot_id, (int)sender);
            }
            node->snapshot_channel_markers[incoming_index] = 1;
            debug_log(node, "received marker for snapshot %u from node %u",
                      snapshot_id, sender);
            int terminated = try_finish_snapshot(node, snapshot_id);
            pthread_mutex_unlock(&node->state_mutex);
            if (terminated) {
                request_halt(node);
            }
            continue;
        }

        if (type == REPORT_MESSAGE) {
            pthread_mutex_lock(&node->state_mutex);
            int valid_report = node->snapshot_active &&
                               node->snapshot_id == (int)snapshot_id &&
                               sender < (uint32_t)node->config->node_count &&
                               node->snapshot_tree_parent[sender] == node->id;
            if (!valid_report) {
                debug_log(node, "ignored unexpected report for snapshot %u from node %u",
                          snapshot_id, sender);
                pthread_mutex_unlock(&node->state_mutex);
                continue;
            }
            if (node->snapshot_child_reports_received[sender]) {
                debug_log(node, "ignored duplicate report for snapshot %u from node %u",
                          snapshot_id, sender);
                pthread_mutex_unlock(&node->state_mutex);
                continue;
            }
            node->snapshot_child_reports_received[sender] = 1;
            node->snapshot_reports_received++;
            node->snapshot_subtree_active += value;
            node->snapshot_subtree_messages += auxiliary;
            debug_log(node, "received snapshot %u report from node %u: active=%u in_flight=%u",
                      snapshot_id, sender, value, auxiliary);
            int terminated = try_finish_snapshot(node, snapshot_id);
            pthread_mutex_unlock(&node->state_mutex);
            if (terminated) {
                request_halt(node);
            }
            continue;
        }

        if (type == HALT_MESSAGE) {
            forward_halt(node, (int)sender);
            break;
        }

        fprintf(stderr, "Node %d received unknown message type %u from node %u.\n",
                node->id, type, sender);
    }

    free(remote_clock);
    return NULL;
}

static int establish_channels(Node *node)
{
    /* Establish TCP socket */
    int listener = create_listener(node->config->nodes[node->id].port);
    if (listener < 0)
    {  
        perror("listen"); 
        return -1;
    }

    /* Determine the number of communication channels based on the node's neighbors */
    node->channel_count = node->config->neighbor_counts[node->id];
    if (node->channel_count == 0 && node->config->max_number > 0) {
        fprintf(stderr, "No channels available for node %d, but must send message.\n", node->id);
        close(listener);
        return -1;
    }

    /* Allocate memory for the communication channels and initialize them */
    node->channels = calloc((size_t)node->channel_count, sizeof(*node->channels));
    node->snapshot_channel_markers = calloc((size_t)node->channel_count, sizeof(*node->snapshot_channel_markers));
    node->snapshot_channels_messages = calloc((size_t)node->channel_count, sizeof(*node->snapshot_channels_messages));
    if(node->channels == NULL || node->snapshot_channel_markers == NULL || node->snapshot_channels_messages == NULL)
    {
        perror("calloc");
        close(listener);
        return -1;
    }

    for(int i = 0; i < node->channel_count; i++)
    {
        node->channels[i].node = node;
        node->channels[i].neighbor_id = node->config->neighbors[node->id][i];
        node->channels[i].socket_fd = -1;
        pthread_mutex_init(&node->channels[i].send_mutex, NULL);
    }

    /* Accept incoming connections from neighbors with higher IDs */
    for(int i = 0; i < node->channel_count; i++)
    {
        int neighbor = node->channels[i].neighbor_id;
        if (node->id < neighbor)
        {
            int socket_fd;
            do {
                socket_fd = connect_to_node(&node->config->nodes[neighbor]);
                if (socket_fd < 0) usleep(100000); // Sleep for 100ms before retrying
            } while (socket_fd < 0);
            if (send_handshake(socket_fd, node->id) != 0) {
                perror("send_handshake");
                close(socket_fd);
                return -1;
            }
            node->channels[i].socket_fd = socket_fd;
        }
    }

    /* Accept incoming connections from neighbors with lower IDs */
    int remaining = 0;
    for (int i = 0; i < node->channel_count; i++) if (node->id > node->channels[i].neighbor_id) remaining++;
    while (remaining > 0) {
        struct sockaddr_in addr;
        socklen_t addrlen = sizeof(addr);
        int socket_fd = accept(listener, (struct sockaddr *)&addr, &addrlen);
        if (socket_fd < 0) {
            perror("accept");
            continue;
        }
        int neighbor = receive_handshake(socket_fd);
        int index = channel_index(node, neighbor);
        if (index < 0 || node->id < neighbor) {
            close(socket_fd);
            continue;
        }
        node->channels[index].socket_fd = socket_fd;
        remaining--;
    }
    close(listener);

    for (int i = 0; i < node->channel_count; i++) {
        int thread_error = pthread_create(&node->channels[i].receiver_thread, NULL,
                                          receive_messages, &node->channels[i]);
        if (thread_error != 0) {
            fprintf(stderr, "Node %d: pthread_create receiver for node %d: %s\n",
                    node->id, node->channels[i].neighbor_id, strerror(thread_error));
            return -1;
        }
        node->channels[i].receiver_started = 1;
    }

    return 0;
}

static int send_application_message(Node *node, int target_index, const uint32_t *timestamp)
{
    if (target_index < 0 || target_index >= node->channel_count) {
        fprintf(stderr, "Node %d selected invalid channel index %d.\n", node->id, target_index);
        return -1;
    }

    Channel *channel = &node->channels[target_index];
    WireMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = htonl(APPLICATION_MESSAGE);
    msg.snapshot_id = 0;
    msg.sender = htonl((uint32_t)node->id);
    msg.value = htonl(timestamp[node->id]);
    pthread_mutex_lock(&channel->send_mutex);

    int failed = channel->socket_fd < 0 ||
                 send_all(channel->socket_fd, &msg, sizeof(msg)) != 0;
    for (int i = 0; !failed && i < node->config->node_count; i++) {
        uint32_t component = htonl(timestamp[i]);
        if (send_all(channel->socket_fd, &component, sizeof(component)) != 0) {
            failed = 1;
        }
    }
    if (failed) {
        perror("send_all");
    }
    pthread_mutex_unlock(&channel->send_mutex);
    if (!failed) {
        debug_log(node, "sent application message to node %d with clock component %d=%d",
                  channel->neighbor_id, node->id, node->vector_clock[node->id]);
    }
    return failed ? -1 : 0;
}

int run_node(Node *node)
{
    node->vector_clock = calloc((size_t)node->config->node_count, sizeof(*node->vector_clock));
    node->snapshot_child_reports_received =
        calloc((size_t)node->config->node_count, sizeof(*node->snapshot_child_reports_received));
    if (node->vector_clock == NULL || node->snapshot_child_reports_received == NULL) {
        perror("calloc node state");
        return -1;
    }
    if (build_snapshot_tree(node) != 0) {
        return -1;
    }

    node->random_seed = (unsigned int)time(NULL) ^ (unsigned int)getpid() ^ (unsigned int)node->id;
    node->snapshot_complete = 1;
    if (node->id == 0 && node->config->max_number > 0) {
        node->active = 1;
        int range = node->config->max_per_active - node->config->min_per_active + 1;
        node->burst_remaining = node->config->min_per_active +
            (int)(rand_r(&node->random_seed) % (unsigned int)range);
        if (node->burst_remaining > node->config->max_number) {
            node->burst_remaining = node->config->max_number;
        }
    }

    uint32_t *timestamp = malloc((size_t)node->config->node_count * sizeof(*timestamp));
    if (timestamp == NULL) {
        perror("malloc message timestamp");
        return -1;
    }

    if (establish_channels(node) != 0) {
        free(timestamp);
        return -1;
    }

    if (node->id == 0) {
        int thread_error = pthread_create(&node->snapshot_timer_thread, NULL, snapshot_timer, node);
        if (thread_error != 0) {
            fprintf(stderr, "Node %d: pthread_create snapshot timer: %s\n",
                    node->id, strerror(thread_error));
            free(timestamp);
            return -1;
        }
    }

    int result = 0;
    pthread_mutex_lock(&node->state_mutex);
    while (!node->halt_requested) {
        while (!node->active && !node->halt_requested) {
            pthread_cond_wait(&node->state_changed, &node->state_mutex);
        }
        if (node->halt_requested) {
            break;
        }

        node->vector_clock[node->id]++;
        int target = (int)(rand_r(&node->random_seed) % (unsigned int)node->channel_count);
        node->sent_count++;
        node->burst_remaining--;
        if (node->burst_remaining <= 0 || node->sent_count >= node->config->max_number) {
            node->active = 0;
        }
        int active_after_send = node->active;
        for (int i = 0; i < node->config->node_count; i++) {
            timestamp[i] = (uint32_t)node->vector_clock[i];
        }
        debug_log(node, "sending application message %d/%d through channel %d to node %d",
                  node->sent_count, node->config->max_number, target,
                  node->channels[target].neighbor_id);
        int send_failed = send_application_message(node, target, timestamp) != 0;
        pthread_mutex_unlock(&node->state_mutex);
        if (send_failed) {
            result = -1;
            request_halt(node);
        }
        if (active_after_send && node->config->min_send_delay > 0) {
            usleep((useconds_t)node->config->min_send_delay * 1000);
        }
        pthread_mutex_lock(&node->state_mutex);
    }
    pthread_mutex_unlock(&node->state_mutex);

    if (node->id == 0) {
        pthread_join(node->snapshot_timer_thread, NULL);
    }
    for (int i = 0; i < node->channel_count; i++) {
        if (node->channels[i].receiver_started) {
            pthread_join(node->channels[i].receiver_thread, NULL);
        }
    }
    for (int i = 0; i < node->channel_count; i++) {
        if (node->channels[i].socket_fd >= 0) {
            close(node->channels[i].socket_fd);
            node->channels[i].socket_fd = -1;
        }
        pthread_mutex_destroy(&node->channels[i].send_mutex);
    }
    free(timestamp);

    return result;
}