import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

void main() {
  runApp(const AudioBridgeApp());
}

class AudioBridgeApp extends StatelessWidget {
  const AudioBridgeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'AudioBridge',
      theme: ThemeData(
        colorSchemeSeed: Colors.deepPurple,
        useMaterial3: true,
        brightness: Brightness.dark,
      ),
      home: const AudioBridgePage(),
    );
  }
}

enum ConnectionState_ {
  disconnected,
  discovering,
  connecting,
  connected,
}

class AudioBridgePage extends StatefulWidget {
  const AudioBridgePage({super.key});

  @override
  State<AudioBridgePage> createState() => _AudioBridgePageState();
}

class _AudioBridgePageState extends State<AudioBridgePage> {
  static const _channel = MethodChannel('com.audiobridge/audio');

  ConnectionState_ _state = ConnectionState_.disconnected;
  String _serverName = '';
  String _serverAddress = '';
  double _latencyMs = 0.0;
  String _statusMessage = 'Tap Connect to find a server';

  RawDatagramSocket? _discoverySocket;
  RawDatagramSocket? _audioSocket;
  Timer? _discoveryTimer;
  Timer? _keepaliveTimer;
  Timer? _timeoutTimer;
  Timer? _latencyTimer;
  DateTime? _lastPacketTime;

  @override
  void dispose() {
    _disconnect();
    super.dispose();
  }

  Future<void> _startDiscovery() async {
    setState(() {
      _state = ConnectionState_.discovering;
      _statusMessage = 'Searching for server...';
    });

    try {
      _discoverySocket = await RawDatagramSocket.bind(
        InternetAddress.anyIPv4,
        0,
        reuseAddress: true,
      );
      _discoverySocket!.broadcastEnabled = true;

      _discoverySocket!.listen((event) {
        if (event == RawSocketEvent.read) {
          final datagram = _discoverySocket!.receive();
          if (datagram != null) {
            final msg = String.fromCharCodes(datagram.data);
            if (msg.startsWith('AB_OFFER|')) {
              final parts = msg.split('|');
              if (parts.length >= 3) {
                _serverName = parts[1];
                final port = int.tryParse(parts[2]) ?? 4012;
                _serverAddress = datagram.address.address;
                _discoveryTimer?.cancel();
                _connectToServer(datagram.address, port);
              }
            }
          }
        }
      });

      // Send discovery broadcast every 2 seconds
      _sendDiscovery();
      _discoveryTimer = Timer.periodic(
        const Duration(seconds: 2),
        (_) => _sendDiscovery(),
      );

      // Timeout after 30 seconds
      _timeoutTimer = Timer(const Duration(seconds: 30), () {
        if (_state == ConnectionState_.discovering) {
          setState(() {
            _statusMessage = 'No server found. Try again.';
          });
          _stopDiscovery();
          setState(() => _state = ConnectionState_.disconnected);
        }
      });
    } catch (e) {
      setState(() {
        _state = ConnectionState_.disconnected;
        _statusMessage = 'Discovery error: $e';
      });
    }
  }

  void _sendDiscovery() {
    final data = Uint8List.fromList('AB_DISCOVER'.codeUnits);
    _discoverySocket?.send(
      data,
      InternetAddress('255.255.255.255'),
      4011,
    );
  }

  void _stopDiscovery() {
    _discoveryTimer?.cancel();
    _discoveryTimer = null;
    _timeoutTimer?.cancel();
    _timeoutTimer = null;
    _discoverySocket?.close();
    _discoverySocket = null;
  }

