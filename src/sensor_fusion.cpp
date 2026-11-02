#include "sensor_fusion.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sensor_fusion {

DepthWindow depth_readings{};
ImuWindow imu_readings{};
Max30102Window max30102_readings{};

namespace {

constexpr float kMadToStandardDeviation = 1.4826F;

bool isFinite(float value) {
    return std::isfinite(value);
}

bool isSensorInvalid(QualityFlags flags) {
    return (flags & quality::SensorInvalid) != 0U;
}

bool isValidDepthWindow(const DepthWindow& input) {
    if (input.samples.empty() || input.end_us < input.start_us) {
        return false;
    }

    TimestampUs previous_timestamp = 0;
    bool first = true;
    for (const auto& sample : input.samples) {
        if (!isFinite(sample.depth_m) && !isSensorInvalid(sample.quality_flags)) {
            return false;
        }
        if (!first && sample.timestamp_us <= previous_timestamp) {
            return false;
        }
        previous_timestamp = sample.timestamp_us;
        first = false;
    }
    return true;
}

bool isValidImuWindow(const ImuWindow& input) {
    if (input.samples.empty() || input.end_us < input.start_us) {
        return false;
    }

    TimestampUs previous_timestamp = 0;
    bool first = true;
    for (const auto& sample : input.samples) {
        if (!isSensorInvalid(sample.quality_flags) &&
            (!isFinite(sample.acceleration_x) ||
            !isFinite(sample.acceleration_y) ||
            !isFinite(sample.acceleration_z) ||
            !isFinite(sample.angular_velocity_x) ||
            !isFinite(sample.angular_velocity_y) ||
            !isFinite(sample.angular_velocity_z))) {
            return false;
        }
        if (!first && sample.timestamp_us <= previous_timestamp) {
            return false;
        }
        previous_timestamp = sample.timestamp_us;
        first = false;
    }
    return true;
}

bool isValidMax30102Window(const Max30102Window& input) {
    if (input.samples.empty() || input.end_us < input.start_us) {
        return false;
    }

    TimestampUs previous_timestamp = 0;
    bool first = true;
    for (const auto& sample : input.samples) {
        if (!first && sample.timestamp_us <= previous_timestamp) {
            return false;
        }
        previous_timestamp = sample.timestamp_us;
        first = false;
    }
    return true;
}

float median(std::vector<float> values) {
    const auto middle = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    if (values.size() % 2U != 0U) {
        return *middle;
    }

    const float upper = *middle;
    const auto lower_middle = values.begin() +
                              static_cast<std::ptrdiff_t>(values.size() / 2U - 1U);
    std::nth_element(values.begin(), lower_middle, values.end());
    return (*lower_middle + upper) / 2.0F;
}

bool validRiskValue(const std::optional<float>& value) {
    return value.has_value() && isFinite(*value) && *value >= 0.0F && *value <= 1.0F;
}

StatusCode firstProcessingFailure(const FusionOutput& output) {
    if (output.depth.status != StatusCode::Ok) {
        return output.depth.status;
    }
    if (output.ascent.status != StatusCode::Ok) {
        return output.ascent.status;
    }
    if (output.imu.status != StatusCode::Ok) {
        return output.imu.status;
    }
    if (output.max30102.status != StatusCode::Ok) {
        return output.max30102.status;
    }
    return StatusCode::Ok;
}

FusionOutput buildFeatureOutput(const FusionWindow& input,
                                const DepthFilterConfig& depth_config,
                                std::size_t minimum_slope_samples,
                                const Max30102Features& max30102) {
    FusionOutput output;
    output.depth = cleanDepth(input.depth, depth_config);
    output.ascent = estimateDepthSlope(output.depth.samples, minimum_slope_samples);
    output.imu = describeImu(input.imu);
    output.max30102 = max30102;
    return output;
}

FusionOutput finalizeWithRisk(FusionOutput output,
                              const RiskComponents& components) {
    const StatusCode processing_status = firstProcessingFailure(output);
    if (processing_status != StatusCode::Ok) {
        output.status = processing_status;
        output.risk.status = processing_status;
        output.risk.value.reset();
        return output;
    }

    output.risk = calculateRiskScore(components);
    output.status = output.risk.status;
    return output;
}

}  // namespace

