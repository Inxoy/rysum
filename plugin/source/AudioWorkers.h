#pragma once

#include <atomic>
#include <juce_core/juce_core.h>
#include <memory>
#include <vector>
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace rysum
{
/** A few threads that help the audio thread with work that splits into independent jobs (the early
 *  reflections of the Saal: every group of sounds into its own bus).
 *
 *  One pool for the whole process (all instances of the plugin share it, see
 *  juce::SharedResourcePointer): while one instance uses it, another does its jobs alone instead of
 *  waiting. The audio thread never takes a lock: it publishes the jobs, wakes the helpers that
 *  sleep, takes jobs itself and waits (spinning) only for jobs a helper has already begun. Which
 *  thread does which job is open - the jobs must not depend on it. */
class AudioWorkers
{
public:
    using Fn = void (*)(void *context, int job);

    AudioWorkers()
    {
        // (up to four threads can be asked for; helpers that are not used sleep)
        const int most = juce::jmax(wantedThreads(), juce::jmin(4, juce::SystemStats::getNumCpus()));
        const int n = juce::jlimit(0, kMaxHelpers, most - 1);
        for (int i = 0; i < n; ++i)
        {
            auto w = std::make_unique<Helper>(*this, i);
            // as urgent as the audio thread itself (else the picture's threads would hold it up)
            if (!w->startRealtimeThread(juce::Thread::RealtimeOptions().withPriority(9)))
                w->startThread(juce::Thread::Priority::highest);
            helpers.push_back(std::move(w));
        }
    }

    ~AudioWorkers()
    {
        for (auto &w : helpers)
            w->signalThreadShouldExit();
        for (auto &w : helpers)
            w->wake.signal();
        for (auto &w : helpers)
            w->stopThread(2000);
    }

    /** how many threads can work at once (the caller and the helpers) */
    int threads() const noexcept
    {
        return (int)helpers.size() + 1;
    }

    /** How many threads the audio may use when nothing else is said: half the processor's cores,
     *  at most four (the host, other plugins and the picture need the rest). The environment
     *  variable RYSUM_AUDIO_THREADS overrides it (1 = everything on the audio thread). */
    static int wantedThreads() noexcept
    {
        // (worked out once, when the first engine is made: asking the environment allocates, and
        // the audio thread asks for this number in every slice)
        static const int wanted = []
        {
            const auto forced =
                juce::SystemStats::getEnvironmentVariable("RYSUM_AUDIO_THREADS", {});
            if (forced.isNotEmpty())
                return juce::jlimit(1, kMaxHelpers + 1, forced.getIntValue());
            return juce::jlimit(1, 4, juce::SystemStats::getNumPhysicalCpus() / 2);
        }();
        return wanted;
    }

    /** Calls fn (context, 0 ... jobs - 1), every job once, and returns when all are done.
     *  maxThreads: how many threads may take part (1 = only the caller). */
    void run(int jobs, Fn fn, void *context, int maxThreads) noexcept
    {
        const int allow = juce::jmin(maxThreads, threads()) - 1;
        if (jobs < 2 || allow < 1 || busy.exchange(true))
        {
            for (int i = 0; i < jobs; ++i)
                fn(context, i);
            return;
        }
        // (no helper is in the middle of anything: the last run waited for them)
        jobFn = fn;
        jobContext = context;
        jobCount = jobs;
        allowed = allow;
        next = 0;
        done = 0;
        open = true;
        ++generation;
        for (int i = 0; i < allow; ++i)
            if (helpers[(size_t)i]->asleep.load())
                helpers[(size_t)i]->wake.signal();
        for (int i = next.fetch_add(1); i < jobs; i = next.fetch_add(1))
        {
            fn(context, i);
            done.fetch_add(1);
        }
        open = false;
        // jobs a helper has begun, and helpers that are just looking: wait for them
        for (int spins = 0; done.load() < jobs || active.load() > 0; ++spins)
        {
            if (spins < 4000)
                relax();
            else
                juce::Thread::yield();
        }
        busy = false;
    }

private:
    static constexpr int kMaxHelpers = 7;

    static void relax() noexcept
    {
#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield");
#endif
    }

    struct Helper : juce::Thread
    {
        Helper(AudioWorkers &o, int i)
            : juce::Thread("Rysum audio " + juce::String(i + 2)), owner(o), index(i)
        {
        }
        void run() override
        {
            owner.help(*this);
        }
        AudioWorkers &owner;
        const int index;
        juce::WaitableEvent wake;
        std::atomic<bool> asleep{false};
    };

    void help(Helper &me) noexcept
    {
        uint32_t seen = generation.load();
        while (!me.threadShouldExit())
        {
            // the next run: watched for a moment (the slices of one block follow each other
            // closely), then slept for
            for (int spins = 0; generation.load() == seen;)
            {
                if (me.threadShouldExit())
                    return;
                if (++spins < kSpins)
                {
                    relax();
                    continue;
                }
                me.asleep = true;
                if (generation.load() == seen)
                    me.wake.wait(100);
                me.asleep = false;
                spins = 0;
            }
            seen = generation.load();
            active.fetch_add(1);
            if (open.load() && me.index < allowed.load())
            {
                juce::ScopedNoDenormals noDenormals;
                const int count = jobCount.load();
                for (int i = next.fetch_add(1); i < count; i = next.fetch_add(1))
                {
                    jobFn.load()(jobContext.load(), i);
                    done.fetch_add(1);
                }
            }
            active.fetch_sub(1);
        }
    }

    static constexpr int kSpins = 2000; // (about 30 - 60 microseconds)
    std::vector<std::unique_ptr<Helper>> helpers;
    std::atomic<bool> busy{false}, open{false};
    std::atomic<uint32_t> generation{0};
    std::atomic<int> next{0}, done{0}, active{0}, allowed{0}, jobCount{0};
    std::atomic<Fn> jobFn{nullptr};
    std::atomic<void *> jobContext{nullptr};
};
} // namespace rysum
