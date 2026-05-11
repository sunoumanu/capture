// ms2109_device.hpp
// Platform-specific video capture device enumeration and name resolution.

#ifndef MS2109_DEVICE_HPP
#define MS2109_DEVICE_HPP

#include "ms2109_common.hpp"
#include <vector>
#include <string>

std::vector<std::string> listVideoDevices();
int resolveDevice(const std::string& arg,
                  const std::vector<std::string>& names);

#endif // MS2109_DEVICE_HPP