DepthFilterResult cleanDepth(const DepthWindow& input,
                             const DepthFilterConfig& config) {
    DepthFilterResult result;

    if (!isValidDepthWindow(input)) {
        result.status = input.samples.empty() ? StatusCode::EmptyWindow
                                              : StatusCode::InvalidWindow;
        return result;
    }

    if (config.hampel_radius == 0U ||
        !isFinite(config.hampel_threshold) || config.hampel_threshold <= 0.0F ||
        !isFinite(config.ema_alpha) || config.ema_alpha <= 0.0F ||
        config.ema_alpha > 1.0F) {
        result.status = StatusCode::InvalidConfiguration;
        return result;
    }

    result.samples.reserve(input.samples.size());
    std::optional<float> previous_ema;

    for (std::size_t index = 0; index < input.samples.size(); ++index) {
        const std::size_t first = index > config.hampel_radius
                                      ? index - config.hampel_radius
                                      : 0U;
        const std::size_t last = std::min(
            input.samples.size() - 1U, index + config.hampel_radius);

        std::vector<float> neighborhood;
        neighborhood.reserve(last - first + 1U);
        for (std::size_t neighbor = first; neighbor <= last; ++neighbor) {
            const auto& sample = input.samples[neighbor];
            if (!isSensorInvalid(sample.quality_flags) && isFinite(sample.depth_m)) {
                neighborhood.push_back(sample.depth_m);
            }
        }

        CleanedDepthReading cleaned;
        cleaned.timestamp_us = input.samples[index].timestamp_us;
        cleaned.quality_flags = input.samples[index].quality_flags;
        const bool current_invalid =
            isSensorInvalid(input.samples[index].quality_flags) ||
            !isFinite(input.samples[index].depth_m);

        if (current_invalid || neighborhood.empty()) {
            ++result.invalid_count;
            cleaned.quality_flags |= quality::SensorInvalid;
            if (previous_ema.has_value()) {
                cleaned.cleaned_depth_m = *previous_ema;
                cleaned.has_value = true;
                cleaned.quality_flags |= quality::CarriedForward;
            }
            result.samples.push_back(cleaned);
            continue;
        }

        const float neighborhood_median = median(neighborhood);
        std::vector<float> absolute_deviations;
        absolute_deviations.reserve(neighborhood.size());
        for (const float value : neighborhood) {
            absolute_deviations.push_back(std::fabs(value - neighborhood_median));
        }

        const float mad = median(absolute_deviations);
        const float allowed_deviation =
            config.hampel_threshold * kMadToStandardDeviation * mad;
        const bool is_outlier =
            std::fabs(input.samples[index].depth_m - neighborhood_median) >
            allowed_deviation;

        cleaned.outlier_rejected = is_outlier;

        if (is_outlier) {
            ++result.rejected_count;
            cleaned.quality_flags |= quality::OutlierRejected;
            if (previous_ema.has_value()) {
                cleaned.cleaned_depth_m = *previous_ema;
                cleaned.has_value = true;
                cleaned.quality_flags |= quality::CarriedForward;
            }
        } else {
            const float current = input.samples[index].depth_m;
            const float filtered = previous_ema.has_value()
                                       ? config.ema_alpha * current +
                                             (1.0F - config.ema_alpha) * *previous_ema
                                       : current;
            previous_ema = filtered;
            cleaned.cleaned_depth_m = filtered;
            cleaned.has_value = true;
        }

        result.samples.push_back(cleaned);
    }

    result.status = StatusCode::Ok;
    return result;
}

