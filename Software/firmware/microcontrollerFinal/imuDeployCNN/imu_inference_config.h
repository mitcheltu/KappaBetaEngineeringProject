#pragma once

// Generated from the paired training artifacts; preserve axis order.
constexpr int kImuWindowSamples = 60;
constexpr int kImuAxisCount = 6;
constexpr int kImuWindowStrideSamples = 20;
constexpr float kImuGateThreshold = 0.9981237054F;
constexpr const char* kImuAxisOrder[kImuAxisCount] = {"ax", "ay", "az", "gx", "gy", "gz"};
constexpr float kImuMean[kImuAxisCount] = {-3.3666966F, -2.47147036F, 3.67681384F, -0.466952443F, 0.0845656916F, -0.0842822045F};
constexpr float kImuScale[kImuAxisCount] = {8.51525307F, 7.74811697F, 6.51382637F, 2.85136461F, 2.51579571F, 2.56693411F};
