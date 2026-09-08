#include "../common/sample_client.h"

int main() {
    return sample::Run([] {
        const auto host = sample::find_host(L"", L"", 28252);
        sample::Connection client;
        client.connect(host);
        const auto frame = client.wait_frame();

        // Draw a horizontal line from 20% to 80% of the received display.
        // Open a touch-capable drawing app on the host to see the stroke.
        const int start_x = int((frame.width - 1) / 5);
        const int end_x = int((frame.width - 1) * 4 / 5);
        const int y = int((frame.height - 1) / 2);
        client.touch(sample::Touch::Down, start_x, y);

        sample::ConnectionLoop loop(10000); // Connection lives for 10 seconds.
        uint32_t next_move_ms = 0;
        bool released = false;
        while (loop.running()) {
            const auto elapsed = loop.elapsed_ms();
            if (!released && elapsed >= 1000) {
                client.touch(sample::Touch::Up, end_x, y);
                released = true;
            } else if (!released && elapsed >= next_move_ms) {
                // Same finger moves for 1 second; this is touch, not mouse drag.
                const int x = start_x + int((end_x - start_x) * elapsed / 1000);
                client.touch(sample::Touch::Move, x, y);
                next_move_ms = elapsed + 20;
            }
            client.poll(loop.poll_timeout());
        }
    });
}
