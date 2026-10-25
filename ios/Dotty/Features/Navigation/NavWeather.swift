import CoreLocation
import Foundation

/// The weather where a trip starts, in a few words for the companion's lines (the user: "it'll probably rain,
/// so be quick or bring an umbrella"). From Open-Meteo (free, no key, like the Weather cartridge) until the
/// phone bundle brings WeatherKit.
enum NavWeather {
    /// "14 C, overcast; rain likely within the hour (70%)" (ASCII: Dotty's text stays plain).
    nonisolated static func phrase(temperature: Double, code: Int, rainChance: Int) -> String {
        var s = "\(Int(temperature.rounded())) C, \(words(code))"
        let wet = (51...67).contains(code) || (80...82).contains(code) || code >= 95
        if !wet && rainChance >= 60 { s += "; rain likely within the hour (\(rainChance)%)" }
        else if !wet && rainChance >= 30 { s += "; a chance of rain within the hour (\(rainChance)%)" }
        return s
    }

    /// WMO weather codes.
    nonisolated static func words(_ code: Int) -> String {
        switch code {
        case 0: "clear sky"
        case 1: "mainly clear"
        case 2: "partly cloudy"
        case 3: "overcast"
        case 45, 48: "fog"
        case 51...57: "drizzle"
        case 61...65: "rain"
        case 66, 67: "freezing rain"
        case 71...77: "snow"
        case 80...82: "rain showers"
        case 85, 86: "snow showers"
        case 95...99: "thunderstorm"
        default: "unsettled"
        }
    }

    /// Now and the next hour at `place`; nil when it can't be had quickly (a trip never waits for it).
    nonisolated static func fetch(at place: CLLocationCoordinate2D) async -> String? {
        var url = URLComponents(string: "https://api.open-meteo.com/v1/forecast")!
        url.queryItems = [
            URLQueryItem(name: "latitude", value: String(format: "%.4f", place.latitude)),
            URLQueryItem(name: "longitude", value: String(format: "%.4f", place.longitude)),
            URLQueryItem(name: "current", value: "temperature_2m,weather_code"),
            URLQueryItem(name: "hourly", value: "precipitation_probability"),
            URLQueryItem(name: "forecast_hours", value: "2"),
        ]
        var request = URLRequest(url: url.url!)
        request.timeoutInterval = 3
        guard let (data, _) = try? await URLSession.shared.data(for: request),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let current = json["current"] as? [String: Any],
              let temperature = current["temperature_2m"] as? Double,
              let code = current["weather_code"] as? Int else { return nil }
        let chances = (json["hourly"] as? [String: Any])?["precipitation_probability"] as? [Int] ?? []
        return phrase(temperature: temperature, code: code, rainChance: chances.max() ?? 0)
    }
}
