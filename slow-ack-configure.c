#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h> // Added for signal handling
#include <poll.h>   // Added for poll
#include <errno.h>  // Added for errno

#include "client.h"

#define FRAME_DELAY 32

uint32_t default_width = 300;
uint32_t default_height = 400;

struct configure {
	uint32_t serial;
	uint32_t width, height;
};

static bool acked_first_configure = false;
static struct configure current_configure = { 0 }, next_configure = { 0 };
static uint32_t countdown = 0;
static struct wleird_toplevel toplevel = {0};
static const struct wl_callback_listener callback_listener;
static volatile sig_atomic_t render_on_sigusr1 = 0; // Flag to indicate SIGUSR1 reception

static void request_frame_callback(void) {
	struct wl_callback *callback = wl_surface_frame(toplevel.surface.wl_surface);
	wl_callback_add_listener(callback, &callback_listener, NULL);
	wl_surface_commit(toplevel.surface.wl_surface);
}

static void callback_handle_done(void *data, struct wl_callback *callback,
		uint32_t time_ms) {
	if (callback != NULL) {
		wl_callback_destroy(callback);
	}

	countdown--;
	if (countdown > 0) {
		request_frame_callback();
		return;
	}

	fprintf(stderr, "acking configure %d, width: %d, height: %d\n",
		current_configure.serial, current_configure.width, current_configure.height);
	toplevel.surface.width = current_configure.width ?: default_width;
	toplevel.surface.height = current_configure.height ?: default_height;
	xdg_surface_ack_configure(toplevel.xdg_surface, current_configure.serial);
	surface_render(&toplevel.surface);

	if (next_configure.serial == current_configure.serial) {
		return;
	}

	countdown = FRAME_DELAY;
	current_configure = next_configure;
	request_frame_callback();
}

static const struct wl_callback_listener callback_listener = {
	.done = callback_handle_done,
};

static void xdg_surface_handle_configure(void *data,
		struct xdg_surface *xdg_surface, uint32_t serial) {
	struct wleird_toplevel *toplevel = data;

	if (!acked_first_configure) {
		xdg_surface_ack_configure(toplevel->xdg_surface, serial);
		surface_render(&toplevel->surface);
		acked_first_configure = true;
		return;
	}

	next_configure.serial = serial;

	if (countdown == 0) {
		current_configure = next_configure;
		countdown = FRAME_DELAY;
		request_frame_callback();
	}
}

static void xdg_toplevel_handle_configure(void *data,
		struct xdg_toplevel *xdg_toplevel, int32_t w, int32_t h,
		struct wl_array *states) {
	if (w == 0 || h == 0) {
		return;
	}

	next_configure.width = w;
	next_configure.height = h;
}

// Signal handler for SIGUSR1
static void sigusr1_handler(int signum) {
    render_on_sigusr1 = 1; // Set the flag to trigger re-render in main loop
}

int main(int argc, char *argv[]) {
	struct wl_display *display = wl_display_connect(NULL);
	if (display == NULL) {
		fprintf(stderr, "failed to create display\n");
		return EXIT_FAILURE;
	}

	default_width = argc >= 2 ? atoi(argv[1]) : 300;
	default_height = argc >= 3 ? atoi(argv[2]) : 400;

	xdg_surface_listener.configure = xdg_surface_handle_configure;
	xdg_toplevel_listener.configure = xdg_toplevel_handle_configure;

	// Register SIGUSR1 handler
	struct sigaction sa = { .sa_handler = sigusr1_handler };
	sigemptyset(&sa.sa_mask);
	// Do NOT set SA_RESTART. We want poll() to return EINTR if a signal arrives.
	sa.sa_flags = 0;
	if (sigaction(SIGUSR1, &sa, NULL) == -1) {
		perror("sigaction");
		return EXIT_FAILURE;
	}

	registry_init(display);
	toplevel_init(&toplevel, "wleird-slow-ack-configure");

	float color[4] = {1, 0, 0, 1};
	memcpy(toplevel.surface.color, color, sizeof(float[4]));

	// Get the Wayland display file descriptor
	int display_fd = wl_display_get_fd(display);
	if (display_fd < 0) {
		fprintf(stderr, "Failed to get Wayland display FD\n");
		return EXIT_FAILURE;
	}

	struct pollfd pfd = { .fd = display_fd, .events = POLLIN };

	while (true) {
		// Flush any pending requests to the compositor
		// This ensures that requests (like wl_surface_commit) are sent
		// before we wait for events.
		while (wl_display_flush(display) == -1 && errno == EAGAIN);

		// Prepare to read events. This must be called before poll().
		// It might return -1 if there are already events buffered.
		if (wl_display_prepare_read(display) == -1) {
			// Events are already buffered, dispatch them immediately.
			wl_display_dispatch_pending(display);
		} else {
			// Wait for events on the Wayland display FD, or for a signal.
			// Use a small timeout (e.g., 100ms) to allow periodic checks of render_on_sigusr1
			// even if no Wayland events arrive.
			int ret = poll(&pfd, 1, 100); // 100ms timeout

			if (ret == -1) {
				if (errno == EINTR) {
					// Signal received, poll was interrupted.
					// We need to cancel the prepared read before checking the flag.
					wl_display_cancel_read(display);
				} else {
					// An actual error occurred, not just an interruption.
					wl_display_cancel_read(display); // Cancel the prepared read
					fprintf(stderr, "poll failed: %s\n", strerror(errno));
					exit(EXIT_FAILURE);
				}
			} else if (ret > 0 && (pfd.revents & POLLIN)) {
				// Wayland events are available, read them.
				if (wl_display_read_events(display) == -1) {
					fprintf(stderr, "failed to read Wayland events: %s\n", strerror(errno));
					exit(EXIT_FAILURE);
				}
			} else {
				// Timeout occurred (ret == 0) or other revents.
				// No Wayland events, cancel the prepared read.
				wl_display_cancel_read(display);
			}
		}

		// Dispatch any events that have been read (either by wl_display_read_events
		// or those already buffered when prepare_read returned -1).
		wl_display_dispatch_pending(display);

		// Now, check the flag and render if SIGUSR1 was received.
		// This check is now performed after each poll cycle or event dispatch.
		if (render_on_sigusr1) {
			fprintf(stderr, "SIGUSR1 received, re-rendering surface\n");
			surface_render(&toplevel.surface);
			render_on_sigusr1 = 0; // Reset the flag after handling
		}
	}

	// Cleanup (though unreachable in this infinite loop)
	wl_display_disconnect(display);
	return EXIT_SUCCESS;
}
