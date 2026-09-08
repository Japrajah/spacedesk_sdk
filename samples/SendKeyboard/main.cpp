#include "../common/sample_client.h"

int main() {
    return sample::Run([] {
        // Example: find_host(L"192.168.1.5", L"", 28252) or
        //          find_host(L"", L"MY-PC", 28252). Empty values enable selection.
        const auto host = sample::find_host(L"", L"", 28252);
        sample::Connection client;
        client.connect(host);
        client.wait_frame();

        // Send one key press: Windows VK_A, with both DOWN and UP.
        client.key_press('A');

        // Text alternative (goes to the currently focused field on the host):
        // client.text(L"Hello from the SDK!");

        sample::ConnectionLoop loop(10000); // 10 seconds of connection events.
        while (loop.running()) {
            client.poll(loop.poll_timeout());
        }
    });
}
