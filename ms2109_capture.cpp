// ms2109_capture.cpp
// High-throughput capture program for MS2109-based HDMI->USB UVC capture
// devices. Optimized for maximum sustained FPS.
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
// Key things to know about the MS2109:
//   * USB 2.0 only. Hardware ceiling is 1920x1080 @ 30 fps OR 1280x720 @ 60 fps.
//     There is no way to exceed those — the silicon will not deliver more.
//   * To hit 1080p at all you MUST get MJPEG; YUY2 at 1080p exceeds USB 2.0
//     bandwidth and the chip falls back to ~5 fps.
//   * The device's reported FPS in CAP_PROP_FPS is unreliable; trust the
//     measured rate from the capture thread.
//
// Build (Windows, MSVC, vcpkg):
//     cl /std:c++17 /O2 /EHsc ms2109_capture.cpp /I <opencv-include> ^
//        /link opencv_world4xx.lib ole32.lib oleaut32.lib strmiids.lib winmm.lib
//
// Build (Linux):
//     g++ -O3 -std=c++17 -pthread ms2109_capture.cpp -o ms2109_capture \
//         `pkg-config --cflags --libs opencv4`
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
// Press 'q' or ESC to quit. Press 's' to snapshot a still PNG.

#include <opencv2/opencv.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <cctype>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <deque>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <dshow.h>
  #include <timeapi.h>
  #pragma comment(lib, "ole32.lib")
  #pragma comment(lib, "oleaut32.lib")
  #pragma comment(lib, "strmiids.lib")
  #pragma comment(lib, "winmm.lib")
#endif

using clk = std::chrono::steady_clock;

struct Mode {
    int width;
    int height;
    int fps;
};

static Mode parseMode(const std::string& s) {
    if (s == "1080p30") return {1920, 1080, 30};
    if (s == "720p60")  return {1280, 720, 60};
    if (s == "720p30")  return {1280, 720, 30};
    if (s == "480p30")  return {640, 480, 30};
    std::cerr << "Unknown mode '" << s << "', falling back to 1080p30.\n";
    return {1920, 1080, 30};
}

// ---- Device enumeration --------------------------------------------------

#ifdef _WIN32
static std::vector<std::string> listVideoDevices() {
    std::vector<std::string> names;
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return names;

    ICreateDevEnum* devEnum = nullptr;
    if (FAILED(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&devEnum)))) {
        CoUninitialize();
        return names;
    }

    IEnumMoniker* enumMon = nullptr;
    if (devEnum->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &enumMon, 0) == S_OK) {
        IMoniker* mon = nullptr;
        while (enumMon->Next(1, &mon, nullptr) == S_OK) {
            IPropertyBag* props = nullptr;
            if (SUCCEEDED(mon->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&props)))) {
                VARIANT v;
                VariantInit(&v);
                if (SUCCEEDED(props->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR) {
                    int len = WideCharToMultiByte(CP_UTF8, 0, v.bstrVal, -1,
                                                  nullptr, 0, nullptr, nullptr);
                    std::string s(len > 0 ? len - 1 : 0, '\0');
                    if (len > 0) {
                        WideCharToMultiByte(CP_UTF8, 0, v.bstrVal, -1,
                                            s.data(), len, nullptr, nullptr);
                    }
                    names.push_back(std::move(s));
                }
                VariantClear(&v);
                props->Release();
            }
            mon->Release();
        }
        enumMon->Release();
    }
    devEnum->Release();
    CoUninitialize();
    return names;
}
#else
static std::vector<std::string> listVideoDevices() {
    std::vector<std::string> names;
    for (int i = 0; i < 10; ++i) {
        cv::VideoCapture probe(i, cv::CAP_V4L2);
        if (probe.isOpened()) {
            names.push_back("/dev/video" + std::to_string(i));
            probe.release();
        }
    }
    return names;
}
#endif

static bool isAllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char c : s) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

static std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static int resolveDevice(const std::string& arg,
                         const std::vector<std::string>& names) {
    if (isAllDigits(arg)) return std::atoi(arg.c_str());
    std::string needle = toLower(arg);
    for (size_t i = 0; i < names.size(); ++i) {
        if (toLower(names[i]).find(needle) != std::string::npos) return (int)i;
    }
    return -1;
}

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

int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--list") {
        auto names = listVideoDevices();
        if (names.empty()) std::cout << "(no video capture devices found)\n";
        else for (size_t i = 0; i < names.size(); ++i)
            std::cout << "  [" << i << "] " << names[i] << "\n";
        return 0;
    }

    // Positional: <device> [mode] [outfile]
    std::string devArg = (argc > 1) ? argv[1] : "0";
    Mode mode          = (argc > 2 && argv[2][0] != '-') ? parseMode(argv[2]) : Mode{1920,1080,30};
    std::string outFile = (argc > 3 && argv[3][0] != '-') ? argv[3] : "";

    // Flags (anywhere after the device arg).
    bool   showDisplay  = true;
    int    displayEvery = 1;
    double displayScale = 1.0;
    double benchSeconds = 0.0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--no-display")             showDisplay = false;
        else if (a == "--display-every" && i+1 < argc) displayEvery = std::max(1, std::atoi(argv[++i]));
        else if (a == "--display-scale" && i+1 < argc) displayScale = std::atof(argv[++i]);
        else if (a == "--bench" && i+1 < argc)         benchSeconds = std::atof(argv[++i]);
    }

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

    auto fourccStr = [](int f) {
        char s[5] = { (char)(f & 0xFF), (char)((f >> 8) & 0xFF),
                      (char)((f >> 16) & 0xFF), (char)((f >> 24) & 0xFF), 0 };
        return std::string(s);
    };
    std::string fcc = fourccStr(negotiatedFourcc);
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
    int       displayCount = 0;
    int       snapshotIdx  = 0;
    auto      benchStart   = clk::now();
    cv::Mat   scaled;       // reused buffer for downscaled preview

    while (running.load(std::memory_order_relaxed)) {
        bool fresh = mailbox.tryTake(lastSeen, displayed);

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
