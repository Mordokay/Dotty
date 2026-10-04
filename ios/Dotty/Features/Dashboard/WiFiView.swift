import SwiftUI

/// Networks Dotty can see (Dotty scans, not the phone) and the ones it remembers. Dotty keeps
/// passwords forever and joins the strongest saved network whenever it needs the internet.
struct WiFiView: View {
    @Environment(DottyLink.self) private var link
    @State private var visible: [Network] = []
    @State private var saved: [String] = []
    @State private var scanning = false
    @State private var error: String?
    @State private var joining: Network?
    @State private var notice: String?

    struct Network: Identifiable, Hashable {
        var id: String { ssid }
        let ssid: String
        let rssi: Int
        let secure: Bool
        let known: Bool
    }

    var body: some View {
        LightField {
            ScrollView {
                VStack(alignment: .leading, spacing: Spacing.xl) {
                    PageHeader(title: "Wi-Fi",
                               subtitle: "Pick a network for Dotty. It remembers every network you add and joins the strongest one nearby.")
                        .padding(.top, Spacing.l)
                    if let notice { NoticeCard(kind: .success, text: notice) }
                    if let error { NoticeCard(kind: .error, text: error) }
                    visibleCard
                    if !saved.isEmpty { savedCard }
                }
                .padding(.horizontal, Spacing.l)
                .padding(.bottom, Spacing.xxl)
            }
            .refreshable { await refresh() }
        }
        .task { await refresh() }
        .onChange(of: link.connection) { _, state in
            if state == .connected { Task { await refresh() } }  // clears a stale "not connected"
        }
        .sheet(item: $joining) { network in
            JoinNetworkSheet(network: network) { name in
                notice = "Dotty joined \(name) and saved it."
                Task { await refresh() }
            }
            .presentationDetents([.medium])
            .presentationBackground(.clear)
        }
    }

    private var visibleCard: some View {
        GlassCard(title: "Networks near Dotty") {
            if scanning && visible.isEmpty {
                HStack { Spacer(); FireflyLoader(size: 80, label: "Dotty is scanning"); Spacer() }
                    .padding(.vertical, Spacing.l)
            } else if visible.isEmpty {
                LightRow(title: "No networks found", subtitle: "Pull down to scan again", systemImage: "wifi.slash")
            }
            ForEach(visible) { network in
                LightRow(title: network.ssid,
                         subtitle: network.known ? "Saved" : (network.secure ? "Needs a password" : "Open"),
                         systemImage: network.known ? "checkmark.circle" : (network.secure ? "lock" : "wifi"),
                         action: { joining = network }) {
                    SignalStrength(rssi: network.rssi)
                }
            }
        }
    }

    private var savedCard: some View {
        GlassCard(title: "Saved on Dotty") {
            ForEach(saved, id: \.self) { name in
                LightRow(title: name, systemImage: "wifi") {
                    Button("Remove") { Task { await remove(name) } }
                        .buttonStyle(.quiet(DottyLight.ember.color))
                }
            }
        }
    }

    private func refresh() async {
        if let info = link.info, !info.commands.contains("wifi.scan") {
            error = "\(info.name) \(info.version) is too old to set up Wi-Fi. Update it under Cartridges first."
            return
        }
        scanning = true
        error = nil
        defer { scanning = false }
        do {
            let reply = try await link.send("wifi.scan", timeout: 30)
            visible = (reply["networks"] as? [[String: Any]] ?? []).compactMap { item in
                guard let ssid = item["ssid"] as? String else { return nil }
                return Network(ssid: ssid, rssi: item["rssi"] as? Int ?? -100,
                               secure: item["secure"] as? Bool ?? true, known: item["known"] as? Bool ?? false)
            }
            saved = try await link.send("wifi.list")["networks"] as? [String] ?? []
        } catch {
            self.error = link.connection == .connected ? error.localizedDescription
                                                       : "Dotty isn't connected. Press PWR on Dotty to wake it."
        }
    }

    private func remove(_ name: String) async {
        do {
            try await link.send("wifi.remove", ["ssid": name])
            await refresh()
        } catch {
            self.error = error.localizedDescription
        }
    }
}

/// Password for one network; Dotty joins it to check before saving.
struct JoinNetworkSheet: View {
    let network: WiFiView.Network
    var onJoined: (String) -> Void

    @Environment(DottyLink.self) private var link
    @Environment(\.dismiss) private var dismiss
    @State private var password = ""
    @State private var working = false
    @State private var error: String?
    @FocusState private var focused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: Spacing.l) {
            Text(network.ssid).font(.lpTitle).foregroundStyle(Color.ink)
            if network.secure {
                SecureField("Password", text: $password)
                    .font(.lpBody)
                    .foregroundStyle(Color.ink)
                    .textContentType(.password)
                    .focused($focused)
                    .padding(Spacing.l)
                    .glassSurface(cornerRadius: Radius.soft)
                    .submitLabel(.join)
                    .onSubmit { Task { await join() } }
            }
            if let error {
                Text(error).font(.lpCallout).foregroundStyle(DottyLight.ember.color)
            }
            HStack(spacing: Spacing.m) {
                Button(working ? "Joining…" : "Join") { Task { await join() } }
                    .buttonStyle(.light())
                    .disabled(working || (network.secure && password.isEmpty))
                Button("Cancel") { dismiss() }.buttonStyle(.quiet())
            }
            if working {
                Text("Dotty is trying the password…").font(.lpCaption).foregroundStyle(Color.inkMuted)
            }
            Spacer(minLength: 0)
        }
        .padding(Spacing.xl)
        .glassSurface(cornerRadius: Radius.sheet, frosted: true)
        .padding(Spacing.s)
        .onAppear { focused = network.secure }
        .sensoryFeedback(.error, trigger: error)
    }

    private func join() async {
        working = true
        error = nil
        defer { working = false }
        do {
            try await link.send("wifi.add", ["ssid": network.ssid, "password": password], timeout: 45)
            onJoined(network.ssid)
            dismiss()
        } catch {
            self.error = error.localizedDescription
        }
    }
}
