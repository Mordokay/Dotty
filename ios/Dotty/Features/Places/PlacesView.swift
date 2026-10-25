import MapKit
import SwiftUI

/// Companion › Places: the places the companion can take you to ("walk me home"). An Apple Maps map with
/// our pins, a search field with suggestions as you type, a long press to drop a pin; tap a pin to rename,
/// change its icon, move or delete it (directions spec, "Places").
struct PlacesView: View {
    private let store = PlacesStore.shared
    @State private var search = PlaceSearch()
    @State private var query = ""
    @State private var camera: MapCameraPosition = .userLocation(fallback: .automatic)
    @State private var draft: SavedPlace?        // a new place being saved
    @State private var editing: SavedPlace?      // a saved place being edited
    @State private var moving: SavedPlace?       // the next long press moves it
    @State private var failed = false
    @FocusState private var focused: Bool

    var body: some View {
        ZStack(alignment: .top) {
            MapReader { proxy in
                Map(position: $camera) {
                    UserAnnotation()
                    ForEach(store.places) { place in
                        Annotation(place.name, coordinate: place.coordinate, anchor: .bottom) {
                            PlacePin(icon: place.icon, selected: editing?.id == place.id || moving?.id == place.id)
                                .onTapGesture { editing = place }
                        }
                    }
                    if let draft {
                        Annotation(draft.name, coordinate: draft.coordinate, anchor: .bottom) {
                            PlacePin(icon: draft.icon, selected: true)
                        }
                    }
                }
                .mapStyle(.standard(pointsOfInterest: .including([.cafe, .restaurant, .park, .museum, .publicTransport])))
                .mapControls { MapUserLocationButton() }
                .gesture(
                    LongPressGesture(minimumDuration: 0.5)
                        .sequenced(before: DragGesture(minimumDistance: 0, coordinateSpace: .local))
                        .onEnded { value in
                            guard case .second(true, let drag?) = value,
                                  let coordinate = proxy.convert(drag.location, from: .local) else { return }
                            Haptic.impact(0.6)
                            dropped(at: coordinate)
                        }
                )
            }
            .ignoresSafeArea(edges: .bottom)
            VStack(spacing: Spacing.s) {
                searchField
                if moving != nil {
                    Text("Long-press the new spot for \(moving?.name ?? "")")
                        .font(.lpCallout).foregroundStyle(Color.ink)
                        .padding(Spacing.m).glassSurface(cornerRadius: Radius.soft)
                }
                if failed {
                    Text("Couldn't find that place. Check your internet connection.")
                        .font(.lpCallout).foregroundStyle(DottyLight.ember.color)
                        .padding(Spacing.m).glassSurface(cornerRadius: Radius.soft)
                }
                if focused && !search.results.isEmpty {
                    ScrollView {
                        VStack(spacing: 0) {
                            ForEach(search.results, id: \.self) { result in
                                ChoiceRow(title: result.title, subtitle: result.subtitle.isEmpty ? nil : result.subtitle,
                                          chosen: false) {
                                    focused = false
                                    Task { await pick(result) }
                                } trailing: { EmptyView() }
                            }
                        }
                    }
                    .frame(maxHeight: 300)
                    .glassSurface(cornerRadius: Radius.soft)
                }
                if store.places.isEmpty && !focused && draft == nil {
                    Text("Search for a place or long-press the map, then save it with a name like Home: \"walk me home\" works after that.")
                        .font(.lpCaption).foregroundStyle(Color.inkFaint)
                        .padding(Spacing.m).glassSurface(cornerRadius: Radius.soft)
                }
            }
            .padding(.horizontal, Spacing.l)
            .padding(.top, Spacing.s)
        }
        .navigationTitle("Places")
        .navigationBarTitleDisplayMode(.inline)
        .onChange(of: query) { _, text in search.update(text) }
        .sheet(item: $draft) { place in
            PlaceEditor(place: place, isNew: true) { saved in
                store.save(saved)
                draft = nil
            } onMove: {} onDelete: { draft = nil }
            .presentationDetents([.medium])
        }
        .sheet(item: $editing) { place in
            PlaceEditor(place: place, isNew: false) { saved in
                store.save(saved)
                editing = nil
            } onMove: {
                moving = place
                editing = nil
            } onDelete: {
                store.remove(place.id)
                editing = nil
            }
            .presentationDetents([.medium])
        }
    }

