#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cJSON.h"
#include "room_files.h"

static esp_openclaw_node_command_handler_t write_handler;

esp_err_t esp_openclaw_node_register_command(
    esp_openclaw_node_handle_t node, const esp_openclaw_node_command_t *command)
{
    assert(node != NULL);
    if (strcmp(command->name, "file.write") == 0) write_handler = command->handler;
    return ESP_OK;
}

#if defined(__linux__)
size_t strlcpy(char *destination, const char *source, size_t size)
{
    size_t length = strlen(source);
    if (size != 0) {
        size_t count = length < size - 1 ? length : size - 1;
        memcpy(destination, source, count);
        destination[count] = '\0';
    }
    return length;
}
#endif

static bool exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static void write_file(const char *path, bool create_parents, bool preflight,
    esp_err_t expected, const char *expected_error)
{
    cJSON *params = cJSON_CreateObject();
    assert(params != NULL);
    cJSON_AddStringToObject(params, "path", path);
    cJSON_AddStringToObject(params, "contentBase64", "aGVsbG8=");
    cJSON_AddBoolToObject(params, "createParents", create_parents);
    cJSON_AddBoolToObject(params, "preflightOnly", preflight);
    char *json = cJSON_PrintUnformatted(params);
    assert(json != NULL);
    char *output = NULL;
    esp_openclaw_node_error_t error = {0};
    esp_err_t result = write_handler(NULL, NULL, json, strlen(json), &output, &error);
    if (result != expected) {
        fprintf(stderr, "file.write: expected %d, got %d (%s)\n", expected, result,
            error.code != NULL ? error.code : "no error");
        abort();
    }
    if (expected == ESP_OK) {
        cJSON *response = cJSON_Parse(output);
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "ok")));
        assert(strcmp(cJSON_GetObjectItemCaseSensitive(response, "path")->valuestring, path) == 0);
        assert(strcmp(cJSON_GetObjectItemCaseSensitive(response, "sha256")->valuestring,
            "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824") == 0);
        assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(response, "preflightOnly")) == preflight);
        cJSON_Delete(response);
        if (!preflight) {
            char bytes[8] = {0};
            FILE *file = fopen(path, "rb");
            assert(file != NULL);
            assert(fread(bytes, 1, sizeof(bytes), file) == 5);
            assert(fclose(file) == 0);
            assert(strcmp(bytes, "hello") == 0);
        }
    } else {
        assert(output == NULL);
        assert(error.code != NULL && strcmp(error.code, expected_error) == 0);
    }
    free(output);
    free(json);
    cJSON_Delete(params);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    assert(chdir(argv[2]) == 0);
    assert(mkdir("root", 0700) == 0);
    char root_path[4096];
    snprintf(root_path, sizeof(root_path), "%s/root", argv[2]);
    char *root = realpath(root_path, NULL);
    assert(root != NULL);
    assert(room_files_register_node_commands((esp_openclaw_node_handle_t)1, root) == ESP_OK);
    assert(write_handler != NULL);
    char path[4096];
    if (strcmp(argv[1], "root-loss") == 0 || strcmp(argv[1], "root-loss-nested") == 0) {
        assert(rmdir("root") == 0);
        snprintf(path, sizeof(path), "%s/%s", root,
            strcmp(argv[1], "root-loss") == 0 ? "note.txt" : "new/deep/note.txt");
        write_file(path, true, false, ESP_OK, NULL);
        puts("registered file.write recreated missing root and preserved content/hash");
    } else if (strcmp(argv[1], "preflight") == 0) {
        snprintf(path, sizeof(path), "%s/new/note.txt", root);
        write_file(path, true, true, ESP_OK, NULL);
        assert(!exists("root/new"));
        snprintf(path, sizeof(path), "%s/new/deep/note.txt", root);
        write_file(path, true, true, ESP_OK, NULL);
        assert(!exists("root/new"));
        write_file(path, true, false, ESP_OK, NULL);
        puts("registered file.write preflight accepted missing parents without mutation; real write succeeded");
    } else if (strcmp(argv[1], "boundaries") == 0) {
        snprintf(path, sizeof(path), "%s/new/note.txt", root);
        write_file(path, false, false, ESP_ERR_NOT_FOUND, "PARENT_NOT_FOUND");
        assert(!exists("root/new"));
        assert(mkdir("outside", 0700) == 0);
        assert(symlink("../outside", "root/link") == 0);
        snprintf(path, sizeof(path), "%s/link/new/note.txt", root);
        write_file(path, true, true, ESP_ERR_INVALID_ARG, "SYMLINK_REDIRECT");
        write_file(path, true, false, ESP_ERR_INVALID_ARG, "SYMLINK_REDIRECT");
        assert(!exists("outside/new"));
        assert(unlink("root/link") == 0);
        assert(rmdir("root") == 0);
        snprintf(path, sizeof(path), "%s/new/note.txt", root);
        write_file(path, true, true, ESP_ERR_NOT_FOUND, "PARENT_NOT_FOUND");
        assert(!exists("root"));
        puts("registered file.write retained missing-parent and symlink boundaries");
    } else {
        abort();
    }
    free(root);
    return 0;
}
