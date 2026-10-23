#include "media.h"

#include "log.h"
#include "settings.h"

#include <unknwn.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <mutex>
#include <thread>

namespace ep {

double MediaSnapshot::PositionNow() const {
    double pos = position;
    if (playing) pos += std::chrono::duration<double>(std::chrono::steady_clock::now() - sampledAt).count();
    if (duration > 0) pos = std::min(pos, duration);
    return std::max(pos, 0.0);
}

namespace {

using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Storage::Streams;
using Session = GlobalSystemMediaTransportControlsSession;
using Status = GlobalSystemMediaTransportControlsSessionPlaybackStatus;

std::mutex g_mutex;
MediaSnapshot g_snap;
std::thread g_thread;
std::atomic<bool> g_stop{false};
std::atomic<int> g_pendingCommand{static_cast<int>(MediaCommand::None)};

std::string Utf8(winrt::hstring const& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

bool AppMatches(std::string app, const std::string& filter) {
    if (filter.empty()) return true;
    std::transform(app.begin(), app.end(), app.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return app.find(filter) != std::string::npos;
}

// Among the sessions of the wanted app (settings: app_filter, default Spotify) a playing one wins, then the
// first one even if paused. Without a filter any playing session wins, then the system's current one.
Session PickSession(GlobalSystemMediaTransportControlsSessionManager const& manager) {
    const std::string& filter = GetSettings().appFilter;
    Session playing{nullptr};
    Session any{nullptr};
    for (auto const& s : manager.GetSessions()) {
        if (!AppMatches(Utf8(s.SourceAppUserModelId()), filter)) continue;
        if (!any) any = s;
        if (!playing && s.GetPlaybackInfo().PlaybackStatus() == Status::Playing) playing = s;
    }
    if (playing) return playing;
    if (any) return any;
    if (filter.empty()) return manager.GetCurrentSession();
    return Session{nullptr};
}

std::shared_ptr<const std::vector<uint8_t>> ReadThumbnail(GlobalSystemMediaTransportControlsSessionMediaProperties const& props) {
    auto thumb = props.Thumbnail();
    if (!thumb) return nullptr;
    auto stream = thumb.OpenReadAsync().get();
    uint32_t size = static_cast<uint32_t>(stream.Size());
    if (size == 0 || size > 16u * 1024 * 1024) return nullptr;
    DataReader reader(stream);
    reader.LoadAsync(size).get();
    auto bytes = std::make_shared<std::vector<uint8_t>>(size);
    reader.ReadBytes(*bytes);
    return bytes;
}

void Run() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        auto manager = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
        Logf("MEDIA SMTC manager ready");

        std::string trackKey;
        int coverTries = 0;
        int tick = 0;
        while (!g_stop) {
            try {
                Session session = PickSession(manager);
                auto command = static_cast<MediaCommand>(g_pendingCommand.exchange(static_cast<int>(MediaCommand::None)));
                if (session && command != MediaCommand::None) {
                    bool ok = false;
                    if (command == MediaCommand::PlayPause) ok = session.TryTogglePlayPauseAsync().get();
                    else if (command == MediaCommand::Next) ok = session.TrySkipNextAsync().get();
                    else if (command == MediaCommand::Previous) ok = session.TrySkipPreviousAsync().get();
                    Logf("MEDIA command %d -> %s", static_cast<int>(command), ok ? "ok" : "refused by the player");
                }
                MediaSnapshot next;
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    next = g_snap;  // keeps serials and the cover between polls
                }
                if (!session) {
                    if (next.hasSession) Logf("MEDIA no active session");
                    next.hasSession = false;
                    next.playing = false;
                } else {
                    auto props = session.TryGetMediaPropertiesAsync().get();
                    auto info = session.GetPlaybackInfo();
                    auto tl = session.GetTimelineProperties();

                    next.hasSession = true;
                    next.app = Utf8(session.SourceAppUserModelId());
                    next.title = Utf8(props.Title());
                    next.artist = Utf8(props.Artist());
                    next.album = Utf8(props.AlbumTitle());
                    next.playing = info.PlaybackStatus() == Status::Playing;
                    next.duration = std::chrono::duration<double>(tl.EndTime() - tl.StartTime()).count();

                    double raw = std::chrono::duration<double>(tl.Position()).count();
                    double age = std::chrono::duration<double>(winrt::clock::now() - tl.LastUpdatedTime()).count();
                    next.position = raw + (next.playing ? std::max(age, 0.0) : 0.0);
                    next.sampledAt = std::chrono::steady_clock::now();

                    std::string key = next.app + "|" + next.title + "|" + next.artist + "|" + next.album;
                    if (key != trackKey) {
                        trackKey = key;
                        ++next.trackSerial;
                        next.cover.reset();
                        ++next.coverSerial;
                        coverTries = 0;
                        Logf("MEDIA [%s] %s: '%s' - '%s' (%s) %.0fs", next.app.c_str(), next.playing ? "playing" : "paused",
                             next.artist.c_str(), next.title.c_str(), next.album.c_str(), next.duration);
                    }
                    if (!next.cover && coverTries < 8) {
                        ++coverTries;
                        try {
                            auto cover = ReadThumbnail(props);
                            if (cover) {
                                next.cover = cover;
                                ++next.coverSerial;
                                Logf("MEDIA cover loaded: %zu bytes", cover->size());
                            }
                        } catch (winrt::hresult_error const& e) {
                            Logf("MEDIA cover failed: 0x%08x", static_cast<unsigned>(e.code().value));
                        }
                    }
                    if (tick % 60 == 0) {
                        Logf("MEDIA timeline raw=%.1fs age=%.1fs -> %.1fs of %.1fs", raw, age, next.position, next.duration);
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(g_mutex);
                    g_snap = std::move(next);
                }
            } catch (winrt::hresult_error const& e) {
                Logf("MEDIA poll failed: 0x%08x", static_cast<unsigned>(e.code().value));
            }
            ++tick;
            // Poll twice a second, but react to a button press within ~50 ms.
            for (int i = 0; i < 10 && !g_stop && g_pendingCommand.load() == static_cast<int>(MediaCommand::None); ++i) Sleep(50);
        }
    } catch (winrt::hresult_error const& e) {
        Logf("MEDIA failed to start: 0x%08x", static_cast<unsigned>(e.code().value));
    } catch (...) {
        Logf("MEDIA failed with an unknown exception");
    }
}

}  // namespace

void MediaStart() {
    g_stop = false;
    g_thread = std::thread(Run);
}

void MediaStop() {
    g_stop = true;
    if (g_thread.joinable()) g_thread.join();
}

void MediaSendCommand(MediaCommand cmd) { g_pendingCommand = static_cast<int>(cmd); }

MediaSnapshot MediaGetSnapshot() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_snap;
}

}  // namespace ep
