#include "../src/sensor_fusion.h"

#include <cassert>
#include <cmath>

namespace {

using namespace sensor_fusion;

void testDepthCleaningAndSlope() {
    DepthWindow input;
    input.start_us = 0;
    input.end_us = 4'000'000;
    input.samples = {
        {0, 10.0F},
        {1'000'000, 9.5F},
        {2'000'000, 100.0F},
        {3'000'000, 8.5F},
        {4'000'000, 8.0F},
    };

    const DepthFilterConfig config{1, 3.0F, 1.0F};
    const DepthFilterResult cleaned = cleanDepth(input, config);
    assert(cleaned.status == StatusCode::Ok);
    assert(cleaned.rejected_count == 1U);
    assert(cleaned.samples[2].outlier_rejected);

    const SlopeEstimate slope = estimateDepthSlope(cleaned.samples, 3U);
    assert(slope.status == StatusCode::Ok);
    assert(slope.slope_m_per_s.has_value());
    assert(*slope.slope_m_per_s < 0.0F);
}

void testRiskFormula() {
    const RiskComponents components{
        0.8F,  // M
        0.6F,  // A
        0.4F,  // H
        0.2F,  // O
        0.5F,  // q
    };

    const RiskScore score = calculateRiskScore(components);
    assert(score.status == StatusCode::Ok);
    assert(score.value.has_value());

    const float expected = (0.37F * 0.8F + 0.33F * 0.6F +
                            0.15F * 0.5F * 0.4F +
                            0.15F * 0.5F * 0.2F) /
                           (0.70F + 0.30F * 0.5F);
    assert(std::fabs(*score.value - expected) < 1e-6F);
}

void testQualityBehavior() {
    RiskComponents no_quality{1.0F, 1.0F, 1.0F, 1.0F, std::nullopt};
    assert(calculateRiskScore(no_quality).status == StatusCode::MissingQuality);

    RiskComponents unusable_quality{1.0F, 1.0F, 1.0F, 1.0F, 0.0F};
    const RiskScore score = calculateRiskScore(unusable_quality);
    assert(score.status == StatusCode::Ok);
    assert(score.value.has_value());
    assert(std::fabs(*score.value - 1.0F) < 1e-6F);
}

void testMax30102RawFeatures() {
    Max30102Window input;
    input.start_us = 0;
    input.end_us = 2'000;
    input.samples = {
        {0, 100, 200},
        {1'000, 200, 400},
        {2'000, 300, 600},
    };

    const Max30102Features features = describeMax30102(input);
    assert(features.status == StatusCode::Ok);
    assert(features.sample_count == 3U);
    assert(features.mean_red.has_value());
    assert(features.mean_infrared.has_value());
    assert(std::fabs(*features.mean_red - 200.0F) < 1e-6F);
    assert(std::fabs(*features.mean_infrared - 400.0F) < 1e-6F);
    assert(features.red_stddev.has_value());
    assert(features.infrared_stddev.has_value());
    assert(!features.pulse_bpm.has_value());
    assert(!features.spo2_percent.has_value());
}

void testReplaceableRiskMappers() {
    ImuFeatures imu;
    SlopeEstimate ascent;
    Max30102Features max30102;
    RiskMappers mappers;
    mappers.motion = [](const ImuFeatures&) { return std::optional<float>{0.2F}; };
    mappers.ascent = [](const SlopeEstimate&) { return std::optional<float>{0.4F}; };
    mappers.pulse = [](const Max30102Features&) { return std::optional<float>{0.6F}; };
    mappers.spo2 = [](const Max30102Features&) { return std::optional<float>{0.8F}; };

    const MappedRiskComponents mapped = mapRiskComponents(
        imu, ascent, max30102, 0.5F, mappers);
    assert(mapped.status == StatusCode::Ok);
    assert(mapped.components.motion_m == std::optional<float>{0.2F});
    assert(mapped.components.ascent_a == std::optional<float>{0.4F});
    assert(mapped.components.pulse_h == std::optional<float>{0.6F});
    assert(mapped.components.spo2_o == std::optional<float>{0.8F});
    assert(mapped.components.quality_q == std::optional<float>{0.5F});

    const RiskScore score = calculateRiskScore(mapped.components);
    assert(score.status == StatusCode::Ok);
}

void testImuOrientationContext() {
    ImuWindow input;
    input.start_us = 0;
    input.end_us = 2'000;
    input.samples = {
        {0, 0.0F, 0.0F, 9.81F, 0.0F, 0.0F, 0.0F},
        {1'000, 0.0F, 0.0F, 9.81F, 0.0F, 0.0F, 0.0F},
        {2'000, 0.0F, 0.0F, 9.81F, 0.0F, 0.0F, 0.0F},
    };

    const ImuFeatures features = describeImu(input);
    assert(features.status == StatusCode::Ok);
    assert(features.orientation.valid);
    assert(!features.orientation.yaw_valid);
    assert(std::fabs(features.orientation.roll_rad) < 1e-6F);
    assert(std::fabs(features.orientation.pitch_rad) < 1e-6F);
}

void testQualityFlagsArePreserved() {
    DepthWindow input;
    input.start_us = 0;
    input.end_us = 2'000;
    input.samples = {
        {0, 10.0F},
        {1'000, 0.0F, quality::SensorInvalid},
        {2'000, 8.0F},
    };

    const DepthFilterResult cleaned = cleanDepth(
        input, DepthFilterConfig{1, 3.0F, 1.0F});
    assert(cleaned.status == StatusCode::Ok);
    assert(cleaned.invalid_count == 1U);
    assert(cleaned.samples[1].has_value);
    assert((cleaned.samples[1].quality_flags & quality::SensorInvalid) != 0U);
    assert((cleaned.samples[1].quality_flags & quality::CarriedForward) != 0U);

    ImuWindow imu;
    imu.start_us = 0;
    imu.end_us = 2'000;
    imu.samples = {
        {0, 0.0F, 0.0F, 9.81F, 0.0F, 0.0F, 0.0F},
        {1'000, 0.0F, 0.0F, 9.81F, 0.0F, 0.0F, 0.0F, quality::SensorInvalid},
    };
    const ImuFeatures imu_features = describeImu(imu);
    assert(imu_features.status == StatusCode::Ok);
    assert(imu_features.sample_count == 2U);
    assert(imu_features.valid_sample_count == 1U);
}

void testInvalidSensorProcessingBlocksFinalScore() {
    FusionWindow input;
    input.depth.start_us = 0;
    input.depth.end_us = 0;
    input.imu.start_us = 0;
    input.imu.end_us = 0;
    input.max30102.start_us = 0;
    input.max30102.end_us = 0;

    RiskMappers mappers;
    mappers.motion = [](const ImuFeatures&) { return std::optional<float>{0.0F}; };
    mappers.ascent = [](const SlopeEstimate&) { return std::optional<float>{0.0F}; };
    mappers.pulse = [](const Max30102Features&) { return std::optional<float>{0.0F}; };
    mappers.spo2 = [](const Max30102Features&) { return std::optional<float>{0.0F}; };

    const SensorFusion fusion(DepthFilterConfig{1, 3.0F, 1.0F}, 2U);
    const FusionOutput output = fusion.process(input, mappers, 0.0F);
    assert(output.status == StatusCode::EmptyWindow);
    assert(output.risk.status == StatusCode::EmptyWindow);
    assert(!output.risk.value.has_value());
}

}  // namespace

int main() {
    testDepthCleaningAndSlope();
    testRiskFormula();
    testQualityBehavior();
    testMax30102RawFeatures();
    testReplaceableRiskMappers();
    testImuOrientationContext();
    testQualityFlagsArePreserved();
    testInvalidSensorProcessingBlocksFinalScore();
    return 0;
}
