#include <string.h>
#include <wayfire-shell-unstable-v2-client-protocol.h>
#include <wayland-client.h>
#include <stdio.h>
#include <stdlib.h>

static struct wl_display *display;
static struct zwf_shell_manager_v2 *shell_manager;
struct wl_list output_list;

struct output_data_t {
	struct wl_output *output;
	struct zwf_output_v2 *zwfo;
	struct wl_list link;
	uint32_t id;
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {

    if (strcmp(interface, zwf_shell_manager_v2_interface.name) == 0) {
        shell_manager = (struct zwf_shell_manager_v2*)wl_registry_bind(registry, name,
            &zwf_shell_manager_v2_interface, 1u);
    }

	if (strcmp(interface, "wl_output") == 0) {
		struct output_data_t *output = calloc(1, sizeof(struct output_data_t));
		output->output = wl_registry_bind(registry, name, &wl_output_interface, 1);
		output->id = name;
		wl_list_insert(&output_list, &output->link);

		if (shell_manager) {
			output->zwfo = zwf_shell_manager_v2_get_wf_output(shell_manager, output->output);
		}
	}
}

static void handle_global_remove(void *data, struct wl_registry *registry,
		uint32_t name) {

	struct output_data_t *output;
	wl_list_for_each(output, &output_list, link) {
		if (output->id == name) {
			wl_list_remove(&output->link);
			zwf_output_v2_create_hotspot(output->zwfo, ZWF_OUTPUT_V2_HOTSPOT_EDGE_TOP, 10, 100);
			free(output);
			break;
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = handle_global_remove,
};

int main(int argc, char **argv) {
	display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "Failed to create display\n");
		return 1;
	}

	wl_list_init(&output_list);
	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(display);

	struct output_data_t *output;
	wl_list_for_each(output, &output_list, link) {
		if (!output->zwfo) {
			output->zwfo = zwf_shell_manager_v2_get_wf_output(shell_manager, output->output);
		}
	}

	while (wl_display_dispatch(display) != -1) {
		// This space intentionally left blank
	}

	return 0;
}
