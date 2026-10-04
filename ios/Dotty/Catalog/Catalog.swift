import CoreGraphics
import Foundation
import SwiftUI
import UIKit

/// The published cartridges (tools/build_catalog.py → the latest GitHub Release).
struct Catalog: Decodable, Sendable {
    let format: Int
    let installProtocol: Int
    let generated: String
    let release: String?
    let cartridges: [CatalogCartridge]

    enum CodingKeys: String, CodingKey {
        case format, generated, release, cartridges
        case installProtocol = "protocol"
    }

    static let url = URL(string: "https://github.com/Mordokay/Dotty/releases/latest/download/catalog.json")!

    static func load() async throws -> Catalog {
        var request = URLRequest(url: url)
        request.cachePolicy = .reloadIgnoringLocalCacheData
        let (data, response) = try await URLSession.shared.data(for: request)
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw URLError(.badServerResponse)
        }
        return try JSONDecoder().decode(Catalog.self, from: data)
    }
}

struct CatalogCartridge: Decodable, Identifiable, Sendable {
    let id: String
    let name: String
    let version: String
    let description: String
    let requires: [String]
    let size: Int
    let sha256: String
    let firmware: String
    /// 64×64, 1 bit per pixel, rows MSB-first, set bit = black on e-paper (base64).
    let icon: String?
    /// Full-colour square picture for the app (a URL; older catalogs don't have it).
    let artwork: String?

    var artworkURL: URL? { artwork.flatMap(URL.init(string:)) }
    var sizeText: String { ByteCountFormatter.string(fromByteCount: Int64(size), countStyle: .file) }
}

extension CatalogCartridge {
    /// The e-paper icon as a crisp pixel-art image: e-paper black becomes `ink` on the dark UI.
    func iconImage(ink: Color = .ink) -> Image? {
        guard let icon, let bits = Data(base64Encoded: icon), bits.count == 512 else { return nil }
        let side = 64
        var pixels = [UInt8](repeating: 0, count: side * side * 4)
        let rgb = UIColor(ink).cgColor.components ?? [1, 1, 1, 1]
        for y in 0..<side {
            for x in 0..<side where bits[(y * side + x) / 8] & (0x80 >> (x % 8)) != 0 {
                let i = (y * side + x) * 4
                pixels[i] = UInt8((rgb[0]) * 255)
                pixels[i + 1] = UInt8((rgb.count > 2 ? rgb[1] : rgb[0]) * 255)
                pixels[i + 2] = UInt8((rgb.count > 2 ? rgb[2] : rgb[0]) * 255)
                pixels[i + 3] = 255
            }
        }
        guard let provider = CGDataProvider(data: Data(pixels) as CFData),
              let image = CGImage(width: side, height: side, bitsPerComponent: 8, bitsPerPixel: 32,
                                  bytesPerRow: side * 4, space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                                  provider: provider, decode: nil, shouldInterpolate: false,
                                  intent: .defaultIntent)
        else { return nil }
        return Image(decorative: image, scale: 1).interpolation(.none)
    }
}
