#include "map.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
    /* Check for correct number of arguments */
    if (argc != 3) 
    {
        fprintf(stderr, "usage: %s <config-file> <node-id>\n", argv[0]);
        return 1; //Exit failure
    }

    /* Convert node-id argument to integer and validate it */
    char *end = NULL;
    long node_id = strtol(argv[2], &end, 10); //Convert node-id argument to long
    if (*argv[2] == '\0' || *end != '\0') //Check if the entire argument was a valid number
    {
        fprintf(stderr, "node-id must be integer. invalid node-id: %s\n", argv[2]);
        return 1; //Exit failure
    }

    Config config;
    if (load_config(argv[1], &config) != 0 || node_id < 0 || node_id >= config.node_count)
    {
        perror("load_config or invalid node_id");
        free_config(&config);
        return 1; //Exit failure
    }

    /* Initialize the node structure */
    Node node = {0};
    node.id = (int)node_id;
    node.config = &config;
    snprintf(node.output_path, sizeof(node.output_path), "%s", argv[1]);
    char *dot = strrchr(node.output_path, '.');
    if (dot != NULL) {
        *dot = '\0';
    }
    snprintf(node.output_path + strlen(node.output_path), sizeof(node.output_path) - strlen(node.output_path), "-%d.out", node.id);
    pthread_mutex_init(&node.state_mutex, NULL);
    pthread_cond_init(&node.state_changed, NULL);

    /* Run the node */
    int result = run_node(&node);

    /* Clean up the node's resources */
    free(node.channels);
    free(node.snapshot_channel_markers);
    free(node.snapshot_channels_messages);
    free(node.snapshot_tree_parent);
    free(node.snapshot_child_reports_received);
    free(node.vector_clock);
    pthread_mutex_destroy(&node.state_mutex);
    pthread_cond_destroy(&node.state_changed);
    free_config(&config);


    return (result != 0); // Exit with 0 on success, 1 on failure
}