SlopeEstimate estimateDepthSlope(
    const std::vector<CleanedDepthReading>& cleaned_samples,
    std::size_t minimum_valid_samples) {
    SlopeEstimate result;
    if (minimum_valid_samples < 2U) {
        result.status = StatusCode::InvalidConfiguration;
        return result;
    }

    std::vector<std::pair<double, double>> valid_samples;
    valid_samples.reserve(cleaned_samples.size());
    for (const auto& sample : cleaned_samples) {
        if (sample.has_value && isFinite(sample.cleaned_depth_m)) {
            valid_samples.emplace_back(static_cast<double>(sample.timestamp_us) / 1'000'000.0,
                                       static_cast<double>(sample.cleaned_depth_m));
        }
    }

    result.valid_sample_count = valid_samples.size();
    if (valid_samples.size() < minimum_valid_samples) {
        result.status = StatusCode::InsufficientData;
        return result;
    }

    const double reference_time = valid_samples.front().first;
    double mean_time = 0.0;
    double mean_depth = 0.0;
    for (const auto& sample : valid_samples) {
        mean_time += sample.first - reference_time;
        mean_depth += sample.second;
    }
    mean_time /= static_cast<double>(valid_samples.size());
    mean_depth /= static_cast<double>(valid_samples.size());

    double numerator = 0.0;
    double denominator = 0.0;
    for (const auto& sample : valid_samples) {
        const double time_delta = (sample.first - reference_time) - mean_time;
        const double depth_delta = sample.second - mean_depth;
        numerator += time_delta * depth_delta;
        denominator += time_delta * time_delta;
    }

    if (denominator <= std::numeric_limits<double>::epsilon()) {
        result.status = StatusCode::DegenerateRegression;
        return result;
    }

    result.slope_m_per_s = static_cast<float>(numerator / denominator);
    result.status = StatusCode::Ok;
    return result;
}

ImuFeatures describeImu(const ImuWindow& input) {
    ImuFeatures result;
    if (!isValidImuWindow(input)) {
        result.status = input.samples.empty() ? StatusCode::EmptyWindow
                                              : StatusCode::InvalidWindow;
        return result;
    }

    std::vector<float> acceleration_magnitudes;
    acceleration_magnitudes.reserve(input.samples.size());
    double angular_speed_sum = 0.0;
    double acceleration_x_sum = 0.0;
    double acceleration_y_sum = 0.0;
    double acceleration_z_sum = 0.0;

    for (const auto& sample : input.samples) {
        if (isSensorInvalid(sample.quality_flags)) {
            continue;
        }
        const float acceleration_magnitude = std::sqrt(
            sample.acceleration_x * sample.acceleration_x +
            sample.acceleration_y * sample.acceleration_y +
            sample.acceleration_z * sample.acceleration_z);
        const float angular_speed = std::sqrt(
            sample.angular_velocity_x * sample.angular_velocity_x +
            sample.angular_velocity_y * sample.angular_velocity_y +
            sample.angular_velocity_z * sample.angular_velocity_z);

        acceleration_magnitudes.push_back(acceleration_magnitude);
        angular_speed_sum += angular_speed;
        acceleration_x_sum += sample.acceleration_x;
        acceleration_y_sum += sample.acceleration_y;
        acceleration_z_sum += sample.acceleration_z;
    }

    if (acceleration_magnitudes.empty()) {
        result.status = StatusCode::InsufficientData;
        result.sample_count = input.samples.size();
        return result;
    }

    double acceleration_sum = 0.0;
    for (const float magnitude : acceleration_magnitudes) {
        acceleration_sum += magnitude;
    }
    const double acceleration_mean =
        acceleration_sum / static_cast<double>(acceleration_magnitudes.size());

    double variance_sum = 0.0;
    for (const float magnitude : acceleration_magnitudes) {
        const double difference = static_cast<double>(magnitude) - acceleration_mean;
        variance_sum += difference * difference;
    }

    result.status = StatusCode::Ok;
    result.sample_count = input.samples.size();
    result.valid_sample_count = acceleration_magnitudes.size();
    result.mean_acceleration_magnitude = static_cast<float>(acceleration_mean);
    result.acceleration_magnitude_stddev = static_cast<float>(std::sqrt(
        variance_sum / static_cast<double>(acceleration_magnitudes.size())));
    result.mean_angular_speed_rad_per_s = static_cast<float>(
        angular_speed_sum / static_cast<double>(input.samples.size()));

    const double sample_count = static_cast<double>(input.samples.size());
    const float mean_acceleration_x =
        static_cast<float>(acceleration_x_sum / sample_count);
    const float mean_acceleration_y =
        static_cast<float>(acceleration_y_sum / sample_count);
    const float mean_acceleration_z =
        static_cast<float>(acceleration_z_sum / sample_count);
    const float horizontal_acceleration = std::sqrt(
        mean_acceleration_y * mean_acceleration_y +
        mean_acceleration_z * mean_acceleration_z);
    const float mean_acceleration_magnitude = std::sqrt(
        mean_acceleration_x * mean_acceleration_x +
        mean_acceleration_y * mean_acceleration_y +
        mean_acceleration_z * mean_acceleration_z);

    if (mean_acceleration_magnitude > std::numeric_limits<float>::epsilon()) {
        result.orientation.valid = true;
        result.orientation.roll_rad =
            std::atan2(mean_acceleration_y, mean_acceleration_z);
        result.orientation.pitch_rad =
            std::atan2(-mean_acceleration_x, horizontal_acceleration);
        // BMI270 is a 6DoF IMU. Yaw needs a heading reference and is not
        // observable from acceleration and gyro data over an isolated window.
        result.orientation.yaw_valid = false;
    }

    return result;
}

