#include "../common/sample_client.h"
#include "../common/media_files.h"

int main() {
    return sample::Run([] {
        const auto host = sample::find_host(L"", L"", 28252);
        sample::Connection client;
        client.connect(host);

        // Receive the first decoded BGRA frame and save it as a new BMP file.
        const auto first_frame = client.wait_frame();
        sample::SaveBmp(L"frame.bmp", first_frame);

        // Continue receiving image events for 10 seconds.
        sample::ConnectionLoop loop(10000);
        while (loop.running()) {
            const auto event = client.poll(loop.poll_timeout());
            if (event.type == SD_EVENT_FRAME) {
                const auto frame = client.frame();
                std::cout << "Frame " << frame.sequence << ": "
                          << frame.width << 'x' << frame.height << '\n';
                // Use frame.pixels here; copy them if needed after the next poll.
            }
        }
    });
}
