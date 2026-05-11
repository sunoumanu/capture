// ms2109_main.cpp
// Main entry point and orchestration logic for MS2109 capture.
//
// Pipeline architecture:
//
//   [USB driver] -> [capture thread: cap.read()] --(mailbox)-->
//   [main thread: imshow + key handling] --(parallel)--> [writer thread]
//
// The capture thread runs as fast as the device delivers frames and never
// waits on display or disk I/O. A single-slot mailbox holds the newest
// frame; if display is slower than capture, older preview frames get
// silently dropped (the recording still gets every frame because it has
// its own queue). This guarantees the camera is always the bottleneck,
// not the program.
//
// Build (Windows, MSVC, vcpkg):
//     cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
//     cmake --build build --config Release
//
// Build (Linux):
//     cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
//     cmake --build build -j
//
// Usage:
//     ms2109_capture                       -> opens device 0, 1080p30
//     ms2109_capture --list                -> lists every video capture device by name
//     ms2109_capture 1                     -> opens device index 1
//     ms2109_capture "USB Video"           -> opens first device whose name matches
//     ms2109_capture 1 720p60              -> 1280x720 @ 60 fps from device 1
//     ms2109_capture "USB Video" 1080p30 out.avi -> record to out.avi
//
// Flags (any position after the device arg):
//     --no-display       : don't show a preview window (max headless throughput)
//     --display-every N  : only blit every Nth frame to the window (default 1)
//     --display-scale F  : downscale preview by factor F (e.g. 0.5)
//     --bench S          : exit after S seconds, print average capture FPS
//
// YOLO flags (requires an ONNX model and optional class-names file):
//     --yolo-model PATH        : path to ONNX model (YOLOv5 / v8)
//     --yolo-classes PATH      : optional newline-separated class list
//     --yolo-conf F            : confidence threshold (default 0.25, OBB 0.50)
//     --yolo-nms  F            : NMS IoU threshold (default 0.45)
//
// NOTE: a YOLO model only produces useful detections on input that matches
// what it was trained on. The Ultralytics *-obb checkpoints are trained on
// DOTA (top-down aerial imagery) and will spew junk "ship" / "plane" boxes
// on ordinary video. For HDMI capture / general video use a COCO-trained
// model such as yolov8n.onnx.
//
// Press 'q' or ESC to quit. Press 's' to snapshot a still PNG.

#include "ms2109_common.hpp"
#include "ms2109_device.hpp"
#include "ms2109_pipeline.hpp"
#include "ms2109_yolo.hpp"

#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <memory>

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <timeapi.h>
  #pragma comment(lib, "winmm.lib")
#endif

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--list") {
        auto names = listVideoDevices();
        if (names.empty()) std::cout << "(no video capture devices found)\n";
        else for (size_t i = 0; i < names.size(); ++i)
            std::cout << "  [" << i << "] " << names[i] << "\n";
        return 0;
    }

    // Parse flags first, collect positional args from leftovers.
    bool   showDisplay  = true;
    int    displayEvery = 1;
    double displayScale = 1.0;
    double benchSeconds = 0.0;
    std::string yoloModelPath;
    std::string yoloClassesPath;
    float yoloConf = -1.0f;   // -1 = "use detector default"
    float yoloNms  = -1.0f;
    std::vector<std::string> positional;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-display") {
            showDisplay = false;
        } else if (a == "--display-every" && i + 1 < argc) {
            int v = std::atoi(argv[++i]);
            displayEvery = v > 1 ? v : 1;
        } else if (a == "--display-scale" && i + 1 < argc) {
            displayScale = std::atof(argv[++i]);
        } else if (a == "--bench" && i + 1 < argc) {
            benchSeconds = std::atof(argv[++i]);
        } else if (a == "--yolo-model" && i + 1 < argc) {
            yoloModelPath = argv[++i];
        } else if (a == "--yolo-classes" && i + 1 < argc) {
            yoloClassesPath = argv[++i];
        } else if (a == "--yolo-conf" && i + 1 < argc) {
            yoloConf = static_cast<float>(std::atof(argv[++i]));
        } else if (a == "--yolo-nms" && i + 1 < argc) {
            yoloNms = static_cast<float>(std::atof(argv[++i]));
        } else if (a[0] == '-') {
            std::cerr << "Unknown flag: " << a << "\n";
        } else {
            positional.push_back(a);
        }
    }

    // Positional: <device> [mode] [outfile]
    std::string devArg  = !positional.empty() ? positional[0] : "0";
    Mode mode           = (positional.size() > 1) ? parseMode(positional[1]) : Mode{1920, 1080, 30};
    std::string outFile = (positional.size() > 2) ? positional[2] : "";

