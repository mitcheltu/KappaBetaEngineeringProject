import Foundation

public enum HistoryCSV {
    public static let columns = ["entry", "device_id", "boot_session", "sequence", "end_exclusive", "acquisition_us", "kind", "flags",
        "pressure_pa", "depth_m", "temperature_c", "surface_pa", "accel_x_g", "accel_y_g", "accel_z_g",
        "gyro_x_dps", "gyro_y_dps", "gyro_z_dps", "red", "infrared", "heart_rate_bpm", "spo2_percent",
        "battery_v", "pump_enabled", "pump_on", "vibration_on", "countdown_active", "remaining_ms",
        "cancel_pressed", "sos_pressed", "ppg_overflows", "sensor_errors", "estimated_sample_time", "imported_at"]

    public static func row(session: HistorySession, entry: HistoryEntry) throws -> String {
        var values: [String: String] = [
            "entry": entry.isGap ? "gap" : "record", "device_id": session.deviceId,
            "boot_session": session.bootId, "sequence": String(entry.sequence), "end_exclusive": String(entry.end),
            "imported_at": entry.importedAt.ISO8601Format()
        ]
        func set<T>(_ key: String, _ value: T?) { if let value { values[key] = String(describing: value) } }
        if let record = entry.record {
            values["acquisition_us"] = String(record.timeUs)
            values["kind"] = String(record.kind); values["flags"] = String(record.flags)
            let bytes = Bytes(record.raw)
            let baseline = Double(bytes.f32(record.kind == 5 ? 16 : 28))
            if record.kind == 5 {
                set("surface_pa", record.flags & 1 != 0 && baseline.isFinite ? baseline : nil)
            } else {
                var live = Data([1, record.flags, 0, 0, 0, 0, 0, 0])
                live.append(record.raw.subdata(in: 16..<28))
                let kind: TelemetryKind = [.depth, .imu, .ppg, .system][Int(record.kind) - 1]
                switch try TelemetryPacket.decode(live, kind: kind) {
                case .depth(let d):
                    set("pressure_pa", d.pressurePa); set("depth_m", d.depthM); set("temperature_c", d.temperatureC)
                    set("surface_pa", baseline.isFinite ? baseline : nil)
                case .imu(let d):
                    for (i, axis) in ["x", "y", "z"].enumerated() {
                        set("accel_\(axis)_g", d.accelerationG?[i]); set("gyro_\(axis)_dps", d.angularVelocityDps?[i])
                    }
                case .ppg(let d):
                    set("red", d.red); set("infrared", d.infrared)
                    set("heart_rate_bpm", d.heartRateBpm); set("spo2_percent", d.spo2Percent)
                    set("estimated_sample_time", record.flags & 128 != 0)
                case .system(let d):
                    set("battery_v", d.batteryV); set("pump_enabled", d.pumpEnabled); set("pump_on", d.pumpOn)
                    set("vibration_on", d.vibrationOn); set("countdown_active", d.countdownActive)
                    set("remaining_ms", d.remainingMs); set("cancel_pressed", d.cancelPressed); set("sos_pressed", d.sosPressed)
                    set("ppg_overflows", d.ppgOverflowCount); set("sensor_errors", d.sensorErrors)
                }
            }
        }
        return columns.map { escape(values[$0] ?? "") }.joined(separator: ",") + "\r\n"
    }

    private static func escape(_ value: String) -> String {
        value.contains(where: { $0 == "," || $0 == "\"" || $0 == "\r" || $0 == "\n" })
            ? "\"" + value.replacingOccurrences(of: "\"", with: "\"\"") + "\"" : value
    }
}
