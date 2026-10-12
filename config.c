#include "map.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *trim_comment(char *line)
{
    /* Trim comment */
    char *comment = strchr(line, '#'); // Find the first occurrence of '#' and treat it as the start of a comment
    if (comment) {
        *comment = '\0';
    }

    /* Trim leading whitespace */
    while (isspace((unsigned char)*line)) {
        line++;
    }

    /* Trim trailing whitespace */
    char *end = line + strlen(line) - 1;
    while (end > line && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    /* Return the trimmed line */
    return line;
}

static int begins_with_unsigned_integer(const char *line) {
    return line != NULL && isdigit((unsigned char)*line);
}

static int parse_valid_lines(const char *path, char ***lines_out, int *count_out)
{
    FILE *file = fopen(path, "r");
    if (!file) {
        perror(path);
        return -1; // Return non-zero if the file cannot be opened
    }

    char **lines = NULL;
    int count = 0;
    int capacity = 0;
    char buffer[MAX_LINE];

    /* Read and parse valid lines from the file */
    while(fgets(buffer, sizeof(buffer), file) != NULL) {
        char *line = trim_comment(buffer);
        if(!begins_with_unsigned_integer(line)) {
            continue; // Skip lines that are empty or do not start with a digit
        }

        /* Allocate more space if necessary */
        if (count >= capacity) {
            capacity = capacity == 0 ? 8 : capacity * 2; // Double the capacity if needed, starting with 8 if capacity is 0
            char **new_lines = realloc(lines, (size_t)capacity * sizeof(char *));
            if (!new_lines) { // Handle memory allocation failure for realloc
                perror("realloc");
                goto fail;
            }

            /* Update the lines pointer to the newly allocated memory */
            lines = new_lines;
        }
        lines[count] = strdup(line);
        if (!lines[count]) { // Handle memory allocation failure for strdup
            perror("strdup");
            goto fail;
        }
        count++;
    }

    /* Set values to return */
    *lines_out = lines;
    *count_out = count;

    fclose(file);
    return 0; // Return 0 on success


    /* Failure handling in one place so it looks nicer */
    fail:
        fclose(file);
        for (int i = 0; i < count; i++) {
            free(lines[i]);
        }
        free(lines);
        return -1;
}

int load_config(const char *path, Config *config)
{
    memset(config, 0, sizeof(Config));
    char **lines = NULL;
    int line_count = 0;
    if (parse_valid_lines(path, &lines, &line_count) != 0 || line_count < 1) {
        fprintf(stderr, "Failed to parse configuration file or no valid lines found.\n");
        return -1; // Return non-zero on failure
    }

    /* Parse the first line of the configuration file into the Config structure */
    int parsed = sscanf(lines[0], "%d %d %d %d %d %d",
                        &config->node_count,
                        &config->min_per_active,
                        &config->max_per_active,
                        &config->min_send_delay,
                        &config->snapshot_delay,
                        &config->max_number);
    /* Validate the parsed configuration values */
    if (parsed != 6 || 
        config->node_count <= 0 || 
        config->min_per_active < 0 || 
        config->max_per_active < config->min_per_active || 
        config->max_number < 0) 
    {
        fprintf(stderr, "Invalid configuration values.\n");
        goto fail;
    }

    /* Validate line count*/
    if (line_count < 1 + 2 * config->node_count) {
        fprintf(stderr, "Insufficient number of lines in configuration file.\n");
        goto fail;
    }

    /* Allocations! */
    config->nodes = calloc((size_t)config->node_count, sizeof(*config->nodes));
    config->neighbors = calloc((size_t)config->node_count, sizeof(*config->neighbors));
    config->neighbor_counts = calloc((size_t)config->node_count, sizeof(*config->neighbor_counts));
    if (!config->nodes || !config->neighbors || !config->neighbor_counts) {
        fprintf(stderr, "Memory allocation failed.\n");
        goto fail;
    }

    /* Parse node configurations*/
    for (int i = 0; i < config->node_count; i++) {
        int id;
        if (sscanf(lines[1 + i], "%d %255s %d", &id, config->nodes[i].host, &config->nodes[i].port) != 3) {
            fprintf(stderr, "Invalid node configuration at line %d.\n", 1 + i);
            goto fail;
        }
        config->nodes[i].node_id = id;
    }

    /* Parse neighbor configurations */
    for (int i = 0; i < config->node_count; i++) {
        char *cursor = lines[1 + config->node_count + i]; // Pointer to the current line containing neighbor IDs for node i
        int neighbor;
        while (sscanf(cursor, "%d", &neighbor) == 1) {
            if (neighbor < 0 || neighbor >= config->node_count || neighbor == i) {
                fprintf(stderr, "Invalid neighbor configuration at line %d.\n", 1 + config->node_count + i);
                goto fail;
            }
            int count = config->neighbor_counts[i];
            int *new_neighbors = realloc(config->neighbors[i], (size_t)(count + 1) * sizeof(int));
            if (!new_neighbors) {
                fprintf(stderr, "Memory allocation failed.\n");
                goto fail;
            }
            config->neighbors[i] = new_neighbors;
            config->neighbors[i][count] = neighbor;
            config->neighbor_counts[i]++;
            while (isdigit((unsigned char)*cursor)) cursor++;
            while (isspace((unsigned char)*cursor)) cursor++;
        }
    }

    /* Free the lines */
    for (int i = 0; i < line_count; i++) {
        free(lines[i]);
    }
    free(lines);

    return 0; // Return 0 on success

        /* Failure handling in one place so it looks nicer */
    fail:
        for (int j = 0; j < line_count; j++) {
            free(lines[j]);
        }
        free(lines);
        free_config(config);
        return -1;
}

void free_config(Config *config)
{
    if (!config) return; // No config to free, so return immediately
    free(config->nodes); 
    for (int i = 0; i < config->node_count; i++) {
        free(config->neighbors[i]);
    }
    free(config->neighbors);
    free(config->neighbor_counts);
    memset(config, 0, sizeof(*config));
}