#ifdef _WIN32
    // Boost timer resolution to 1ms. Without this, std::this_thread::sleep_for
    // and the OS scheduler quantum default to ~15.6ms, which can throttle the
    // capture loop. timeBeginPeriod is process-wide and must be paired with
    // timeEndPeriod at exit.
    timeBeginPeriod(1);
    // Lift process priority so capture isn't preempted by background tasks.
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif

    auto names = listVideoDevices();
    int deviceIndex = resolveDevice(devArg, names);
    if (deviceIndex < 0) {
        std::cerr << "No device matched \"" << devArg << "\". Available:\n";
        for (size_t i = 0; i < names.size(); ++i)
            std::cerr << "  [" << i << "] " << names[i] << "\n";
        return 1;
    }
    if (deviceIndex < (int)names.size())
        std::cout << "Selected [" << deviceIndex << "] " << names[deviceIndex] << "\n";

#ifdef _WIN32
    int backend = cv::CAP_DSHOW;
#else
    int backend = cv::CAP_V4L2;
#endif

    cv::VideoCapture cap;
    std::vector<int> params = {
        cv::CAP_PROP_FOURCC,       cv::VideoWriter::fourcc('M', 'J', 'P', 'G'),
        cv::CAP_PROP_FRAME_WIDTH,  mode.width,
        cv::CAP_PROP_FRAME_HEIGHT, mode.height,
        cv::CAP_PROP_FPS,          mode.fps,
    };
    if (!cap.open(deviceIndex, backend, params)) {
        std::cerr << "Failed to open capture device " << deviceIndex << "\n";
        return 1;
    }

    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    // Best-effort: turn off auto-exposure / autofocus / auto-WB. Most have
    // no effect on a digital capture chip but the few that do can throttle
    // delivery, and the call is harmless on backends that ignore them.
    cap.set(cv::CAP_PROP_AUTO_EXPOSURE, 0.25); // 0.25 = manual on many DSHOW drivers
    cap.set(cv::CAP_PROP_AUTOFOCUS,     0);
    cap.set(cv::CAP_PROP_AUTO_WB,       0);
    cap.set(cv::CAP_PROP_CONVERT_RGB,   1);   // ensure backend hands us BGR; faster path on most builds

    // Tell OpenCV's internal threadpool it can use all cores for MJPEG decode.
    cv::setUseOptimized(true);
    cv::setNumThreads(std::thread::hardware_concurrency());

    int  w   = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int  h   = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double fps = cap.get(cv::CAP_PROP_FPS);
    int   negotiatedFourcc = (int)cap.get(cv::CAP_PROP_FOURCC);

    std::string fcc = fourccToString(negotiatedFourcc);
    std::cout << "Opened at " << w << "x" << h << " @ " << fps
              << " fps, format=" << fcc << "\n";

    if (fcc != "MJPG" && fcc != "mjpg") {
        std::cerr << "\n!!! WARNING: device is NOT delivering MJPEG (got '" << fcc
                  << "'). Expect throttled FPS at high resolutions.\n\n";
    }

    // Recording: writer runs on its own thread fed by RecordQueue.
    cv::VideoWriter   writer;
    RecordQueue       recQ;
    std::thread       writerThread;
    if (!outFile.empty()) {
        int fourcc = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
        double recFps = (fps > 1.0 && fps < 200.0) ? fps : mode.fps;
        if (!writer.open(outFile, fourcc, recFps, cv::Size(w, h), true)) {
            std::cerr << "Failed to open output file: " << outFile << "\n";
            return 2;
        }
        std::cout << "Recording to " << outFile << "\n";
        writerThread = std::thread([&]{
            cv::Mat f;
            while (recQ.pop(f)) {
                writer.write(f);
            }
        });
    }

    if (showDisplay) {
        cv::namedWindow("MS2109 capture", cv::WINDOW_NORMAL);
        cv::resizeWindow("MS2109 capture", 1280, 720);
    }

    // ---- Optional YOLO object-detection thread ---------------------------
    YoloPipeline yoloPipeline;
    bool yoloEnabled = !yoloModelPath.empty();
    if (yoloEnabled) {
        // -1 means "let the detector pick its own default". The detector
        // raises the conf floor for OBB models because they're much noisier
        // on out-of-domain inputs.
        float useConf = (yoloConf >= 0.0f) ? yoloConf : 0.25f;
        float useNms  = (yoloNms  >= 0.0f) ? yoloNms  : 0.45f;
        if (!yoloPipeline.detector.loadModel(yoloModelPath, yoloClassesPath,
                                             useConf, useNms)) {
            std::cerr << "Failed to load YOLO model: " << yoloModelPath << "\n";
            return 3;
        }
        yoloPipeline.start();
        std::cout << "YOLO enabled (model: " << yoloModelPath
                  << ", conf=" << useConf << ", nms=" << useNms << ")\n";
    }

    // ---- Capture thread --------------------------------------------------
    // Reads frames as fast as the device delivers, posts to mailbox, and
    // pushes to record queue. Never blocks on display.
    FrameMailbox          mailbox;
    std::atomic<bool>     running{true};
    std::atomic<uint64_t> capCount{0};
    std::atomic<double>   lastCapFps{0.0};

    std::thread captureThread([&]{
        cv::Mat frame;
        int    local = 0;
        auto   t0 = clk::now();
        while (running.load(std::memory_order_relaxed)) {
            if (!cap.read(frame) || frame.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            mailbox.post(frame);
            if (yoloEnabled) {
                yoloPipeline.push(frame);  // non-blocking; drops if queue full
            }
            if (writer.isOpened()) recQ.push(frame);
            capCount.fetch_add(1, std::memory_order_relaxed);

            ++local;
            auto now = clk::now();
            double elapsed = std::chrono::duration<double>(now - t0).count();
            if (elapsed >= 1.0) {
                lastCapFps.store(local / elapsed, std::memory_order_relaxed);
                std::cout << "  capture " << (local / elapsed) << " fps\n";
                t0 = now;
                local = 0;
            }
        }
    });

    // ---- Main thread: display + key handling -----------------------------
    cv::Mat   displayed;
    uint64_t  lastSeen     = 0;
    uint64_t  yoloLastSeen = 0;
    int       displayCount = 0;
    int       snapshotIdx  = 0;
    auto      benchStart   = clk::now();
    cv::Mat   scaled;       // reused buffer for downscaled preview

    while (running.load(std::memory_order_relaxed)) {
        bool fresh = false;
        if (yoloEnabled) {
            fresh = yoloPipeline.tryTake(yoloLastSeen, displayed);
        } else {
            fresh = mailbox.tryTake(lastSeen, displayed);
        }

        if (fresh && showDisplay && (displayCount++ % displayEvery == 0)) {
            if (displayScale > 0.0 && displayScale < 1.0) {
                cv::resize(displayed, scaled, cv::Size(), displayScale, displayScale,
                           cv::INTER_NEAREST); // INTER_NEAREST is fastest; quality irrelevant for preview
                cv::imshow("MS2109 capture", scaled);
            } else {
                cv::imshow("MS2109 capture", displayed);
            }
        }

        // pollKey() returns immediately (no sleep), unlike waitKey(1) which
        // can block for ~16ms on Windows. We still need to pump the GUI
        // event loop for the window to be responsive, so call it every iter.
        int key = -1;
        if (showDisplay) {
            key = cv::pollKey() & 0xFF;
        } else if (!fresh) {
            // No display, no fresh frame: yield briefly to let capture run.
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
        if (key == 'q' || key == 27) running = false;
        if (key == 's' && !displayed.empty()) {
            std::string name = "snapshot_" + std::to_string(snapshotIdx++) + ".png";
            cv::imwrite(name, displayed);
            std::cout << "Saved " << name << "\n";
        }

        if (benchSeconds > 0.0 &&
            std::chrono::duration<double>(clk::now() - benchStart).count() >= benchSeconds) {
            running = false;
        }
    }

    // Shutdown.
    running = false;
    captureThread.join();
    cap.release();

    if (yoloEnabled) {
        yoloPipeline.stop();
        std::cout << "  YOLO processed "
                  << yoloPipeline.processedCount.load()
                  << " frames\n";
    }

    if (writer.isOpened()) {
        recQ.close();
        if (writerThread.joinable()) writerThread.join();
        writer.release();
    }

    if (showDisplay) cv::destroyAllWindows();

    if (benchSeconds > 0.0) {
        double total = std::chrono::duration<double>(clk::now() - benchStart).count();
        std::cout << "\nBench: " << capCount.load() << " frames in " << total
                  << " s = " << (capCount.load() / total) << " fps avg\n";
    }

#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return 0;
}
