#ifndef MAP_H
#define MAP_H

#include <pthread.h>
#include <stdint.h>

#define MAX_LINE 4096

typedef struct {
    int node_id;
    char host[256];
    int port;
} NodeInfo;

typedef struct {
    int node_count;
    int min_per_active;
    int max_per_active;
    int min_send_delay;
    int snapshot_delay; // time to wait between initiating snapshots
    int max_number;
    NodeInfo *nodes;
    int **neighbors;
    int *neighbor_counts; 
} Config;

typedef struct Node Node;

int load_config(const char *path, Config *config);
void free_config(Config *config);
int run_node(Node *node);

typedef struct {
    Node *node;
    int neighbor_id;
    int socket_fd;
    pthread_t receiver_thread;
    int receiver_started;
    pthread_mutex_t send_mutex;
} Channel;

struct Node {
    int id;
    Config *config;
    Channel *channels;
    int channel_count;
    int sent_count;
    int active; // flag indicating if the node is active
    int burst_remaining; // number of messages the node can send in the current active period
    unsigned int random_seed; // seed for the node's random number generator
    pthread_mutex_t state_mutex; // mutex to protect node's state
    pthread_cond_t state_changed; // condition variable to signal state changes
    int halt_requested;
    int halted;

    /* Part 2 additions for snapshot handling */
    int snapshot_active;
    int snapshot_id;
    int snapshot_sequence;
    int snapshot_parent;
    int snapshot_local_recorded;
    int snapshot_report_sent;
    int snapshot_complete;
    int snapshot_recorded_active;
    int snapshot_report_children;
    int snapshot_reports_received;
    uint32_t snapshot_subtree_active;
    uint32_t snapshot_subtree_messages;
    int *snapshot_channel_markers; // markers for snapshot channels
    int *snapshot_channels_messages; // messages for snapshot channels
    int *snapshot_tree_parent;
    int *snapshot_child_reports_received;
    pthread_t snapshot_timer_thread;

    /* Part 3 vector clock */
    int *vector_clock; // vector clock for the node
    char output_path[256]; // path to the output file for the vector clock
};

#endif // MAP_H 