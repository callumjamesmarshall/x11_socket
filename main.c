// Platform includes
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>

// Standard includes
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


#define FATAL_ERROR(msg, ...) { fprintf(stderr, msg "\n", ##__VA_ARGS__); exit(-1); }


//
// X11 protocol definitions

enum {
    X11_DEFAULT_BORDER = 0,
    X11_DEFAULT_GROUP = 0,
    X11_EXPOSURES_NOT_ALLOWED = 0,
    X11_GC_GRAPHICS_EXPOSURES = 1<<16,

    X11_OPCODE_CREATE_WINDOW = 1,
    X11_OPCODE_MAP_WINDOW = 8,
    X11_OPCODE_CREATE_GC = 16,

    X11_CW_BACK_PIXEL = 1<<1,
    X11_CW_EVENT_MASK = 1<<11,

    X11_EVENT_MASK_KEY_PRESS = 1,
    X11_EVENT_MASK_POINTER_MOTION = 1<<6,
};


typedef struct __attribute__((packed)) {
    uint8_t order;
    uint8_t pad1;
    uint16_t major_version, minor_version;
    uint16_t auth_proto_name_len;
    uint16_t auth_proto_data_len;
    uint16_t pad2;
} connection_request_t;


typedef struct __attribute__((packed)) {
    uint32_t root_id;
    uint32_t colormap;
    uint32_t white, black;
    uint32_t input_mask;
    uint16_t width, height;
    uint16_t width_mm, height_mm;
    uint16_t maps_min, maps_max;
    uint32_t root_visual_id;
    uint8_t backing_store;
    uint8_t save_unders;
    uint8_t depth;
    uint8_t allowed_depths_len;
} screen_t;


typedef struct __attribute__((packed)) {
    uint8_t depth;
    uint8_t bpp;
    uint8_t scanline_pad;
    uint8_t pad[5];
} pixmap_format_t;


typedef struct __attribute__((packed)) {
    uint32_t release;
    uint32_t id_base, id_mask;
    uint32_t motion_buffer_size;
    uint16_t vendor_len;
    uint16_t request_max;
    uint8_t num_screens;
    uint8_t num_pixmap_formats;
    uint8_t image_byte_order;
    uint8_t bitmap_bit_order;
    uint8_t scanline_unit, scanline_pad;
    uint8_t keycode_min, keycode_max;
    uint32_t pad;
    char vendor_string[1];
} connection_reply_success_body_t;


typedef struct __attribute__((packed)) {
    uint8_t success;
    uint8_t pad;
    uint16_t major_version, minor_version;
    uint16_t len;
} connection_reply_header_t;


typedef struct __attribute__((packed)) {
    uint8_t group;
    uint8_t bits;
    uint16_t colormap_entries;
    uint32_t mask_red, mask_green, mask_blue;
    uint32_t pad;
} visual_t;

// End of X11 protocol definitions
//


typedef struct {
    int socket_fd;

    connection_reply_header_t connection_reply_header;
    connection_reply_success_body_t *connection_reply_success_body;

    pixmap_format_t *pixmap_formats; // Points into connection_reply_success_body.
    screen_t *screens; // Points into connection_reply_success_body.

    uint32_t next_resource_id;
    uint32_t graphics_context_id;
    uint32_t window_id;
} state_t;


static void fatal_write(int fd, const void *buf, size_t count) {
    if (write(fd, buf, count) != count) {
        FATAL_ERROR("Failed to write.");
    }
}


static void fatal_read(int fd, void *buf, size_t count) {
    if (recvfrom(fd, buf, count, 0, NULL, NULL) != count) {
        FATAL_ERROR("Failed to read.");
    }
}


