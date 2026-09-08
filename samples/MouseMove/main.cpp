#include "../common/sample_client.h"

int main() {
    return sample::Run([] {
        // Find by IP or name; leave both empty to discover and select a host.
        const auto host = sample::find_host(L"", L"", 28252);
        sample::Connection client;
        client.connect(host);
        client.wait_frame();

        // Move the remote mouse 100 pixels right (relative movement).
        client.mouse_move(100, 0);

        // Absolute alternative: pixels in the received display image.
        // client.mouse_move(320, 240, true);

        // Process connection events for 10 seconds, then disconnect.
        sample::ConnectionLoop loop(10000);
        while (loop.running()) {
            client.poll(loop.poll_timeout());
        }
    });
}
