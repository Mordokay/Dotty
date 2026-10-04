import CoreBluetooth
import Foundation
import Observation

/// The app's Bluetooth link to Dotty: scanning, pairing, staying connected and commands.
///
/// Pairing is the operating-system kind (bonding): the first encrypted command makes iOS ask
/// for the code Dotty shows on its screen, and Dotty then appears in the iPhone's Bluetooth
/// settings. Once paired, the link keeps a pending connection to that Dotty, so it reconnects
/// on its own whenever Dotty is in range and advertising (unlocked, or after a cartridge
/// switch restarts it).
@Observable
final class DottyLink: NSObject {
    enum Radio: Equatable { case unknown, ready, off, unauthorized, unsupported }
    enum Connection: Equatable { case idle, connecting, connected }

    struct Nearby: Identifiable, Equatable {
        let id: UUID
        var name: String
        var serial: String?
        var rssi: Int
    }

    private(set) var radio: Radio = .unknown
    private(set) var connection: Connection = .idle
    private(set) var isScanning = false
    private(set) var nearby: [Nearby] = []
    private(set) var info: DottyInfo?
    private(set) var paired: PairedDotty? = PairedDotty.load()
    /// The latest event that wasn't a reply (e.g. fetch.progress).
    private(set) var lastEvent: DottyMessage?

    @ObservationIgnored private var central: CBCentralManager!
    @ObservationIgnored private var peripheral: CBPeripheral?
    @ObservationIgnored private var discovered: [UUID: CBPeripheral] = [:]
    @ObservationIgnored private var infoCharacteristic: CBCharacteristic?
    @ObservationIgnored private var commandCharacteristic: CBCharacteristic?
    @ObservationIgnored private var eventCharacteristic: CBCharacteristic?
    @ObservationIgnored private var subscribed = false
    @ObservationIgnored private var wantsScan = false

