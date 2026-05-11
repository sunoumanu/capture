// ms2109_device.cpp
// Platform-specific video capture device enumeration and name resolution.

#include "ms2109_device.hpp"

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <dshow.h>
  #pragma comment(lib, "ole32.lib")
  #pragma comment(lib, "oleaut32.lib")
  #pragma comment(lib, "strmiids.lib")
#endif

#ifdef _WIN32
std::vector<std::string> listVideoDevices() {
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
std::vector<std::string> listVideoDevices() {
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

int resolveDevice(const std::string& arg,
                  const std::vector<std::string>& names) {
    if (isAllDigits(arg)) return std::atoi(arg.c_str());
    std::string needle = toLower(arg);
    for (size_t i = 0; i < names.size(); ++i) {
        if (toLower(names[i]).find(needle) != std::string::npos) return static_cast<int>(i);
    }
    return -1;
}
