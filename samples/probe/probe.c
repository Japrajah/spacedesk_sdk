#include "spacedesk_sdk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

int main(int argc, char** argv) {
    sd_config config;
    sd_client* client = NULL;
    sd_result result;
    int websocket = argc > 1 && strcmp(argv[1], "--websocket") == 0;
    int frames = 0;
    int wanted;
    unsigned long port;
    ULONGLONG deadline;
    if (websocket) { ++argv; --argc; }
    wanted = argc > 3 ? atoi(argv[3]) : 1;
    port = argc > 2 ? strtoul(argv[2], NULL, 10) : 28252;
    if (argc > 1 && strcmp(argv[1], "--discover") == 0) {
        sd_host hosts[32] = {0}; uint32_t count = 0, i;
        unsigned long udp_port = argc > 3 ? strtoul(argv[3], NULL, 10) : 28252;
        if (!udp_port || udp_port > 65535) return 2;
        for (i = 0; i < 32; ++i) hosts[i].struct_size = sizeof(sd_host);
        result = sd_discover(argc > 2 ? argv[2] : NULL, (uint16_t)udp_port, 1500, hosts, 32, &count);
        if (result != SD_OK) { fprintf(stderr, "Discovery failed: %d\n", result); return 1; }
        for (i = 0; i < count; ++i) printf("%s:%u  %s  protocol=%u.%u features=0x%x\n", hosts[i].ipv4,
            hosts[i].port, hosts[i].hostname, hosts[i].protocol_major, hosts[i].protocol_minor, hosts[i].feature_flags);
        printf("%u host(s)\n", count); return 0;
    }
    if (argc < 2 || port > 65535 || !port || wanted < 1) {
        fprintf(stderr, "Usage: sd_probe [--websocket] IPv4 [port=28252] [frames=1]\n       sd_probe --discover [target-IPv4] [port=28252]\n");
        return 2;
    }
    sd_config_init(&config);
    result = sd_client_create(&config, &client);
    if (result != SD_OK) { fprintf(stderr, "Create failed: %d\n", result); return 1; }
    if (websocket) {
        result = sd_client_set_transport(client, SD_TRANSPORT_WEBSOCKET);
        if (result != SD_OK) goto done;
    }
    result = sd_client_connect(client, argv[1], (uint16_t)port);
    if (result != SD_OK) goto done;
    deadline = GetTickCount64() + 15000;
    while (frames < wanted && GetTickCount64() < deadline) {
        sd_event event = {0};
        event.struct_size = sizeof(event);
        result = sd_client_poll(client, 100, &event);
        if (result < 0 || result == SD_DISCONNECTED) break;
        if (event.type == SD_EVENT_FRAME) {
            sd_frame frame = {0};
            frame.struct_size = sizeof(frame);
            result = sd_client_get_frame(client, &frame);
            if (result != SD_OK) break;
            printf("Frame %llu: %u x %u, BGRA stride=%u\n",
                (unsigned long long)frame.sequence, frame.width, frame.height, frame.stride);
            ++frames;
        }
    }
done:
    if (frames != wanted) fprintf(stderr, "No complete frame / stopped (%d): %s\n", result, sd_client_last_error(client));
    sd_client_disconnect(client);
    sd_client_destroy(client);
    return frames == wanted ? 0 : 1;
}
