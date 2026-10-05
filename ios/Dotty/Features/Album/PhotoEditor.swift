import PhotosUI
import SwiftUI

/// Prepares picked photos for Dotty, one at a time: pinch and drag the photo to choose its
/// square, tune brightness and contrast, and see it as Dotty will show it (200 x 200, black
/// and white) while editing. Finished photos go to the outbox, into `album` if given.
struct PhotoEditor: View {
    let items: [PhotosPickerItem]
    let album: String?
    let model: AlbumModel
    /// Called when every photo is done or skipped (with how many were added).
    var onFinish: (Int) -> Void

    @State private var position = 0
    @State private var photo: LoadedPhoto?
    @State private var failed = false
    @State private var added = 0
    @State private var names: Set<String> = []

    struct LoadedPhoto {
        let image: CGImage
        let taken: Date?
    }

    var body: some View {
        VStack(alignment: .leading, spacing: Spacing.l) {
            HStack {
                VStack(alignment: .leading, spacing: 2) {
                    Text(items.count == 1 ? "Frame your photo" : "Photo \(position + 1) of \(items.count)")
                        .font(.lpTitle).foregroundStyle(Color.ink)
                    Text("Pinch and drag to choose the square").font(.lpCaption).foregroundStyle(Color.inkMuted)
                }
                Spacer()
                Button("Cancel") { onFinish(added) }.buttonStyle(.quiet())
            }
            if let photo {
                CropEditor(photo: photo, isLast: position == items.count - 1) { bitmap in
                    if let bitmap { save(bitmap, taken: photo.taken) }
                    next()
                }
                .id(position)  // fresh framing for every photo
            } else if failed {
                NoticeCard(kind: .error, text: "Couldn't open this photo.")
                Button("Skip") { next() }.buttonStyle(.light())
            } else {
                HStack { Spacer(); FireflyLoader(size: 72, label: "Opening photo"); Spacer() }
                    .frame(maxHeight: .infinity)
            }
        }
        .padding(Spacing.xl)
        .glassSurface(cornerRadius: Radius.sheet, frosted: true)
        .padding(Spacing.s)
        .task(id: position) { await load() }
        .onAppear { names = Set(model.photos.map(\.name)).union(model.outbox.queuedNames) }
    }

    private func load() async {
        photo = nil
        failed = false
        guard position < items.count,
              let data = try? await items[position].loadTransferable(type: Data.self),
              let image = UIImage(data: data), let upright = PhotoDither.upright(image) else {
            failed = position < items.count
            return
        }
        photo = LoadedPhoto(image: upright, taken: PhotoDither.dateTaken(data))
    }

    private func save(_ bitmap: Data, taken: Date?) {
        let name = PhotoDither.fileName(for: taken ?? Date(), avoiding: names)
        names.insert(name)
        model.outbox.add(bitmap, name: name, album: album)
        added += 1
    }

    private func next() {
        if position + 1 < items.count {
            position += 1
        } else {
            onFinish(added)
        }
    }
}

/// The framing square over the photo, a live e-paper preview, and the tone sliders.
private struct CropEditor: View {
    let photo: PhotoEditor.LoadedPhoto
    let isLast: Bool
    /// The finished bitmap, or nil to skip this photo.
    var onDone: (Data?) -> Void

    @State private var zoom: CGFloat = 1
    @State private var offset: CGSize = .zero
    @GestureState private var pinch: CGFloat = 1
    @GestureState private var drag: CGSize = .zero
    @State private var brightness = PhotoDither.Tone().brightness
    @State private var contrast = PhotoDither.Tone().contrast
    @State private var preview: UIImage?
    @State private var bitmap: Data?
    @Environment(\.displayScale) private var displayScale

    private var width: CGFloat { CGFloat(photo.image.width) }
    private var height: CGFloat { CGFloat(photo.image.height) }

