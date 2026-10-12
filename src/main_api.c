/*
 * netrescue_api: native host for the browser API.
 *
 * Reads one JSON request per line from stdin (or from the file given as the first argument) and
 * prints one JSON response per line. This is the same nr_api_call() that the website calls through
 * WebAssembly, so it doubles as a C-backend fallback and as a scripting/verification tool:
 *
 *   echo {"cmd":"run_preset","id":"gbn_vs_sr"} | netrescue_api
 */
#include "api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    FILE *input = stdin;
    if (argc > 1) {
        input = fopen(argv[1], "r");
        if (input == NULL) { fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    }
    size_t capacity = 1U << 16U;
    char *line = malloc(capacity);
    if (line == NULL) return 2;
    int failures = 0;
    for (;;) {
        size_t length = 0U; int c;
        while ((c = fgetc(input)) != EOF && c != '\n') {
            if (length + 2U >= capacity) {
                char *grown = realloc(line, capacity * 2U);
                if (grown == NULL) { free(line); return 2; }
                line = grown; capacity *= 2U;
            }
            line[length++] = (char)c;
        }
        line[length] = '\0';
        if (length > 0U && line[length - 1U] == '\r') line[--length] = '\0';
        if (length > 0U) {
            const char *response = nr_api_call(line);
            if (strncmp(response, "{\"ok\":true", 10) != 0) ++failures;
            puts(response);
            fflush(stdout);
        }
        if (c == EOF) break;
    }
    free(line);
    if (input != stdin) fclose(input);
    nr_api_shutdown();
    return failures == 0 ? 0 : 1;
}
