// ms2109_capture.cpp
// Simple capture program for MS2109-based HDMI->USB UVC capture devices.
//
// The MS2109 enumerates as a standard USB Video Class (UVC) device, so we
// can talk to it through any normal capture API. This file uses OpenCV's
// VideoCapture which wraps DirectShow / MSMF on Windows and V4L2 on Linux.
//
// Key things to know about the MS2109:
//   * USB 2.0 only — bandwidth is the bottleneck.
//   * To hit 1920x1080 @ 30 fps you MUST request MJPEG. YUY2 at 1080p
//     exceeds USB 2.0 throughput and the device will silently drop to a
//     lower frame rate (or refuse the format) if you ask for it.
//   * 1280x720 @ 60 fps is also typically supported via MJPEG.
//   * The device LIES about supported fps in some descriptors; if FPS_PROP
//     reports 0 or something odd, just trust the timestamps of arriving
//     frames.
//
// Build (Windows, MSVC, vcpkg):
//     cl /std:c++17 /EHsc ms2109_capture.cpp /I <opencv-include> ^
//        /link opencv_world4xx.lib ole32.lib oleaut32.lib strmiids.lib
//
// Build (Linux):
//     g++ -std=c++17 ms2109_capture.cpp -o ms2109_capture `pkg-config --cflags --libs opencv4`
//
// Usage:
//     ms2109_capture                       -> opens device 0, 1080p30
//     ms2109_capture --list                -> lists every video capture device by name
//     ms2109_capture 1                     -> opens device index 1
//     ms2109_capture "USB Video"           -> opens first device whose name contains "USB Video"
//     ms2109_capture 1 720p60              -> 1280x720 @ 60 fps from device 1
//     ms2109_capture "USB Video" 1080p30 out.avi -> record to out.avi
//
// Press 'q' or ESC to quit. Press 's' to snapshot a still PNG.

#include <opencv2/opencv.hpp>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <cctype>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <dshow.h>
  #pragma comment(lib, "ole32.lib")
  #pragma comment(lib, "oleaut32.lib")
  #pragma comment(lib, "strmiids.lib")
#endif

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
// Returns the friendly names of every video capture device, in the same
// order that OpenCV / DirectShow assigns to indices 0, 1, 2, ...

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
// On Linux just probe /dev/video0..9 by trying to open them.
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

// Resolve the device argument (either a numeric index or a name substring)
// into a numeric index. Returns -1 if no match.
static int resolveDevice(const std::string& arg,
                         const std::vector<std::string>& names) {
    if (isAllDigits(arg)) return std::atoi(arg.c_str());

    std::string needle = toLower(arg);
    for (size_t i = 0; i < names.size(); ++i) {
        if (toLower(names[i]).find(needle) != std::string::npos) {
            return (int)i;
        }
    }
    return -1;
}

int main(int argc, char** argv) {
    // --list: print devices and exit.
    if (argc > 1 && std::string(argv[1]) == "--list") {
        auto names = listVideoDevices();
        if (names.empty()) {
            std::cout << "(no video capture devices found)\n";
        } else {
            for (size_t i = 0; i < names.size(); ++i) {
                std::cout << "  [" << i << "] " << names[i] << "\n";
            }
        }
        return 0;
    }

    std::string devArg = (argc > 1) ? argv[1] : "0";
    Mode mode          = (argc > 2) ? parseMode(argv[2]) : Mode{1920, 1080, 30};
    std::string outFile = (argc > 3) ? argv[3] : "";

    auto names = listVideoDevices();
    int deviceIndex = resolveDevice(devArg, names);
    if (deviceIndex < 0) {
        std::cerr << "No device matched \"" << devArg << "\". Available:\n";
        for (size_t i = 0; i < names.size(); ++i) {
            std::cerr << "  [" << i << "] " << names[i] << "\n";
        }
        return 1;
    }

    if (deviceIndex < (int)names.size()) {
        std::cout << "Selected [" << deviceIndex << "] " << names[deviceIndex] << "\n";
    }

    // On Windows, CAP_DSHOW is usually the most reliable backend for UVC
    // devices. CAP_MSMF works too but sometimes negotiates the wrong format.
    // On Linux, CAP_V4L2 is the right backend.
#ifdef _WIN32
    int backend = cv::CAP_DSHOW;
#else
    int backend = cv::CAP_V4L2;
#endif

    // CRITICAL: pass the format params to open() instead of using set() afterwards.
    // The MS2109 / OpenCV-DSHOW will lock to YUY2 during open() and ignore a later
    // FOURCC change, which traps you at ~5 fps on 1080p (YUY2 bandwidth > USB 2.0).
    // Passing params to open() applies them during the initial negotiation.
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

    // Keep the internal queue shallow so we display the freshest frame, not a
    // stale one. OpenCV defaults to a 4-deep buffer which adds ~130 ms latency.
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    // Read back what the device actually agreed to — may differ from request.
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
              << " fps, format=" << fcc << " (0x" << std::hex
              << negotiatedFourcc << std::dec << ")\n";

    // Warn loudly if we did NOT get MJPEG — that's the #1 cause of slow capture.
    if (fcc != "MJPG" && fcc != "mjpg") {
        std::cerr << "\n!!! WARNING: device is NOT delivering MJPEG (got '" << fcc << "').\n"
                  << "    USB 2.0 cannot carry uncompressed video at " << w << "x" << h
                  << "@" << mode.fps << ", expect ~5 fps.\n"
                  << "    Try a lower resolution (720p30 / 480p30) if MJPEG can't be\n"
                  << "    negotiated, or use the MSMF backend instead of DSHOW.\n\n";
    }

    // Optional recorder. We write MJPEG into AVI to avoid an extra encode pass
    // and to preserve what the chip is already producing.
    cv::VideoWriter writer;
    if (!outFile.empty()) {
        int fourcc = cv::VideoWriter::fourcc('M', 'J', 'P', 'G');
        double recFps = (fps > 1.0 && fps < 200.0) ? fps : mode.fps;
        if (!writer.open(outFile, fourcc, recFps, cv::Size(w, h), true)) {
            std::cerr << "Failed to open output file: " << outFile << "\n";
            return 2;
        }
        std::cout << "Recording to " << outFile << "\n";
    }

    cv::namedWindow("MS2109 capture", cv::WINDOW_NORMAL);
    cv::resizeWindow("MS2109 capture", 1280, 720);

    cv::Mat frame;
    int frameCount = 0;
    auto t0 = std::chrono::steady_clock::now();
    int snapshotIndex = 0;

    while (true) {
        if (!cap.read(frame) || frame.empty()) {
            // Don't bail on a single dropped frame — MS2109 occasionally
            // hiccups when the HDMI source changes resolution / sleeps.
            std::cerr << "(no frame)\n";
            if (cv::waitKey(10) == 27) break;
            continue;
        }

        if (writer.isOpened()) writer.write(frame);

        cv::imshow("MS2109 capture", frame);

        // Print measured fps every second so we can see what we're really getting.
        ++frameCount;
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - t0).count();
        if (elapsed >= 1.0) {
            std::cout << "  measured " << (frameCount / elapsed) << " fps\n";
            t0 = now;
            frameCount = 0;
        }

        int key = cv::waitKey(1) & 0xFF;
        if (key == 'q' || key == 27) break;          // q or ESC
        if (key == 's') {
            std::string name = "snapshot_" + std::to_string(snapshotIndex++) + ".png";
            cv::imwrite(name, frame);
            std::cout << "Saved " << name << "\n";
        }
    }

    cap.release();
    if (writer.isOpened()) writer.release();
    cv::destroyAllWindows();
    return 0;
}