Max30102Features describeMax30102(const Max30102Window& input) {
    Max30102Features result;
    if (!isValidMax30102Window(input)) {
        result.status = input.samples.empty() ? StatusCode::EmptyWindow
                                              : StatusCode::InvalidWindow;
        return result;
    }

    result.sample_count = input.samples.size();
    double red_sum = 0.0;
    double infrared_sum = 0.0;
    for (const auto& sample : input.samples) {
        if (isSensorInvalid(sample.quality_flags)) {
            continue;
        }
        red_sum += static_cast<double>(sample.red);
        infrared_sum += static_cast<double>(sample.infrared);
    }

    result.valid_sample_count = input.samples.size();
    for (const auto& sample : input.samples) {
        if (isSensorInvalid(sample.quality_flags)) {
            --result.valid_sample_count;
        }
    }
    if (result.valid_sample_count == 0U) {
        result.status = StatusCode::InsufficientData;
        return result;
    }

    const double sample_count = static_cast<double>(result.valid_sample_count);
    const double red_mean = red_sum / sample_count;
    const double infrared_mean = infrared_sum / sample_count;
    double red_variance_sum = 0.0;
    double infrared_variance_sum = 0.0;

    for (const auto& sample : input.samples) {
        if (isSensorInvalid(sample.quality_flags)) {
            continue;
        }
        const double red_delta = static_cast<double>(sample.red) - red_mean;
        const double infrared_delta =
            static_cast<double>(sample.infrared) - infrared_mean;
        red_variance_sum += red_delta * red_delta;
        infrared_variance_sum += infrared_delta * infrared_delta;
    }

    result.mean_red = static_cast<float>(red_mean);
    result.mean_infrared = static_cast<float>(infrared_mean);
    result.red_stddev = static_cast<float>(std::sqrt(red_variance_sum / sample_count));
    result.infrared_stddev = static_cast<float>(
        std::sqrt(infrared_variance_sum / sample_count));
    if (red_mean > 0.0) {
        result.red_perfusion_index = static_cast<float>(
            *result.red_stddev / static_cast<float>(red_mean));
    }
    if (infrared_mean > 0.0) {
        result.infrared_perfusion_index = static_cast<float>(
            *result.infrared_stddev / static_cast<float>(infrared_mean));
    }
    result.status = StatusCode::Ok;
    return result;
}

Max30102Features extractMax30102Metrics(
    const Max30102Window& input,
    const Max30102Extractors& extractors) {
    Max30102Features result = describeMax30102(input);
    if (result.status != StatusCode::Ok) {
        return result;
    }

    if (!extractors.pulse || !extractors.spo2) {
        result.status = StatusCode::MissingSignalProcessor;
        return result;
    }

    result.pulse_bpm = extractors.pulse(input, result);
    result.spo2_percent = extractors.spo2(input, result);
    if (!result.pulse_bpm.has_value() || !result.spo2_percent.has_value()) {
        result.status = StatusCode::InsufficientData;
        return result;
    }

    result.status = StatusCode::Ok;
    return result;
}