    private struct ReplyWaiter {
        let id = UUID()
        let command: String
        let continuation: CheckedContinuation<DottyMessage, Error>
    }
    @ObservationIgnored private var replyWaiters: [ReplyWaiter] = []
    @ObservationIgnored private var readyWaiters: [CheckedContinuation<Void, Error>] = []

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: .main,
                                   options: [CBCentralManagerOptionRestoreIdentifierKey: "dotty.central"])
    }

    // MARK: - Scanning and pairing

    func startScanning() {
        wantsScan = true
        guard radio == .ready, !isScanning else { return }
        nearby = []
        central.scanForPeripherals(withServices: [DottyUUID.service],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        isScanning = true
    }

    func stopScanning() {
        wantsScan = false
        central.stopScan()
        isScanning = false
    }

    /// Connects to a Dotty found while scanning and pairs with it. iOS shows the code prompt;
    /// the code is on Dotty's screen. Call `remember(_:)` with the result to keep it.
    func pair(with dotty: Nearby) async throws -> PairedDotty {
        guard let target = discovered[dotty.id] else { throw DottyError.notConnected }
        stopScanning()
        try await connect(target)
        do {
            // The first encrypted command triggers the pairing prompt; give the person time.
            _ = try await send("core.ping", timeout: 120)
        } catch {
            disconnect()
            throw DottyError.pairingFailed
        }
        return PairedDotty(identifier: target.identifier, name: dotty.name, serial: dotty.serial ?? info?.serial)
    }

    /// Keeps a paired Dotty: from now on the app reconnects to it by itself.
    func remember(_ dotty: PairedDotty) {
        dotty.save()
        paired = dotty
    }

    /// Reconnects to the paired Dotty (called whenever the app comes to the foreground).
    /// The connection request stays pending until Dotty advertises.
    func reconnect() {
        guard let paired, radio == .ready, connection == .idle else { return }
        guard let known = central.retrievePeripherals(withIdentifiers: [paired.identifier]).first else { return }
        attach(known)
        connection = .connecting
        central.connect(known)
    }

    func disconnect() {
        if let peripheral { central.cancelPeripheralConnection(peripheral) }
    }

    /// Unpairs: Dotty forgets this phone, and the app forgets Dotty. The person should also
    /// remove Dotty under Settings › Bluetooth.
    func forget() async {
        _ = try? await send("core.forget")
        PairedDotty.clear()
        paired = nil
        disconnect()
        info = nil
    }

    // MARK: - Commands

    /// Sends a command and waits for its reply. Throws `.refused` when Dotty answers not ok.
    @discardableResult
    func send(_ command: String, _ arguments: [String: Any] = [:], timeout: Double = 15) async throws -> DottyMessage {
        guard let peripheral, let commandCharacteristic, connection == .connected else { throw DottyError.notConnected }
        var body = arguments
        body["cmd"] = command
        let data = try JSONSerialization.data(withJSONObject: body)
        let reply: DottyMessage = try await withCheckedThrowingContinuation { continuation in
            let waiter = ReplyWaiter(command: command, continuation: continuation)
            replyWaiters.append(waiter)
            peripheral.writeValue(data, for: commandCharacteristic, type: .withResponse)
            Task { [weak self] in
                try? await Task.sleep(for: .seconds(timeout))
                self?.finishWaiter(id: waiter.id, with: .failure(DottyError.timeout))
            }
        }
        if !reply.ok { throw DottyError.refused(reply.error ?? "Dotty said no.") }
        return reply
    }

    func refreshInfo() {
        if let peripheral, let infoCharacteristic { peripheral.readValue(for: infoCharacteristic) }
    }

    /// Makes sure the launcher is running (installs go through it), switching if needed.
    func ensureLauncher() async throws {
        if info?.isLauncher == true { return }
        try await send("core.toLauncher")
        try await waitUntil(timeout: 45) { $0.connection == .connected && $0.info?.isLauncher == true }
    }

    /// Waits for the next connection after Dotty restarts (e.g. into a new cartridge).
    func waitForReconnect(timeout: Double = 60) async throws {
        try await waitUntil(timeout: 5) { $0.connection != .connected }
        try await waitUntil(timeout: timeout) { $0.connection == .connected }
    }

    // MARK: - Internals

    private func connect(_ target: CBPeripheral) async throws {
        attach(target)
        connection = .connecting
        central.connect(target)
        try await withCheckedThrowingContinuation { continuation in
            readyWaiters.append(continuation)
            Task { [weak self] in
                try? await Task.sleep(for: .seconds(20))
                guard let self, self.connection != .connected else { return }
                self.central.cancelPeripheralConnection(target)
                self.failReadyWaiters(DottyError.timeout)
            }
        }
    }

    private func attach(_ target: CBPeripheral) {
        peripheral = target
        target.delegate = self
    }

    private func waitUntil(timeout: Double, _ condition: (DottyLink) -> Bool) async throws {
        let deadline = Date().addingTimeInterval(timeout)
        while !condition(self) {
            if Date() > deadline { throw DottyError.timeout }
            try await Task.sleep(for: .milliseconds(250))
        }
    }

    private func finishWaiter(id: UUID, with result: Result<DottyMessage, Error>) {
        guard let index = replyWaiters.firstIndex(where: { $0.id == id }) else { return }
        replyWaiters.remove(at: index).continuation.resume(with: result)
    }

    private func failAllReplies(_ error: Error) {
        let waiters = replyWaiters
        replyWaiters = []
        waiters.forEach { $0.continuation.resume(throwing: error) }
    }

    private func failReadyWaiters(_ error: Error) {
        let waiters = readyWaiters
        readyWaiters = []
        waiters.forEach { $0.resume(throwing: error) }
    }

    private func becameReadyIfComplete() {
        guard connection != .connected, info != nil, subscribed, commandCharacteristic != nil else { return }
        connection = .connected
        let waiters = readyWaiters
        readyWaiters = []
        waiters.forEach { $0.resume() }
    }

    private func handle(_ message: DottyMessage) {
        if let command = message.command, let waiter = replyWaiters.first(where: { $0.command == command }) {
            finishWaiter(id: waiter.id, with: .success(message))
        } else {
            lastEvent = message
        }
    }

    /// "70:04:1D:D7:B1:00" from manufacturer data 0xFFFF + 6 bytes.
    private static func serial(from advertisement: [String: Any]) -> String? {
        guard let data = advertisement[CBAdvertisementDataManufacturerDataKey] as? Data, data.count == 8,
              data[0] == 0xFF, data[1] == 0xFF else { return nil }
        return data.dropFirst(2).map { String(format: "%02X", $0) }.joined(separator: ":")
    }
}

