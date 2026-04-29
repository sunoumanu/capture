# Summary

Capture the video stream from USB using low-cost capture board with MS2109 chip.

The MS2109 is a highly integrated HDMI to USB 2.0 Video/Audio Capture Controller. 

The board have following capabilities:

Video Input: Supports HDMI 1.4b (up to 4K @ 30Hz input).
USB Output: Implements USB Video Class (UVC) and USB Audio Class (UAC), meaning it works plug-and-play with Windows, macOS, and Linux without needing custom drivers.
Encoding: It typically outputs video in MJPEG or YUY2 formats (usually maxing out at 1080p @ 30Hz or 720p @ 60Hz for the USB stream).
Audio: Includes a built-in 96kHz mono or 48kHz stereo ADC for audio capture.

# Tools

https://visualstudio.microsoft.com/vs/community/
build tools, compiler, vs ide

```
git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg install opencv4:x64-windows
.\vcpkg integrate install
winget install Kitware.CMake
```

# Build example:
```
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=c:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release
```

# Build (Windows, MSVC, vcpkg):
```
     cl /std:c++17 /O2 /EHsc ms2109_capture.cpp /I <opencv-include> ^/        /link opencv_world4xx.lib ole32.lib oleaut32.lib strmiids.lib winmm.lib
```
# Build (Linux):
```
     g++ -O3 -std=c++17 -pthread ms2109_capture.cpp -o ms2109_capture \
        `pkg-config --cflags --libs opencv4`
```

# Usage:
```
     ms2109_capture                       -> opens device 0, 1080p30
     ms2109_capture --list                -> lists every video capture device by name
     ms2109_capture 1                     -> opens device index 1
     ms2109_capture "USB Video"           -> opens first device whose name matches
     ms2109_capture 1 720p60              -> 1280x720 @ 60 fps from device 1
     ms2109_capture "USB Video" 1080p30 out.avi -> record to out.avi

 Flags (any position after the device arg): 
     --no-display       : don't show a preview window (max headless throughput)
     --display-every N  : only blit every Nth frame to the window (default 1)
     --display-scale F  : downscale preview by factor F (e.g. 0.5)
     --bench S          : exit after S seconds, print average capture FPS

Press 'q' or ESC to quit. Press 's' to snapshot a still PNG.
```

# Run example
```
.\build\Release\ms2109_capture --list
  [0] HP Wide Vision HD Camera
  [1] USB Video
  [2] OBS Virtual Camera

.\build\Release\ms2109_capture 1
```
