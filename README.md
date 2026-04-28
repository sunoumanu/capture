# Summary
The MS2109 is a highly integrated HDMI to USB 2.0 Video/Audio Capture Controller. 

The board have following capabilities:

Video Input: Supports HDMI 1.4b (up to 4K @ 30Hz input).
USB Output: Implements USB Video Class (UVC) and USB Audio Class (UAC), meaning it works plug-and-play with Windows, macOS, and Linux without needing custom drivers.
Encoding: It typically outputs video in MJPEG or YUY2 formats (usually maxing out at 1080p @ 30Hz or 720p @ 60Hz for the USB stream).
Audio: Includes a built-in 96kHz mono or 48kHz stereo ADC for audio capture.

# Tools

https://visualstudio.microsoft.com/vs/community/
build tools, compiler, vs ide

git clone https://github.com/microsoft/vcpkg.git C:\dev\vcpkg
.\bootstrap-vcpkg.bat
.\vcpkg install opencv4:x64-windows
.\vcpkg integrate install
winget install Kitware.CMake

# Build
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=c:/dev/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Release

# Run
.\build\Release\ms2109_capture --list
  [0] HP Wide Vision HD Camera
  [1] USB Video
  [2] OBS Virtual Camera

.\build\Release\ms2109_capture 1