  Future<void> _connectToServer(InternetAddress address, int port) async {
    setState(() {
      _state = ConnectionState_.connecting;
      _statusMessage = 'Connecting to $_serverName...';
    });

    _stopDiscovery();

    try {
      // Bind audio socket
      _audioSocket = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0);

      // Listen for packets (must set up listener before sending connect)
      final completer = Completer<bool>();
      _audioSocket!.listen((event) {
        if (event == RawSocketEvent.read) {
          final datagram = _audioSocket!.receive();
          if (datagram != null) {
            _lastPacketTime = DateTime.now();
            final msg = String.fromCharCodes(datagram.data);
            if (msg == 'AB_ACCEPT' && !completer.isCompleted) {
              completer.complete(true);
              return;
            }
            _handlePacket(datagram.data);
          }
        }
      });

      // Send connect message
      final connectMsg = Uint8List.fromList('AB_CONNECT|AudioBridge'.codeUnits);
      _audioSocket!.send(connectMsg, address, port);

      // Wait for AB_ACCEPT with timeout
      final accepted = await completer.future.timeout(
        const Duration(seconds: 5),
        onTimeout: () => false,
      );

      if (!accepted) {
        setState(() {
          _state = ConnectionState_.disconnected;
          _statusMessage = 'Server did not accept connection';
        });
        _audioSocket?.close();
        _audioSocket = null;
        return;
      }

      // Start native audio pipeline
      await _channel.invokeMethod('startAudio');

      setState(() {
        _state = ConnectionState_.connected;
        _statusMessage = 'Connected to $_serverName';
      });

      // Start keepalive timer (send every 1000ms)
      _keepaliveTimer = Timer.periodic(const Duration(milliseconds: 1000), (_) {
        _sendKeepalive(address, port);
      });

      // Latency update timer
      _latencyTimer = Timer.periodic(const Duration(milliseconds: 500), (_) async {
        try {
          final latency = await _channel.invokeMethod<double>('getLatency');
          if (latency != null && mounted) {
            setState(() => _latencyMs = latency);
          }
        } catch (_) {}
      });

      // Timeout detection
      _timeoutTimer = Timer.periodic(const Duration(seconds: 1), (_) {
        if (_lastPacketTime != null &&
            DateTime.now().difference(_lastPacketTime!).inSeconds > 5) {
          _disconnect();
          if (mounted) {
            setState(() {
              _statusMessage = 'Server disconnected (timeout)';
            });
          }
        }
      });
    } catch (e) {
      setState(() {
        _state = ConnectionState_.disconnected;
        _statusMessage = 'Connection error: $e';
      });
    }
  }

  void _handlePacket(Uint8List data) {
    if (data.length < 16) return;

    final version = data[0];
    final type = data[1];

    if (version != 0x01) return;

    if (type == 0x01) {
      // Audio packet - send to native
      _channel.invokeMethod('feedAudio', data);
    } else if (type == 0x03) {
      // Control
      if (data.length >= 17 && data[16] == 0x03) {
        _disconnect();
        if (mounted) {
          setState(() => _statusMessage = 'Server disconnected');
        }
      }
    }
    // Keepalive (0x02) - just updates _lastPacketTime above
  }

  void _sendKeepalive(InternetAddress address, int port) {
    // Build keepalive packet: version(1) + type(1) + seq(4) + timestamp(8) + len(2)
    final packet = Uint8List(16);
    final view = ByteData.view(packet.buffer);
    packet[0] = 0x01; // version
    packet[1] = 0x02; // keepalive
    view.setUint32(2, 0, Endian.little); // seq
    view.setUint64(6, 0, Endian.little); // timestamp
    view.setUint16(14, 0, Endian.little); // payload length = 0
    _audioSocket?.send(packet, address, port);
  }

  Future<void> _disconnect() async {
    _keepaliveTimer?.cancel();
    _keepaliveTimer = null;
    _latencyTimer?.cancel();
    _latencyTimer = null;
    _timeoutTimer?.cancel();
    _timeoutTimer = null;
    _stopDiscovery();

    try {
      await _channel.invokeMethod('stopAudio');
    } catch (_) {}

    // Send AB_DISCONNECT before closing
    if (_audioSocket != null && _serverAddress.isNotEmpty) {
      try {
        final disconnectMsg = Uint8List.fromList('AB_DISCONNECT'.codeUnits);
        _audioSocket!.send(
          disconnectMsg,
          InternetAddress(_serverAddress),
          4012,
        );
      } catch (_) {}
    }

    _audioSocket?.close();
    _audioSocket = null;

    if (mounted) {
      setState(() {
        _state = ConnectionState_.disconnected;
        _latencyMs = 0;
        _statusMessage = 'Tap Connect to find a server';
      });
    }
  }

  void _onConnectPressed() {
    if (_state == ConnectionState_.disconnected) {
      _startDiscovery();
    } else {
      _disconnect();
    }
  }

  @override
  Widget build(BuildContext context) {
    final isConnected = _state == ConnectionState_.connected;
    final isWorking = _state == ConnectionState_.discovering ||
        _state == ConnectionState_.connecting;

    return Scaffold(
      appBar: AppBar(
        title: const Text('AudioBridge'),
        centerTitle: true,
      ),
      body: Center(
        child: Padding(
          padding: const EdgeInsets.all(24.0),
          child: Column(
            mainAxisAlignment: MainAxisAlignment.center,
            children: [
              // Status icon
              Icon(
                isConnected
                    ? Icons.volume_up
                    : isWorking
                        ? Icons.search
                        : Icons.volume_off,
                size: 80,
                color: isConnected
                    ? Colors.green
                    : isWorking
                        ? Colors.amber
                        : Colors.grey,
              ),
              const SizedBox(height: 24),

              // Server name
              if (isConnected || _serverName.isNotEmpty)
                Text(
                  _serverName,
                  style: Theme.of(context).textTheme.headlineSmall,
                ),
              if (isConnected)
                Text(
                  _serverAddress,
                  style: Theme.of(context).textTheme.bodySmall?.copyWith(
                        color: Colors.grey,
                      ),
                ),
              const SizedBox(height: 8),

              // Status message
              Text(
                _statusMessage,
                style: Theme.of(context).textTheme.bodyLarge,
                textAlign: TextAlign.center,
              ),
              const SizedBox(height: 24),

              // Latency
              if (isConnected)
                Container(
                  padding: const EdgeInsets.symmetric(
                    horizontal: 20,
                    vertical: 12,
                  ),
                  decoration: BoxDecoration(
                    color: Theme.of(context).colorScheme.surfaceContainerHighest,
                    borderRadius: BorderRadius.circular(12),
                  ),
                  child: Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      const Icon(Icons.speed, size: 20),
                      const SizedBox(width: 8),
                      Text(
                        '${_latencyMs.toStringAsFixed(1)} ms',
                        style: Theme.of(context).textTheme.titleMedium?.copyWith(
                              fontFamily: 'monospace',
                            ),
                      ),
                    ],
                  ),
                ),
              const SizedBox(height: 40),

              // Connect/Disconnect button
              SizedBox(
                width: 200,
                height: 56,
                child: FilledButton.icon(
                  onPressed: _onConnectPressed,
                  icon: isWorking
                      ? const SizedBox(
                          width: 20,
                          height: 20,
                          child: CircularProgressIndicator(
                            strokeWidth: 2,
                            color: Colors.white,
                          ),
                        )
                      : Icon(isConnected ? Icons.stop : Icons.play_arrow),
                  label: Text(
                    isConnected
                        ? 'Disconnect'
                        : isWorking
                            ? 'Searching...'
                            : 'Connect',
                    style: const TextStyle(fontSize: 18),
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
