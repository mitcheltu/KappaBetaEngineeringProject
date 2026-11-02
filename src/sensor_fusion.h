#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace sensor_fusion {

using TimestampUs = std::uint64_t;

enum class StatusCode {
    Ok,
    EmptyWindow,
    InvalidWindow,
    InvalidConfiguration,
    InsufficientData,
    DegenerateRegression,
    MissingRiskComponent,
    MissingRiskMapper,
    MissingQuality,
    InvalidRiskComponent,
    NotImplemented,
};

struct DepthReading {
    TimestampUs timestamp_us{0};
    // Depth must already be expressed in metres. Pressure-to-depth conversion
    // belongs in the sensor adapter, before this fusion layer.
    float depth_m{0.0F};
};

struct ImuReading {
    TimestampUs timestamp_us{0};
    float acceleration_x{0.0F};
    float acceleration_y{0.0F};
    float acceleration_z{0.0F};
    float angular_velocity_x{0.0F};
    float angular_velocity_y{0.0F};
    float angular_velocity_z{0.0F};
};

struct Max30102Reading {
    TimestampUs timestamp_us{0};
    std::uint32_t red{0};
    std::uint32_t infrared{0};
};

template <typename Reading>
struct ReadingWindow {
    TimestampUs start_us{0};
    TimestampUs end_us{0};
    std::vector<Reading> samples;
};

using DepthWindow = ReadingWindow<DepthReading>;
using ImuWindow = ReadingWindow<ImuReading>;
using Max30102Window = ReadingWindow<Max30102Reading>;

// Temporary caller-visible objects. Sensor drivers may populate these later;
// the processing APIs do not require global state.
extern DepthWindow depth_readings;
extern ImuWindow imu_readings;
extern Max30102Window max30102_readings;

struct DepthFilterConfig {
    // Number of samples on either side of the sample under test.
    std::size_t hampel_radius{0};
    // Hampel outlier multiplier. Must be greater than zero.
    float hampel_threshold{0.0F};
    // EMA coefficient. Must be greater than zero and no greater than one.
    float ema_alpha{0.0F};
};

struct CleanedDepthReading {
    TimestampUs timestamp_us{0};
    float cleaned_depth_m{0.0F};
    bool has_value{false};
    bool outlier_rejected{false};
};

struct DepthFilterResult {
    StatusCode status{StatusCode::EmptyWindow};
    std::vector<CleanedDepthReading> samples;
    std::size_t rejected_count{0};
};

DepthFilterResult cleanDepth(const DepthWindow& input,
                             const DepthFilterConfig& config);

struct SlopeEstimate {
    StatusCode status{StatusCode::InsufficientData};
    std::optional<float> slope_m_per_s;
    std::size_t valid_sample_count{0};
};

// The input is expected to be one already-selected rolling window of cleaned
// samples. A negative slope means ascent when depth is positive downward.
SlopeEstimate estimateDepthSlope(
    const std::vector<CleanedDepthReading>& cleaned_samples,
    std::size_t minimum_valid_samples);

struct OrientationContext {
    bool valid{false};
    bool yaw_valid{false};
    float roll_rad{0.0F};
    float pitch_rad{0.0F};
    float yaw_rad{0.0F};
};

struct ImuFeatures {
    StatusCode status{StatusCode::EmptyWindow};
    std::size_t sample_count{0};
    std::optional<float> mean_acceleration_magnitude;
    std::optional<float> acceleration_magnitude_stddev;
    std::optional<float> mean_angular_speed_rad_per_s;
    OrientationContext orientation;
};

// Provides descriptive IMU features and a gravity-vector roll/pitch estimate.
// It deliberately does not integrate acceleration into velocity or estimate
// yaw without a heading reference.
ImuFeatures describeImu(const ImuWindow& input);

struct Max30102Features {
    StatusCode status{StatusCode::EmptyWindow};
    std::size_t sample_count{0};
    std::optional<float> mean_red;
    std::optional<float> mean_infrared;
    std::optional<float> red_stddev;
    std::optional<float> infrared_stddev;
    // Pulse and SpO2 extraction will be added after its algorithm and quality
    // requirements are specified.
    std::optional<float> pulse_bpm;
    std::optional<float> spo2_percent;
};

Max30102Features describeMax30102(const Max30102Window& input);

struct RiskComponents {
    std::optional<float> motion_m;
    std::optional<float> ascent_a;
    std::optional<float> pulse_h;
    std::optional<float> spo2_o;
    // Intentionally not derived yet. A supplied q=0 means known unusable;
    // a missing q means the final score cannot be calculated.
    std::optional<float> quality_q;
};

struct RiskScore {
    StatusCode status{StatusCode::MissingRiskComponent};
    std::optional<float> value;
};

RiskScore calculateRiskScore(const RiskComponents& components);

using MotionRiskMapper = std::function<std::optional<float>(const ImuFeatures&)>;
using AscentRiskMapper = std::function<std::optional<float>(const SlopeEstimate&)>;
using PulseRiskMapper = std::function<std::optional<float>(const Max30102Features&)>;
using SpO2RiskMapper = std::function<std::optional<float>(const Max30102Features&)>;

struct RiskMappers {
    MotionRiskMapper motion;
    AscentRiskMapper ascent;
    PulseRiskMapper pulse;
    SpO2RiskMapper spo2;
};

struct MappedRiskComponents {
    StatusCode status{StatusCode::MissingRiskMapper};
    RiskComponents components;
};

// These callbacks are the only place where raw/derived features become
// normalized risk values. No project-specific thresholds are embedded here.
MappedRiskComponents mapRiskComponents(const ImuFeatures& imu,
                                       const SlopeEstimate& ascent,
                                       const Max30102Features& max30102,
                                       std::optional<float> quality_q,
                                       const RiskMappers& mappers);

struct FusionWindow {
    DepthWindow depth;
    ImuWindow imu;
    Max30102Window max30102;
};

struct FusionOutput {
    StatusCode status{StatusCode::EmptyWindow};
    DepthFilterResult depth;
    SlopeEstimate ascent;
    ImuFeatures imu;
    Max30102Features max30102;
    RiskScore risk;
};

class SensorFusion {
public:
    explicit SensorFusion(DepthFilterConfig depth_config,
                           std::size_t minimum_slope_samples);

    FusionOutput process(const FusionWindow& input,
                         const RiskComponents& components) const;

    FusionOutput process(const FusionWindow& input,
                         const RiskMappers& mappers,
                         std::optional<float> quality_q) const;

private:
    DepthFilterConfig depth_config_;
    std::size_t minimum_slope_samples_;
};

}  // namespace sensor_fusion