    private var searchField: some View {
        HStack {
            Image(systemName: "magnifyingglass").foregroundStyle(Color.inkFaint)
            TextField("Search Apple Maps", text: $query)
                .font(.lpBody)
                .foregroundStyle(Color.ink)
                .focused($focused)
                .autocorrectionDisabled()
                .submitLabel(.search)
                .onSubmit { if let best = search.results.first { Task { await pick(best) } } }
            if !query.isEmpty {
                Button { query = "" } label: { Image(systemName: "xmark.circle.fill").foregroundStyle(Color.inkFaint) }
            }
        }
        .padding(Spacing.l)
        .glassSurface(cornerRadius: Radius.soft)
    }

    private func dropped(at coordinate: CLLocationCoordinate2D) {
        if var place = moving {
            place.latitude = coordinate.latitude
            place.longitude = coordinate.longitude
            store.save(place)
            moving = nil
            return
        }
        Task {
            let address = await Self.address(of: coordinate)
            draft = SavedPlace(name: "", icon: .pin, latitude: coordinate.latitude, longitude: coordinate.longitude,
                               address: address)
        }
    }

    private func pick(_ result: MKLocalSearchCompletion) async {
        guard let item = try? await MKLocalSearch(request: MKLocalSearch.Request(completion: result)).start().mapItems.first
        else {
            failed = true
            return
        }
        failed = false
        let coordinate = item.location.coordinate
        withAnimation(.settle) {
            camera = .region(MKCoordinateRegion(center: coordinate, latitudinalMeters: 800, longitudinalMeters: 800))
        }
        draft = SavedPlace(name: item.name ?? result.title, icon: .pin, latitude: coordinate.latitude,
                           longitude: coordinate.longitude, address: result.subtitle)
    }

    /// A dropped pin's street, for the save sheet.
    private static func address(of coordinate: CLLocationCoordinate2D) async -> String {
        let location = CLLocation(latitude: coordinate.latitude, longitude: coordinate.longitude)
        guard let request = MKReverseGeocodingRequest(location: location),
              let item = try? await request.mapItems.first else { return "" }
        return item.addressRepresentations?.fullAddress(includingRegion: false, singleLine: true) ?? item.name ?? ""
    }
}

extension SavedPlace {
    var coordinate: CLLocationCoordinate2D { CLLocationCoordinate2D(latitude: latitude, longitude: longitude) }
}

/// Name, icon, move, delete.
private struct PlaceEditor: View {
    @State var place: SavedPlace
    let isNew: Bool
    let onSave: (SavedPlace) -> Void
    let onMove: () -> Void
    let onDelete: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: Spacing.l) {
            Text(isNew ? "Save this place" : "Edit place").font(.lpTitle).foregroundStyle(Color.ink)
            if !place.address.isEmpty {
                Text(place.address).font(.lpCaption).foregroundStyle(Color.inkFaint)
            }
            TextField("Name (Home, Work, Mum's…)", text: $place.name)
                .font(.lpBody).foregroundStyle(Color.ink)
                .padding(Spacing.l)
                .glassSurface(cornerRadius: Radius.soft)
            HStack(spacing: Spacing.s) {
                ForEach(PlaceIcon.allCases, id: \.self) { icon in
                    Button { place.icon = icon } label: {
                        PlacePin(icon: icon, size: 30, selected: place.icon == icon)
                            .opacity(place.icon == icon ? 1 : 0.55)
                    }
                    .buttonStyle(.plain)
                }
            }
            HStack {
                Button(isNew ? "Cancel" : "Delete", role: isNew ? .cancel : .destructive) { onDelete() }
                    .buttonStyle(.quiet(isNew ? DottyLight.firefly.color : DottyLight.ember.color))
                if !isNew {
                    Button("Move") { onMove() }.buttonStyle(.quiet())
                }
                Spacer()
                Button("Save") { onSave(place) }
                    .buttonStyle(.light())
                    .disabled(place.name.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            Spacer(minLength: 0)
        }
        .padding(Spacing.xl)
    }
}
