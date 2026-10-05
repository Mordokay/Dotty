import SwiftUI

/// One-line song rows with a ≡ handle on the right (where iOS puts it) to drag them into a
/// new order without an Edit mode, and a swipe to the left that reveals Remove. Tapping a
/// row plays it.
struct ReorderableSongList: View {
    let songs: [String]
    let title: (String) -> String
    let isPlaying: (String) -> Bool
    let onPlay: (String) -> Void
    let onMove: (_ from: Int, _ to: Int) -> Void
    let onRemove: (String) -> Void

    private static let rowHeight: CGFloat = 52
    private static let spacing: CGFloat = 6
    private static let pitch = rowHeight + spacing
    private static let removeWidth: CGFloat = 104

    /// The song being dragged by its handle, and how far.
    @State private var dragging: String?
    @State private var dragOffset: CGFloat = 0
    /// The song swiped open to show Remove, and its horizontal offset.
    @State private var swiped: String?
    @State private var swipeOffset: CGFloat = 0
    /// The offset when the current swipe began (open rows start from Remove showing).
    @State private var swipeBase: CGFloat?

    var body: some View {
        VStack(spacing: Self.spacing) {
            ForEach(Array(songs.enumerated()), id: \.element) { index, song in
                row(song, index: index)
                    .offset(y: shift(for: index))
                    .zIndex(dragging == song ? 1 : 0)
            }
        }
        .animation(.settle, value: dropIndex)
        .sensoryFeedback(Haptic.select, trigger: dropIndex)
    }

    // MARK: - Row

    private func row(_ song: String, index: Int) -> some View {
        let open = swiped == song
        return ZStack(alignment: .trailing) {
            // Behind the row: Remove, revealed by swiping left.
            Button(role: .destructive) {
                closeSwipe()
                onRemove(song)
            } label: {
                Label("Remove", systemImage: "minus.circle.fill")
                    .font(.lpCallout.weight(.semibold))
                    .foregroundStyle(Color.onLight)
                    .frame(width: Self.removeWidth, height: Self.rowHeight)
                    .background(RoundedRectangle(cornerRadius: Radius.soft).fill(DottyLight.ember.color))
            }
            .buttonStyle(.plain)
            .opacity(open ? 1 : 0)

            HStack(spacing: Spacing.m) {
                Text(title(song))
                    .font(.lpHeadline)
                    .foregroundStyle(Color.ink)
                    .lineLimit(1)
                    .frame(maxWidth: .infinity, alignment: .leading)
                if isPlaying(song) {
                    Image(systemName: "speaker.wave.2.fill")
                        .font(.system(size: 14, weight: .semibold))
                        .foregroundStyle(DottyLight.firefly.color)
                }
                handle(song, index: index)
            }
            .padding(.leading, Spacing.l)
            .frame(height: Self.rowHeight)
            .glassSurface(cornerRadius: Radius.soft)
            .scaleEffect(dragging == song ? 1.03 : 1)
            .shadow(color: .black.opacity(dragging == song ? 0.35 : 0), radius: 12, y: 6)
            .contentShape(Rectangle())
            .offset(x: open ? swipeOffset : 0)
            .onTapGesture {
                if swiped != nil { closeSwipe() } else { onPlay(song) }
            }
            .simultaneousGesture(swipeGesture(song))
            .contextMenu {
                Button("Play", systemImage: "play") { onPlay(song) }
                Button("Remove from playlist", systemImage: "minus.circle", role: .destructive) { onRemove(song) }
            }
        }
        .frame(height: Self.rowHeight)
    }

    /// The ≡ grip: drag it up or down; the other rows slide out of the way.
    private func handle(_ song: String, index: Int) -> some View {
        Image(systemName: "line.3.horizontal")
            .font(.system(size: 17, weight: .semibold))
            .foregroundStyle(dragging == song ? Color.ink : Color.inkMuted)
            .frame(width: 48, height: Self.rowHeight)
            .contentShape(Rectangle())
            .highPriorityGesture(
                DragGesture(minimumDistance: 0, coordinateSpace: .global)
                    .onChanged { value in
                        if dragging == nil {
                            closeSwipe()
                            dragging = song
                        }
                        dragOffset = value.translation.height
                    }
                    .onEnded { _ in
                        let to = dropIndex ?? index
                        withAnimation(.settle) {
                            dragging = nil
                            dragOffset = 0
                        }
                        if to != index { onMove(index, to) }
                    }
            )
            .accessibilityLabel("Reorder \(title(song))")
    }

    // MARK: - Dragging

    /// Where the dragged song would land.
    private var dropIndex: Int? {
        guard let dragging, let from = songs.firstIndex(of: dragging) else { return nil }
        let moved = from + Int((dragOffset / Self.pitch).rounded())
        return min(max(moved, 0), songs.count - 1)
    }

    /// The dragged row follows the finger; rows between its place and the drop point shift
    /// by one to open a gap.
    private func shift(for index: Int) -> CGFloat {
        guard let dragging, let from = songs.firstIndex(of: dragging), let to = dropIndex else { return 0 }
        if index == from { return dragOffset }
        if from < to, index > from, index <= to { return -Self.pitch }
        if from > to, index >= to, index < from { return Self.pitch }
        return 0
    }

    // MARK: - Swiping

    private func swipeGesture(_ song: String) -> some Gesture {
        DragGesture(minimumDistance: 20)
            .onChanged { value in
                // Only clearly sideways drags; vertical ones scroll the page.
                guard dragging == nil, abs(value.translation.width) > abs(value.translation.height) * 1.5 else { return }
                if swipeBase == nil {
                    swipeBase = swiped == song ? swipeOffset : 0
                    swiped = song
                }
                swipeOffset = min(0, max(-Self.removeWidth * 1.4, (swipeBase ?? 0) + value.translation.width))
            }
            .onEnded { _ in
                swipeBase = nil
                guard swiped == song else { return }
                withAnimation(.settle) {
                    if swipeOffset < -Self.removeWidth / 2 {
                        swipeOffset = -(Self.removeWidth + Spacing.s)
                    } else {
                        swiped = nil
                        swipeOffset = 0
                    }
                }
            }
    }

    private func closeSwipe() {
        withAnimation(.settle) {
            swiped = nil
            swipeOffset = 0
        }
    }
}
