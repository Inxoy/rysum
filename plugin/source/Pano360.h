#pragma once

#include "Walk3D.h"
#include "PanoGpu.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>

/** The 360° recording of the 3D view: a video all around the eye (equirectangular) and the sound field as
    first-order Ambisonics (AmbiX: ACN / SN3D, W Y Z X), both fixed to the room - the front of the picture is
    the Saal's front (azimuth 0), as the scene is before the head's turn. Together they make a 360° video with
    spatial audio for YouTube: when the frames are drawn, ffmpeg (if it is installed) makes rysum360.mov of the
    frames and the WAV, and SpatialMeta writes into it the boxes that mark it as such (those Google's Spatial
    Media Metadata Injector writes). Without ffmpeg the folder has a script for it instead (see its README).

    Recording and drawing are two steps:
    - while recording, the audio thread hands every block's first four Ambisonics channels to the recorder
      (written to ambix.wav), and for every video frame that falls due the 3D view hands over the frame's
      scene - its triangles in the room, materials, lamps, glows (PanoJob) - which is written to scene.bin
      (the room's or model's triangles only when they change). That costs little, so every frame is kept
      however large the picture is to become.
    - after the recording, a thread of the recorder draws every frame from scene.bin: six cameras (a cube
      around the eye, 92° each), resampled into the equirectangular picture, the glows of the sounding pipes
      laid in, written as frames/f_000000.jpg ... by a few threads. The graphics card draws the faces when it
      can (PanoGpu, its own context on this thread: about 20 ms for a 3840-wide frame); otherwise, or when it
      fails on the way, the processor (a few tenths of a second a frame).
    The audio is the clock: video frame k shows the moment k / fps after the start; a frame for which the view
    gave no scene (it drew more slowly than the frame rate) repeats the one before. */
namespace rysum
{
/** what one 360° frame needs: the triangles, the materials, the light and the glows, all in the room */
struct PanoJob
{
    std::vector<Raster::Call> calls;
    size_t staticCount = 0; // the first ones: the room / the model (stored only when they change)
    std::array<Material, 32> mats{};
    std::vector<Raster::Lamp> lamps;
    float ambient = 1.0f;
    V3 eye;
    float fogTone = 0.07f, fogDistance = 20.0f, grain = 0.12f;
    uint32_t frameNo = 0;
    struct Glow
    {
        V3 at;
        float amount, size;
    };
    std::vector<Glow> glows;
    int64_t upTo = 0; // the video frame this scene is for (and the ones before it that have none)
};

/** draws one frame: the equirectangular picture, 'width' wide (face: a renderer to draw the cube's faces with).
    gpu: the graphics card fills the faces and resamples them (if it fails, the processor draws, the card is
    closed and why is in gpuError) */
juce::Image renderPanorama(const PanoJob &job, int width, Raster &face, PanoGpu *gpu = nullptr,
                           juce::String *gpuError = nullptr);

class Pano360Recorder
{
public:
    Pano360Recorder();
    ~Pano360Recorder();

    struct Settings
    {
        int width = 3840; // equirectangular, height = width / 2
        int fps = 30;
        int jpegQuality = 92;
    };
    enum class State
    {
        idle,
        recording,
        drawing
    };
    // ---- message thread
    /** a new recording in 'folder' (created; must not exist yet); only when idle */
    bool start(const juce::File &folder, Settings s, double sampleRate, juce::String &error);
    /** ends the recording; the frames are then drawn (state 'drawing') */
    void stop();
    /** ends the drawing too (the frames drawn so far stay; the README says how far it got) */
    void abort();
    State state() const noexcept { return st.load(); }
    bool isRecording() const noexcept { return armed.load(std::memory_order_acquire); }
    const Settings &settings() const noexcept { return set; }
    juce::File folder() const { return dir; }
    /** true when the next video frame wants a scene (the audio has reached it, it has none yet) */
    bool wantsScene() const noexcept;
    /** the video frame the audio has reached */
    int64_t frameNow() const noexcept;
    /** hands over the scene for the frames up to frameNow() */
    void addScene(PanoJob &&job);
    double secondsRecorded() const noexcept { return (double)samples.load() / sr; }
    /** scenes kept, video frames due by the end */
    int64_t scenesKept() const noexcept { return scenes.load(); }
    int64_t framesTotal() const noexcept { return total.load(); }
    /** drawing: frames written so far */
    int64_t framesDrawn() const noexcept { return drawn.load(); }
    float lastFrameMs() const noexcept { return frameMs.load(); }
    /** true while the last block carried no Ambisonics (a stereo output: nothing of the space is recorded) */
    bool noScene() const noexcept { return lastNoScene.load(); }
    /** counts finished recordings (all frames drawn) */
    int finishedCount() const noexcept { return finished.load(); }
    int64_t samplesLost() const noexcept { return lost.load(); }
    /** drawing: the graphics card that draws the frames (empty: the processor), and why not the card */
    juce::String drawingOn() const
    {
        const std::lock_guard<std::mutex> l(infoLock);
        return drawnOn;
    }
    juce::String whyNotOnCard() const
    {
        const std::lock_guard<std::mutex> l(infoLock);
        return gpuWhy;
    }
    /** after the frames: 0 = no video being made, 1 = ffmpeg makes it, 2 = the 360 marks are written,
        3 = the last recording's video is done */
    int videoStepNow() const noexcept { return videoStep.load(); }
    int64_t videoFramesDone() const noexcept { return videoFrames.load(); }
    /** what went wrong with the last video (empty: nothing, or no ffmpeg) */
    juce::String videoProblem() const
    {
        const std::lock_guard<std::mutex> l(infoLock);
        return videoNote;
    }

    /** ffmpeg: RYSUM_FFMPEG, the PATH, then the usual places (winget, Homebrew); none = File() */
    static juce::File findFfmpeg();
    /** ffmpeg's arguments (after its own name) for the video from the frames and the sound */
    static juce::StringArray ffmpegArgs(const Settings &s, const juce::File &frames, const juce::File &wav,
                                        const juce::File &out);

    // ---- audio thread: the block's Ambisonics (numCh < 4: the missing channels silent)
    void pushAudio(const float *const *ch, int numCh, int n) noexcept;

    /** the text of the script that makes the video (also written into the folder) */
    static juce::String videoScript(const Settings &s);

private:
    class Writer;
    class Drawer;
    std::unique_ptr<Writer> writer;
    std::unique_ptr<Drawer> drawer;
    Settings set;
    juce::File dir;
    double sr = 48000.0;
    std::atomic<State> st{State::idle};
    std::atomic<bool> armed{false};
    std::atomic<int64_t> samples{0}, given{0}, scenes{0}, lost{0}, total{0}, drawn{0};
    std::atomic<float> frameMs{0.0f};
    std::atomic<bool> lastNoScene{false};
    std::atomic<int> finished{0};
    std::atomic<int> videoStep{0};
    std::atomic<int64_t> videoFrames{0};
    mutable std::mutex infoLock;
    juce::String drawnOn, gpuWhy, videoNote; // (under infoLock)
    // audio -> writer
    static constexpr int kFifo = 1 << 18;
    juce::AbstractFifo fifo{kFifo};
    std::array<std::vector<float>, 4> ring;
    void finishDrawing(bool complete); // (the drawer's thread: scripts, README)
    void joinThreads();
    friend class Writer;
    friend class Drawer;
};
} // namespace rysum
