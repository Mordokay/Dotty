import SwiftUI

/// A photo as a companion's avatar: frame it in a circle (drag, pinch), set brightness and contrast
/// with live previews of what Dotty will show at 128 and 48 px, then send both dithered circles
/// (companion.photo, base64 pieces; each command ≤ 512 bytes). A photo has no faces: Dotty adds a
/// small mood badge instead.
struct AvatarPhotoEditor: View {
    let companion: String
    let image: CGImage
    let onDone: (Bool) -> Void
    @Environment(DottyLink.self) private var link
    @Environment(\.dismiss) private var dismiss
    @State private var scale: CGFloat = 1
    @State private var pinchBase: CGFloat = 1
    @State private var offset: CGSize = .zero
    @State private var dragBase: CGSize = .zero
    @State private var tone = PhotoDither.Tone()
    @State private var preview128: UIImage?
    @State private var preview48: UIImage?
    @State private var progress: Double?
    @State private var error: String?

    private let viewport: CGFloat = 280

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: Spacing.l) {
                    frameView
                    HStack(spacing: Spacing.l) {
                        previewImage(preview128, size: 128)
                        previewImage(preview48, size: 48)
                    }
                    VStack(alignment: .leading, spacing: Spacing.s) {
                        Text("Brightness").font(.lpLabel).foregroundStyle(Color.inkMuted)
                        Slider(value: $tone.brightness, in: 0.6...1.8)
                        Text("Contrast").font(.lpLabel).foregroundStyle(Color.inkMuted)
                        Slider(value: $tone.contrast, in: 0.6...2.2)
                    }
                    if let progress {
                        ProgressView(value: progress) { Text("Sending to Dotty…").font(.lpCaption) }
                    }
                    if let error { NoticeCard(kind: .error, text: error) }
                }
                .padding(Spacing.l)
            }
            .navigationTitle("Photo avatar")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancel") { dismiss() } }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Use") { Task { await upload() } }.disabled(progress != nil)
                }
            }
        }
        .onAppear(perform: render)
        .onChange(of: tone) { _, _ in render() }
    }

    // MARK: - Framing

    private var fill: CGFloat { max(viewport / CGFloat(image.width), viewport / CGFloat(image.height)) }

    private var frameView: some View {
        let k = fill * scale
        return Image(decorative: image, scale: 1)
            .resizable()
            .frame(width: CGFloat(image.width) * k, height: CGFloat(image.height) * k)
            .offset(offset)
            .frame(width: viewport, height: viewport)
            .clipped()
            .overlay(Circle().strokeBorder(Color.white, lineWidth: 3))
            .overlay(
                Rectangle().fill(Color.black.opacity(0.45))
                    .mask(Rectangle().overlay(Circle().blendMode(.destinationOut)).compositingGroup())
            )
            .contentShape(Rectangle())
            .gesture(DragGesture()
                .onChanged { v in offset = CGSize(width: dragBase.width + v.translation.width, height: dragBase.height + v.translation.height) }
                .onEnded { _ in dragBase = offset; render() })
            .simultaneousGesture(MagnifyGesture()
                .onChanged { v in scale = max(1, min(6, pinchBase * v.magnification)) }
                .onEnded { _ in pinchBase = scale; render() })
    }

    /// The circle's square in the image's pixels (top-left origin).
    private var crop: CGRect {
        let k = fill * scale
        let side = viewport / k
        let x = CGFloat(image.width) / 2 - (viewport / 2 + offset.width) / k
        let y = CGFloat(image.height) / 2 - (viewport / 2 + offset.height) / k
        return CGRect(x: x, y: y, width: side, height: side)
    }

    // MARK: - Dotty's circles

    /// A dithered circle: nothing outside it, a 2 px black ring (MSB-first rows, 1 = black).
    private func circle(side: Int) -> Data {
        var bits = [UInt8](PhotoDither.dither(PhotoDither.grey(image, crop: crop, side: side), tone: tone, side: side))
        let r = Double(side) / 2
        for y in 0..<side {
            for x in 0..<side {
                let d = ((Double(x) + 0.5 - r) * (Double(x) + 0.5 - r) + (Double(y) + 0.5 - r) * (Double(y) + 0.5 - r)).squareRoot()
                let i = y * (side / 8) + x / 8, bit = UInt8(0x80 >> (x % 8))
                if d > r { bits[i] &= ~bit } else if d > r - 2 { bits[i] |= bit }
            }
        }
        return Data(bits)
    }

    private func render() {
        preview128 = PhotoDither.picture(circle(side: 128), side: 128)
        preview48 = PhotoDither.picture(circle(side: 48), side: 48)
    }

    private func previewImage(_ image: UIImage?, size: CGFloat) -> some View {
        VStack(spacing: 4) {
            Group {
                if let image { Image(uiImage: image).interpolation(.none).resizable() } else { Color.glassStrong }
            }
            .frame(width: size, height: size)
            Text("\(Int(size)) px").font(.lpCaption).foregroundStyle(Color.inkFaint)
        }
    }

    /// Kept in the photo library (any companion can wear it later), then sent to this one.
    private func upload() async {
        let big = circle(side: 128), small = circle(side: 48)
        progress = 0
        error = nil
        let photo = AvatarPhotos.add(preview: PhotoDither.picture(big, side: 128) ?? UIImage(), big: big, small: small)
        do {
            try await AvatarPhotos.send(link, companion: companion, big: big, small: small) { progress = $0 }
            AvatarPhotos.setPhoto(photo, for: companion)
            onDone(true)
            dismiss()
        } catch {
            progress = nil
            if !link.lostConnection(error) { self.error = error.localizedDescription }
        }
    }
}
