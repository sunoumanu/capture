// ms2109_pipeline.hpp
// Thread-safe concurrency primitives for capture -> display / capture -> disk
// communication.

#ifndef MS2109_PIPELINE_HPP
#define MS2109_PIPELINE_HPP

#include "ms2109_common.hpp"

#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>

// ---- Single-slot frame mailbox ------------------------------------------
// Thread-safe drop-old-on-overwrite slot for the freshest captured frame.
// Capture thread .post()s; consumer .tryTake()s the latest, never blocking
// the producer.
struct FrameMailbox {
    std::mutex     m;
    cv::Mat        frame;
    uint64_t       serial = 0;

    void post(const cv::Mat& f) {
        std::lock_guard<std::mutex> lk(m);
        f.copyTo(frame);   // Mat::copyTo is the cheap path here; reuses storage.
        ++serial;
    }
    // Returns true if 'lastSeen' was advanced and 'out' was filled.
    bool tryTake(uint64_t& lastSeen, cv::Mat& out) {
        std::lock_guard<std::mutex> lk(m);
        if (serial == lastSeen || frame.empty()) return false;
        frame.copyTo(out);
        lastSeen = serial;
        return true;
    }
};

// ---- Bounded recording queue --------------------------------------------
// Recording must NOT drop frames, so it gets a real queue. If disk falls
// behind, capture will eventually block — that is the correct behavior for
// recording (better to slow down than silently lose frames).
struct RecordQueue {
    std::mutex                  m;
    std::condition_variable     cv;
    std::deque<cv::Mat>         q;
    bool                        done = false;
    size_t                      maxSize = 16;

    void push(const cv::Mat& f) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&]{ return q.size() < maxSize || done; });
        if (done) return;
        q.push_back(f.clone());
        cv.notify_one();
    }
    bool pop(cv::Mat& out) {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&]{ return !q.empty() || done; });
        if (q.empty()) return false;
        out = std::move(q.front());
        q.pop_front();
        cv.notify_one();
        return true;
    }
    void close() {
        { std::lock_guard<std::mutex> lk(m); done = true; }
        cv.notify_all();
    }
};

#endif // MS2109_PIPELINE_HPP
