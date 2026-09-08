#include "../common/sample_client.h"
#include "../common/media_files.h"
#include <optional>

int main() {
    return sample::Run([] {
        const auto host = sample::find_host(L"", L"", 28252);
        sample::Connection client;
        client.connect(host);
        client.wait_frame();

        // Receive the host's audio. Change false to true for local playback too.
        client.start_audio(false);
        std::optional<sample::WaveFile> output;

        sample::ConnectionLoop loop(10000); // Record for 10 seconds of wall time.
        while (loop.running()) {
            client.poll(0); // Also process the display connection.
            sd_audio_buffer audio{};
            if (client.receive_audio(audio, loop.poll_timeout())) {
                if (audio.sample_rate != 48000 || audio.channels != 2 ||
                    audio.sample_format != SD_AUDIO_S16_LE)
                    throw std::runtime_error("Expected 48000 Hz stereo PCM16");
                if (!output) output.emplace(L"audio.wav");
                output->Write(audio.samples, audio.frame_count);
                // audio.samples belongs to the SDK; consume it before the next audio poll.
            }
        }

        if (!output) throw std::runtime_error("No PCM audio received during the 10-second loop");
        output->Finish();
        // Destruction stops audio, disconnects the display and frees the SDK handle.
    });
}
