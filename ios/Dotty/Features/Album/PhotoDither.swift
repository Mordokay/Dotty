import CoreGraphics
import ImageIO
import SwiftUI
import UIKit

/// Turns a photo into what Dotty's e-paper shows: a square crop, 200 x 200, brightness and
/// contrast, then Atkinson dithering to black and white (the same recipe as tools/img2epd.py,
/// which suited photos best on this panel). Dotty stores the result as a binary PBM.
enum PhotoDither {
    static let size = 200
    static let bitmapBytes = size * size / 8  // rows of 25 bytes, MSB first, 1 = black

    /// Brightness 1.15 and contrast 1.4 looked best on the panel for most photos.
    struct Tone: Equatable {
        var brightness = 1.15
        var contrast = 1.4
    }

    /// An upright copy of the picked photo, at most `maxSide` pixels on its long side (the
    /// crop works on this, so editing stays light).
    static func upright(_ image: UIImage, maxSide: CGFloat = 1600) -> CGImage? {
        let scale = min(1, maxSide / max(image.size.width, image.size.height))
        let target = CGSize(width: (image.size.width * scale).rounded(), height: (image.size.height * scale).rounded())
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = true
        return UIGraphicsImageRenderer(size: target, format: format).image { _ in
            image.draw(in: CGRect(origin: .zero, size: target))
        }.cgImage
    }

    /// The `crop` square (in the image's pixels, top-left origin) as 200 x 200 grey levels.
    static func grey(_ image: CGImage, crop: CGRect) -> [Float] {
        var pixels = [UInt8](repeating: 255, count: size * size)
        pixels.withUnsafeMutableBytes { buffer in
            guard let context = CGContext(data: buffer.baseAddress, width: size, height: size, bitsPerComponent: 8,
                                          bytesPerRow: size, space: CGColorSpaceCreateDeviceGray(),
                                          bitmapInfo: CGImageAlphaInfo.none.rawValue) else { return }
            context.interpolationQuality = .high
            let scale = CGFloat(size) / crop.width
            let height = CGFloat(image.height)
            // Core Graphics counts y from the bottom.
            context.draw(image, in: CGRect(x: -crop.minX * scale, y: -(height - crop.maxY) * scale,
                                           width: CGFloat(image.width) * scale, height: height * scale))
        }
        return pixels.map(Float.init)
    }

    /// Grey levels → the 5000-byte bitmap (1 = black).
    static func dither(_ grey: [Float], tone: Tone) -> Data {
        // Like Pillow's ImageEnhance: brightness scales, contrast pulls away from the mean.
        var px = grey.map { $0 * Float(tone.brightness) }
        let mean = px.reduce(0, +) / Float(px.count)
        px = px.map { mean + ($0 - mean) * Float(tone.contrast) }

        // Atkinson: spreads 6/8 of the error, keeping cleaner whites and more contrast.
        var bits = [UInt8](repeating: 0, count: bitmapBytes)
        let neighbours = [(1, 0), (2, 0), (-1, 1), (0, 1), (1, 1), (0, 2)]
        for y in 0..<size {
            for x in 0..<size {
                let old = px[y * size + x]
                let white = old >= 128
                if !white { bits[y * (size / 8) + x / 8] |= UInt8(0x80 >> (x % 8)) }
                let error = (old - (white ? 255 : 0)) / 8
                for (dx, dy) in neighbours {
                    let nx = x + dx, ny = y + dy
                    if nx >= 0, nx < size, ny < size { px[ny * size + nx] += error }
                }
            }
        }
        return Data(bits)
    }

    /// Dotty's file: "P4 200 200" and the bitmap.
    static func pbm(_ bitmap: Data) -> Data {
        Data("P4\n\(size) \(size)\n".utf8) + bitmap
    }

    /// The bitmap as a picture, in e-paper colours (ink on paper), for previews and thumbnails.
    static func picture(_ bitmap: Data) -> UIImage? {
        guard bitmap.count == bitmapBytes else { return nil }
        var rgba = [UInt8](repeating: 0, count: size * size * 4)
        let ink: (UInt8, UInt8, UInt8) = (0x1C, 0x1D, 0x22), paper: (UInt8, UInt8, UInt8) = (0xEC, 0xE9, 0xE0)
        for i in 0..<(size * size) {
            let black = bitmap[i / 8] & UInt8(0x80 >> (i % 8)) != 0
            let c = black ? ink : paper
            rgba[i * 4] = c.0
            rgba[i * 4 + 1] = c.1
            rgba[i * 4 + 2] = c.2
            rgba[i * 4 + 3] = 255
        }
        guard let provider = CGDataProvider(data: Data(rgba) as CFData),
              let image = CGImage(width: size, height: size, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: size * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
                                  provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent)
        else { return nil }
        return UIImage(cgImage: image)
    }

    /// When the photo was taken (EXIF), for its name on Dotty; nil if the file doesn't say.
    static func dateTaken(_ data: Data) -> Date? {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil),
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let exif = properties[kCGImagePropertyExifDictionary] as? [CFString: Any],
              let text = exif[kCGImagePropertyExifDateTimeOriginal] as? String else { return nil }
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyy:MM:dd HH:mm:ss"
        return formatter.date(from: text)
    }

    /// "20240714-193205.pbm": Dotty sorts by name and shows the date from it.
    static func fileName(for date: Date, avoiding taken: Set<String>) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.dateFormat = "yyyyMMdd-HHmmss"
        let base = formatter.string(from: date)
        var name = base + ".pbm"
        var n = 2
        while taken.contains(name) {
            name = "\(base)-\(n).pbm"
            n += 1
        }
        return name
    }
}

/// A Dotty photo as the app shows it, ink on paper. Shrunk (thumbnails), it's smoothed, the
/// way the eye blends the dots on Dotty; enlarged, each dot becomes the same whole number of
/// screen pixels (uneven nearest-neighbour scaling made the preview look worse than Dotty).
struct EpaperImage: View {
    let image: UIImage?
    @Environment(\.displayScale) private var displayScale

    var body: some View {
        GeometryReader { geometry in
            let pixels = geometry.size.width * displayScale
            let size = PhotoDither.size
            let whole = pixels >= CGFloat(size) ? floor(pixels / CGFloat(size)) * CGFloat(size) / displayScale : geometry.size.width
            ZStack {
                Color(red: 0.925, green: 0.914, blue: 0.878)
                if let image {
                    Image(uiImage: image)
                        .resizable()
                        .interpolation(pixels >= CGFloat(size) ? .none : .high)
                        .frame(width: whole, height: whole)
                }
            }
            .frame(width: geometry.size.width, height: geometry.size.height)
        }
        .aspectRatio(1, contentMode: .fit)
    }
}