void x11_init(state_t *state) {
    // Open socket and connect.
    state->socket_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (state->socket_fd < 0) {
        FATAL_ERROR("Create socket failed");
    }
    struct sockaddr_un serv_addr = { 0 };
    serv_addr.sun_family = AF_UNIX;
    strcpy(serv_addr.sun_path, "/tmp/.X11-unix/X0");
    if (connect(state->socket_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        FATAL_ERROR("Couldn't connect");
    }

    // Read Xauthority.
    char xauth_cookie[4096];
    FILE *xauth_file = fopen("/home/andy/.Xauthority", "rb");
    if (!xauth_file) {
        FATAL_ERROR("Couldn't open .Xauthority.");
    }
    size_t xauth_len = fread(xauth_cookie, 1, sizeof(xauth_cookie), xauth_file);
    if (xauth_len < 0) {
        FATAL_ERROR("Couldn't read from .Xauthority.");
    }
    fclose(xauth_file);

    // Send connection request.
    connection_request_t request = { 0 };
    request.order = 'l';  // Little endian.
    request.major_version = 11;
    request.minor_version =  0;
    request.auth_proto_name_len = 18;
    request.auth_proto_data_len = 16;
    fatal_write(state->socket_fd, &request, sizeof(connection_request_t));
    fatal_write(state->socket_fd, "MIT-MAGIC-COOKIE-1\0\0", 20);
    fatal_write(state->socket_fd, xauth_cookie + xauth_len - 16, 16);

    // Read connection reply header.
    fatal_read(state->socket_fd, &state->connection_reply_header, sizeof(connection_reply_header_t));
    if (state->connection_reply_header.success == 0) {
        FATAL_ERROR("Connection reply indicated failure.");
    }

    // Read rest of connection reply.
    state->connection_reply_success_body = malloc(state->connection_reply_header.len * 4);
    fatal_read(state->socket_fd, state->connection_reply_success_body,
               state->connection_reply_header.len * 4);

    // Set some pointers into the connection reply because they'll be convenient later.
    state->pixmap_formats = (pixmap_format_t *)(state->connection_reply_success_body->vendor_string +
                             state->connection_reply_success_body->vendor_len);
    state->screens = (screen_t *)(state->pixmap_formats +
                                  state->connection_reply_success_body->num_pixmap_formats);

    state->next_resource_id = state->connection_reply_success_body->id_base;
}


static uint32_t generate_id(state_t *state) {
    return state->next_resource_id++;
}


static unsigned popcnt(uint32_t value) {
    int count = 0;
    while (value) {
        count += value & 1;
        value >>= 1;
    }
    return count;
}


void init_gc(state_t *state, uint32_t value_mask, uint32_t *value_list) {
    state->graphics_context_id = generate_id(state);
    uint16_t flag_count = popcnt(value_mask);
    uint16_t len = 4 + flag_count;
    uint32_t packet[4 + 32]; // Ideally length would be 'len' but use 4 + 32 to keep length fixed.
    packet[0] = X11_OPCODE_CREATE_GC | (len<<16);
    packet[1] = state->graphics_context_id;
    packet[2] = state->screens[0].root_id;
    packet[3] = value_mask;
    for (int i = 0; i < flag_count; ++i) {
        packet[4 + i] = value_list[i];
    }

    fatal_write(state->socket_fd, packet, len * 4);
}


void init_window(state_t *state, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 uint32_t window_parent, uint32_t visual, uint32_t value_mask,
                 uint32_t *value_list) {
    state->window_id = generate_id(state);

    uint16_t flag_count = popcnt(value_mask);
    uint16_t len = 8 + flag_count;
    uint32_t packet[8 + 32];

    packet[0] = X11_OPCODE_CREATE_WINDOW | len<<16;
    packet[1] = state->window_id;
    packet[2] = window_parent;
    packet[3] = x | (y<<16);
    packet[4] = w | (h<<16);
    packet[5] = (X11_DEFAULT_BORDER<<16) | X11_DEFAULT_GROUP;
    packet[6] = visual;
    packet[7] = value_mask;
    for (int i = 0; i < flag_count; ++i) {
        packet[8 + i] = value_list[i];
    }

    fatal_write(state->socket_fd, packet, len * 4);
}


void map_window(state_t *state) {
    int const len = 2;
    uint32_t packet[len];
    packet[0] = X11_OPCODE_MAP_WINDOW | (len<<16);
    packet[1] = state->window_id;
    fatal_write(state->socket_fd, packet, 8);
}


int main() {
    state_t state = {0};
    x11_init(&state);
    init_gc(&state, X11_GC_GRAPHICS_EXPOSURES, (uint32_t[]){X11_EXPOSURES_NOT_ALLOWED});
    init_window(&state, 0, 0, 320, 240,
                state.screens[0].root_id, state.screens[0].root_visual_id,
                X11_CW_BACK_PIXEL, (uint32_t[]){0xff00ff});
    map_window(&state);

    while (1) sleep(1);
}