    var body: some View {
        VStack(alignment: .leading, spacing: Spacing.l) {
            GeometryReader { geometry in
                let side = geometry.size.width
                let frame = framing(side: side, zoom: zoom * pinch, offset: offset + drag)
                Image(decorative: photo.image, scale: 1)
                    .resizable()
                    .frame(width: width * frame.scale, height: height * frame.scale)
                    .offset(frame.offset)
                    .frame(width: side, height: side)
                    .clipped()
                    .overlay { Thirds().stroke(.white.opacity(0.35), lineWidth: 0.5) }
                    .overlay { RoundedRectangle(cornerRadius: 4).strokeBorder(.white.opacity(0.8), lineWidth: 1.5) }
                    .contentShape(Rectangle())
                    .gesture(
                        DragGesture()
                            .updating($drag) { value, state, _ in state = value.translation }
                            .onEnded { value in
                                offset = framing(side: side, zoom: zoom, offset: offset + value.translation).offset
                            }
                            .simultaneously(with: MagnifyGesture()
                                .updating($pinch) { value, state, _ in state = value.magnification }
                                .onEnded { value in
                                    zoom = min(6, max(1, zoom * value.magnification))
                                    offset = framing(side: side, zoom: zoom, offset: offset).offset
                                })
                    )
                    .task(id: Key(zoom: zoom * pinch, offset: offset + drag, side: side, brightness: brightness, contrast: contrast)) {
                        try? await Task.sleep(for: .milliseconds(60))  // let a gesture breathe
                        guard !Task.isCancelled else { return }
                        render(frame.crop)
                    }
            }
            .aspectRatio(1, contentMode: .fit)

            HStack(alignment: .center, spacing: Spacing.l) {
                VStack(spacing: Spacing.xs) {
                    // Exactly 2 screen pixels per Dotty dot.
                    EpaperImage(image: preview)
                        .frame(width: 400 / displayScale, height: 400 / displayScale)
                        .clipShape(RoundedRectangle(cornerRadius: 6))
                    Text("On Dotty").font(.lpLabel).foregroundStyle(Color.inkMuted)
                }
                VStack(spacing: Spacing.s) {
                    LightSlider(title: "Brightness", value: $brightness, range: 0.6...1.8, step: 0.05,
                                format: { String(format: "%.2f", $0) })
                    LightSlider(title: "Contrast", value: $contrast, range: 0.6...2.2, step: 0.05,
                                format: { String(format: "%.2f", $0) })
                }
            }

            HStack {
                Button("Skip") { onDone(nil) }.buttonStyle(.quiet())
                Spacer()
                Button(isLast ? "Send to Dotty" : "Next") { onDone(bitmap) }
                    .buttonStyle(.light())
                    .disabled(bitmap == nil)
            }
        }
    }

    private struct Key: Hashable {
        let zoom: CGFloat, offset: CGSize, side: CGFloat, brightness: Double, contrast: Double
    }

    /// Where the photo sits in the square (scale in points per pixel, offset from centre,
    /// kept so the square is always covered) and the square in the photo's pixels.
    private func framing(side: CGFloat, zoom: CGFloat, offset: CGSize) -> (scale: CGFloat, offset: CGSize, crop: CGRect) {
        let zoom = min(6, max(1, zoom))
        let scale = side / min(width, height) * zoom
        let maxX = (width * scale - side) / 2, maxY = (height * scale - side) / 2
        let clamped = CGSize(width: min(maxX, max(-maxX, offset.width)), height: min(maxY, max(-maxY, offset.height)))
        let cropSide = side / scale
        let centre = CGPoint(x: width / 2 - clamped.width / scale, y: height / 2 - clamped.height / scale)
        return (scale, clamped, CGRect(x: centre.x - cropSide / 2, y: centre.y - cropSide / 2, width: cropSide, height: cropSide))
    }

    private func render(_ crop: CGRect) {
        let grey = PhotoDither.grey(photo.image, crop: crop)
        let bits = PhotoDither.dither(grey, tone: .init(brightness: brightness, contrast: contrast))
        bitmap = bits
        preview = PhotoDither.picture(bits)
    }
}

/// Rule-of-thirds guides.
nonisolated private struct Thirds: Shape {
    func path(in rect: CGRect) -> Path {
        Path { path in
            for f in [1.0 / 3, 2.0 / 3] {
                path.move(to: CGPoint(x: rect.width * f, y: 0))
                path.addLine(to: CGPoint(x: rect.width * f, y: rect.height))
                path.move(to: CGPoint(x: 0, y: rect.height * f))
                path.addLine(to: CGPoint(x: rect.width, y: rect.height * f))
            }
        }
    }
}

private extension CGSize {
    static func + (a: CGSize, b: CGSize) -> CGSize { CGSize(width: a.width + b.width, height: a.height + b.height) }
}