// MARK: - CoreBluetooth delegates (called on the main queue)

extension DottyLink: @preconcurrency CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn: radio = .ready
        case .poweredOff: radio = .off
        case .unauthorized: radio = .unauthorized
        case .unsupported: radio = .unsupported
        default: radio = .unknown
        }
        guard radio == .ready else {
            isScanning = false
            connection = .idle
            return
        }
        if wantsScan { startScanning() }
        reconnect()
    }

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        if let restored = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first {
            attach(restored)
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        discovered[peripheral.identifier] = peripheral
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String ?? peripheral.name ?? "Dotty"
        let found = Nearby(id: peripheral.identifier, name: name, serial: Self.serial(from: advertisementData),
                           rssi: RSSI.intValue)
        if let index = nearby.firstIndex(where: { $0.id == found.id }) {
            nearby[index].rssi = found.rssi
            nearby[index].name = found.name
            if found.serial != nil { nearby[index].serial = found.serial }
        } else {
            nearby.append(found)
        }
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        subscribed = false
        peripheral.discoverServices([DottyUUID.service])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        connection = .idle
        failReadyWaiters(error ?? DottyError.notConnected)
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        connection = .idle
        infoCharacteristic = nil
        commandCharacteristic = nil
        eventCharacteristic = nil
        subscribed = false
        failAllReplies(DottyError.notConnected)
        failReadyWaiters(DottyError.notConnected)
        // Paired: keep a connection request pending, so Dotty reconnects by itself.
        if let paired, peripheral.identifier == paired.identifier, radio == .ready {
            connection = .connecting
            central.connect(peripheral)
        }
    }
}

extension DottyLink: @preconcurrency CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == DottyUUID.service }) else {
            central.cancelPeripheralConnection(peripheral)
            return
        }
        peripheral.discoverCharacteristics([DottyUUID.info, DottyUUID.command, DottyUUID.event, DottyUUID.data],
                                           for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for characteristic in service.characteristics ?? [] {
            switch characteristic.uuid {
            case DottyUUID.info: infoCharacteristic = characteristic
            case DottyUUID.command: commandCharacteristic = characteristic
            case DottyUUID.event: eventCharacteristic = characteristic
            default: break
            }
        }
        if let eventCharacteristic { peripheral.setNotifyValue(true, for: eventCharacteristic) }
        refreshInfo()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic,
                    error: Error?) {
        guard characteristic.uuid == DottyUUID.event else { return }
        if let error {
            failReadyWaiters(error)
            return
        }
        subscribed = true
        becameReadyIfComplete()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard error == nil, let value = characteristic.value else { return }
        switch characteristic.uuid {
        case DottyUUID.info:
            info = try? JSONDecoder().decode(DottyInfo.self, from: value)
            becameReadyIfComplete()
        case DottyUUID.event:
            handle(DottyMessage(raw: value))
        default:
            break
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        // A refused write (e.g. pairing cancelled) fails the oldest waiting command.
        if let error, let waiter = replyWaiters.first {
            finishWaiter(id: waiter.id, with: .failure(error))
        }
    }
}