RiskScore calculateRiskScore(const RiskComponents& components) {
    RiskScore result;

    if (!components.quality_q.has_value()) {
        result.status = StatusCode::MissingQuality;
        return result;
    }

    if (!validRiskValue(components.motion_m) ||
        !validRiskValue(components.ascent_a) ||
        !validRiskValue(components.pulse_h) ||
        !validRiskValue(components.spo2_o)) {
        result.status = StatusCode::MissingRiskComponent;
        return result;
    }

    if (!validRiskValue(components.quality_q)) {
        result.status = StatusCode::InvalidRiskComponent;
        return result;
    }

    const float quality = *components.quality_q;
    const float numerator = 0.37F * *components.motion_m +
                            0.33F * *components.ascent_a +
                            0.15F * quality * *components.pulse_h +
                            0.15F * quality * *components.spo2_o;
    const float denominator = 0.70F + 0.30F * quality;

    result.value = numerator / denominator;
    result.status = StatusCode::Ok;
    return result;
}

MappedRiskComponents mapRiskComponents(const ImuFeatures& imu,
                                       const SlopeEstimate& ascent,
                                       const Max30102Features& max30102,
                                       std::optional<float> quality_q,
                                       const RiskMappers& mappers) {
    MappedRiskComponents result;
    result.components.quality_q = quality_q;

    if (!mappers.motion || !mappers.ascent || !mappers.pulse || !mappers.spo2) {
        result.status = StatusCode::MissingRiskMapper;
        return result;
    }

    result.components.motion_m = mappers.motion(imu);
    result.components.ascent_a = mappers.ascent(ascent);
    result.components.pulse_h = mappers.pulse(max30102);
    result.components.spo2_o = mappers.spo2(max30102);

    if (!result.components.motion_m.has_value() ||
        !result.components.ascent_a.has_value() ||
        !result.components.pulse_h.has_value() ||
        !result.components.spo2_o.has_value()) {
        result.status = StatusCode::MissingRiskComponent;
        return result;
    }

    result.status = StatusCode::Ok;
    return result;
}

SensorFusion::SensorFusion(DepthFilterConfig depth_config,
                           std::size_t minimum_slope_samples)
    : depth_config_(depth_config),
      minimum_slope_samples_(minimum_slope_samples) {}

FusionOutput SensorFusion::process(const FusionWindow& input,
                                   const RiskComponents& components) const {
    return finalizeWithRisk(
        buildFeatureOutput(input, depth_config_, minimum_slope_samples_,
                           describeMax30102(input.max30102)),
        components);
}

FusionOutput SensorFusion::process(const FusionWindow& input,
                                   const RiskMappers& mappers,
                                   std::optional<float> quality_q) const {
    FusionOutput output = buildFeatureOutput(
        input, depth_config_, minimum_slope_samples_, describeMax30102(input.max30102));
    if (const StatusCode processing_status = firstProcessingFailure(output);
        processing_status != StatusCode::Ok) {
        output.status = processing_status;
        output.risk.status = processing_status;
        output.risk.value.reset();
        return output;
    }

    const MappedRiskComponents mapped = mapRiskComponents(
        output.imu, output.ascent, output.max30102, quality_q, mappers);

    if (mapped.status != StatusCode::Ok) {
        output.status = mapped.status;
        output.risk.status = mapped.status;
        output.risk.value.reset();
        return output;
    }
    return finalizeWithRisk(std::move(output), mapped.components);
}

FusionOutput SensorFusion::process(
    const FusionWindow& input,
    const Max30102Extractors& max30102_extractors,
    const RiskMappers& mappers,
    std::optional<float> quality_q) const {
    FusionOutput output = buildFeatureOutput(
        input, depth_config_, minimum_slope_samples_,
        extractMax30102Metrics(input.max30102, max30102_extractors));
    if (const StatusCode processing_status = firstProcessingFailure(output);
        processing_status != StatusCode::Ok) {
        output.status = processing_status;
        output.risk.status = processing_status;
        output.risk.value.reset();
        return output;
    }

    const MappedRiskComponents mapped = mapRiskComponents(
        output.imu, output.ascent, output.max30102, quality_q, mappers);
    if (mapped.status != StatusCode::Ok) {
        output.status = mapped.status;
        output.risk.status = mapped.status;
        output.risk.value.reset();
        return output;
    }
    return finalizeWithRisk(std::move(output), mapped.components);
}

}  // namespace sensor_fusion
