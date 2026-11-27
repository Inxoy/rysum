#pragma once

#include "QrCode.h"
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <juce_graphics/juce_graphics.h>
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <vector>

class RysumProcessor;

namespace rysum
{
/** "Publikum": the audience's phones as hands. The plugin serves a small page on the local network
    (a QR code in the 3D view leads there); every phone that opens it gets one pipe of its own, in
    a colour of its own. Holding the pipe's picture plays it like a held key (for as long as the finger
    stays; a phone that goes quiet lets it go after 0.7 s), tapping beside it knocks the pipe, a swipe
    sends it off with a shockwave from where the player stands, holding and dragging blows it like wind
    (up on the phone: the way the 3D view looks) - every phone moves only its own pipe. Only pipes that
    are bodies in the Saal (Schweben, Flug, Fall, Schwarm, Stroemung) take part. Off by default and
    never saved: a project that opens does not open a port.

    The server is one thread that answers plain HTTP (no files from disk, no outside addresses, a few
    short paths; the page's fonts and photographs come from the plugin's own data); it never touches the engine. What the phones do is queued and played on the message
    thread, a few at a time. */
class AudienceServer : private juce::Thread
{
public:
    AudienceServer();
    ~AudienceServer() override;
    /** opens the first free port from 'firstPort' on (loopbackOnly: 127.0.0.1, for the tests) */
    bool start(int firstPort = 8787, bool loopbackOnly = false);
    void stop();
    bool running() const noexcept
    {
        return port > 0;
    }
    int portNumber() const noexcept
    {
        return port;
    }

    struct Event
    {
        int phone = -1;
        char act = 0; // 'k' knock, 's' shockwave, 'w' wind (x, y: -1..1, y up on the phone),
                      // 'n' the pipe held down (sent again while held), 'o' let go
        float amount = 1.0f, x = 0.0f, y = 0.0f;
    };
    /** message thread: what the phones did since the last call */
    std::vector<Event> takeEvents();
    /** message thread: what a phone is shown (its pipe and colour); -1 colour = no pipe yet */
    void setShown(int phone, const juce::String &label, int colour, int stop = -1);
    /** the phones heard from within the last few seconds (index; when last seen, ms) */
    std::vector<std::pair<int, double>> phonesSeen(double withinMs = 6000.0) const;
    int knocksServed() const noexcept
    {
        return served.load();
    }

    static constexpr int kMaxPhones = 48;
    static const char *page();

private:
    void run() override;
    juce::MemoryBlock answer(const juce::String &request);
    std::unique_ptr<juce::StreamingSocket> listener;
    std::atomic<int> port{0};
    std::atomic<int> served{0};
    mutable juce::CriticalSection lock;
    std::deque<Event> events;
    std::map<juce::String, int> ids; // the phone's own random id -> index
    struct Phone
    {
        double seen = 0.0;
        juce::String label;
        int colour = -1;
        int stop = -1; // which stop (the page shows its knob)
    };
    std::array<Phone, kMaxPhones> phones{};
    int nextPhone = 0;
};

/** the processor's side: owns the server, gives the phones their pipes and plays what they do */
class AudienceHands : private juce::Timer
{
public:
    explicit AudienceHands(RysumProcessor &p);
    ~AudienceHands() override;
    bool start(bool loopbackOnly = false);
    void stop();
    bool running() const noexcept
    {
        return server.running();
    }
    /** "http://192.168.1.20:8787/" (the address the phones use) */
    juce::String url() const
    {
        return address;
    }
    /** the other addresses of this computer (if the first is the wrong network) */
    juce::StringArray otherAddresses() const
    {
        return others;
    }
    const QrCode &qr() const noexcept
    {
        return code;
    }
    struct Hand
    {
        int phone = -1, stop = -1, key = -1, colour = 0;
        double lastAct = 0.0; // ms: when it last knocked (the view flashes it)
        bool holding = false; // its pipe sounds, held down on the phone
        double holdSeen = 0.0; // ms: when the phone last said it still holds it
    };
    /** how many phones hold their pipe down now */
    int holdingCount() const noexcept;
    static constexpr double kHoldTimeoutMs = 700.0;
    /** the phones there are now, with their pipes */
    const std::vector<Hand> &hands() const noexcept
    {
        return now;
    }
    double lastShockMs() const noexcept
    {
        return lastShock;
    }
    int phoneCount() const noexcept
    {
        return (int)now.size();
    }
    static juce::Colour colour(int index);
    /** tests: one tick of the timer at once */
    void tickForTest()
    {
        timerCallback();
    }
    AudienceServer &serverForTest() noexcept
    {
        return server;
    }

private:
    void timerCallback() override;
    void assign();
    void press(Hand &h, double t);
    void letGo(Hand &h);
    RysumProcessor &proc;
    AudienceServer server;
    juce::String address;
    juce::StringArray others;
    QrCode code;
    std::vector<Hand> now;
    std::array<Hand, AudienceServer::kMaxPhones> byPhone{};
    std::array<double, AudienceServer::kMaxPhones> lastShockOf{};
    double lastShock = 0.0, lastTick = 0.0;
};
} // namespace rysum
