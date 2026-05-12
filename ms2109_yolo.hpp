// ms2109_yolo.hpp
// YOLO object-detection wrapper using OpenCV DNN.
// Supports ONNX models (YOLOv5 / YOLOv8 recommended).

#ifndef MS2109_YOLO_HPP
#define MS2109_YOLO_HPP

#include "ms2109_common.hpp"
#include <opencv2/dnn.hpp>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>

struct Detection {
    int classId = -1;
    float confidence = 0.0f;
    cv::Rect box;
    std::string className;
};

// ------------------------------------------------------------------
// YoloDetector  -- loads a model and runs inference on a single frame.
// ------------------------------------------------------------------
class YoloDetector {
public:
    YoloDetector() = default;

    bool loadModel(const std::string& modelPath,
                   const std::string& classesPath = "",
                   float confThreshold = 0.25f,
                   float nmsThreshold  = 0.45f);

    bool isLoaded() const { return !net.empty(); }

    // Returns detections for the given BGR frame.
    std::vector<Detection> detect(const cv::Mat& frame);

    // Draws detections in-place onto the frame.
    static void drawDetections(cv::Mat& frame,
                               const std::vector<Detection>& detections);

private:
    cv::dnn::Net net;
    std::vector<std::string> classNames;
    float confThreshold = 0.25f;
    float nmsThreshold  = 0.45f;
    std::vector<std::string> outLayerNames;
    cv::Size modelInputSize{640, 640};
    bool isObb = false;                 // true for *-obb.onnx models (extra angle col)
    bool classNamesFromUser = false;    // user supplied a classes file
    bool inferenceFailed = false;       // sticky once forward() has thrown

    void loadClasses(const std::string& path);
    std::vector<Detection> postProcess(const cv::Mat& frame,
                                       const std::vector<cv::Mat>& outs,
                                       float scale, int padX, int padY);
};

// ------------------------------------------------------------------
// YoloPipeline  -- threaded consumer: raw frames in, detections out.
//
// The mailbox holds *detections only*, not an annotated frame. The display
// thread always shows the latest captured frame and overlays the freshest
// detections on top — that way display FPS = capture FPS even when
// inference is slow (boxes simply lag a few frames). Pushing annotated
// Mats would tie display rate to inference rate.
// ------------------------------------------------------------------
struct YoloPipeline {
    std::mutex m;
    std::condition_variable cv;
    std::deque<cv::Mat> q;
    bool done = false;
    size_t maxSize = 4;          // bounded so capture never blocks

    YoloDetector detector;

    struct Mailbox {
        std::mutex                 m;
        std::vector<Detection>     detections;
        uint64_t                   serial = 0;
    } outMailbox;

    std::thread thread;
    std::atomic<uint64_t> processedCount{0};

    void start();
    void stop();
    bool push(const cv::Mat& f);                                       // false = dropped (queue full)
    bool tryTakeDetections(uint64_t& lastSeen, std::vector<Detection>& out);
};

#endif // MS2109_YOLO_HPP
