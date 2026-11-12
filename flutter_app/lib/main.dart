import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';
import 'desktop_controls.dart';
import 'phone_controls.dart';
import 'app_updates.dart';

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
        colorSchemeSeed: const Color(0xFF7C4DFF),
        useMaterial3: true,
        brightness: Brightness.dark,
      ),
      home: const AudioBridgePage(),
      debugShowCheckedModeBanner: false,
    );
  }
}

enum ConnectionState_ {
  disconnected,
  discovering,
  connecting,
  pairPending,
  connected,
  disconnecting,
  disconnectFailed,
}

/// Represents a previously seen server
class ServerInfo {
  String name;
  String macAddress;
  String lastIp;
  DateTime lastConnected;
  bool active; // currently discovered on LAN
  bool macVerified; // MAC matches stored record
  String pskHex; // stored PSK for DTLS reconnection

  ServerInfo({
    required this.name,
    required this.macAddress,
    required this.lastIp,
    required this.lastConnected,
    this.active = false,
    this.macVerified = false,
    this.pskHex = '',
  });

  Map<String, dynamic> toJson() => {
        'name': name,
        'macAddress': macAddress,
        'lastIp': lastIp,
        'lastConnected': lastConnected.toIso8601String(),
        'pskHex': pskHex,
      };

  factory ServerInfo.fromJson(Map<String, dynamic> json) => ServerInfo(
        name: json['name'] ?? '',
        macAddress: json['macAddress'] ?? '',
        lastIp: json['lastIp'] ?? '',
        lastConnected: DateTime.tryParse(json['lastConnected'] ?? '') ?? DateTime.now(),
        pskHex: json['pskHex'] ?? '',
      );
}

class AudioBridgePage extends StatefulWidget {
  const AudioBridgePage({super.key});

  @override
  State<AudioBridgePage> createState() => _AudioBridgePageState();
}

class _AudioBridgePageState extends State<AudioBridgePage> with SingleTickerProviderStateMixin {
  static const _channel = MethodChannel('com.audiobridge/audio');
  late final DesktopControlState _desktopControls;

  ConnectionState_ _state = ConnectionState_.disconnected;
  String _statusMessage = 'Scan for nearby servers, or choose a recent PC.';
  String _serverName = '';
  String _serverAddress = '';
  String _serverMac = '';
  double _latencyMs = 0.0;
  String _deviceId = '';
  String _deviceName = 'AudioBridge';
  bool _isRecording = true; // mic on by default
  bool _isMuted = false; // audio output mute
  double _volume = 1.0;
  double _lastAudibleVolume = 1.0;
  bool _isPaused = false; // stream paused
  bool _isEncrypted = false; // DTLS active
  String _micSource = 'auto'; // 'auto', 'phone', 'bluetooth'
  bool _micBusy = false;
  bool _micUnknown = false;
  String _micMessage = '';
  bool _micAllowed = true;
  int _connectedPhones = 1;
  int _connectionEpoch = 0;
  bool _disposing = false;
  final FocusNode _shutdownRetryFocus = FocusNode();
  bool _micDesired = true;
  bool _resumeMicrophone = true;
  int _micStartsPending = 0;
  String _shutdownMessage = '';
  bool _playbackApplying = false;
  String _playbackMessage = '';
  ({double volume, bool muted})? _playbackRequested;
  int _discoveryEpoch = 0;

  // Audio level meters
  double _outputLevel = 0.0; // 0.0-1.0 receiving level
  double _inputLevel = 0.0;  // 0.0-1.0 sending level

  // Latency breakdown
  double _rttMs = 0.0;
  double _jitterBufferMs = 0.0;
  double _outputBufferMs = 0.0;
  double _serverCaptureMs = 2.5; // Default 2.5ms Raw PCM frame
  int _serverJitterMs = 0; // Jitter target from server
  bool _isRawPcm = true; // Codec mode from server (true = Raw PCM, false = Opus)

  // Now playing info from server
  String _nowPlayingTitle = '';
  String _nowPlayingArtist = '';
  String _nowPlayingAlbum = '';
  int _nowPlayingStatus = 0; // 0=stopped, 1=playing, 2=paused
  int _nowPlayingPositionMs = 0;
  int _nowPlayingDurationMs = 0;

  RawDatagramSocket? _discoverySocket;
  Timer? _discoveryTimer;
  Timer? _keepaliveTimer;
  Timer? _timeoutTimer;
  Timer? _latencyTimer;
  DateTime? _lastPacketTime;

  // Connection history
  List<ServerInfo> _knownServers = [];
  // Servers discovered in current scan
  final Map<String, _DiscoveredServer> _discoveredServers = {};

  // Scan animation
  late AnimationController _pulseController;

  @override
  void initState() {
    super.initState();
    _desktopControls = DesktopControlState(_channel)..addListener(_desktopChanged);
    _pulseController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1500),
    );
    _loadHistory();
    _loadOrCreateDeviceId();
    _loadDeviceName();
    _requestPermissions();

    // Handle callbacks from native and media notification actions
    _channel.setMethodCallHandler((call) async {
      switch (call.method) {
        case 'onDesktopState':
          _desktopControls.receiveState(call.arguments as String? ?? '');
          break;
        case 'onDesktopResult':
          _desktopControls.receiveResult(call.arguments as String? ?? '');
          break;
        case 'onMediaAction':
          final action = call.arguments as String?;
          switch (action) {
            case 'toggleMic':
              _toggleMic(_micUnknown ? false : !_isRecording);
              break;
            case 'togglePause':
              _mediaPlayPause();
              break;
            case 'disconnect':
              _disconnect();
              break;
          }
          break;

        case 'onSessionInfo':
          try {
            final info = jsonDecode(call.arguments as String) as Map<String, dynamic>;
            if (!mounted) break;
            setState(() {
              _micAllowed = info['micAllowed'] == true;
              _connectedPhones = (info['clients'] as num?)?.toInt() ?? 1;
            });
            if (!_micAllowed && (_isRecording || _micUnknown)) {
              await _toggleMic(false, duringConnection: true);
            }
          } catch (_) {}
          break;

        case 'onMediaInfo':
          // Media info packet from native recv thread
          try {
            final jsonStr = call.arguments as String? ?? '';
            if (jsonStr.isNotEmpty) {
              final info = jsonDecode(jsonStr) as Map<String, dynamic>;
              if (mounted) {
                setState(() {
                  _nowPlayingTitle = (info['t'] as String?) ?? '';
                  _nowPlayingArtist = (info['a'] as String?) ?? '';
                  _nowPlayingAlbum = (info['al'] as String?) ?? '';
                  _nowPlayingStatus = (info['s'] as int?) ?? 0;
                  _nowPlayingPositionMs = (info['p'] as int?) ?? 0;
                  _nowPlayingDurationMs = (info['d'] as int?) ?? 0;
                });
                if (_state == ConnectionState_.connected) _updateServiceState();
              }
            }
          } catch (_) {}
          break;

        case 'onDisconnected':
          // Server disconnect or DTLS error from native
          final reason = call.arguments as String? ?? '';
          _handleNativeDisconnect(reason);
          break;

        case 'onPairPending':
          // Pair pending notification from native connect thread
          if (mounted) {
            setState(() {
              _state = ConnectionState_.pairPending;
              final code = call.arguments as String? ?? '';
              _statusMessage = 'Pairing code: $code\nApprove on your PC only if both codes match.';
            });
          }
          break;

        case 'onSettingsUpdate':
          // Server sent jitter buffer setting
          try {
            final jitterMs = int.tryParse(call.arguments as String? ?? '') ?? 0;
            if (jitterMs > 0 && mounted) {
              setState(() {
                _serverJitterMs = jitterMs;
              });
            }
          } catch (_) {}
          break;

        case 'onFrameSizeUpdate':
          // Server sent frame size setting
          try {
            final frameMs = int.tryParse(call.arguments as String? ?? '') ?? 2;
            if (mounted) {
              setState(() {
                final baseMs = (frameMs == 2) ? 2.5 : frameMs.toDouble();
                _serverCaptureMs = baseMs + (_isRawPcm ? 0.0 : 2.5);
              });
            }
          } catch (_) {}
          break;

        case 'onStreamConfig':
          try {
            final jsonStr = call.arguments as String? ?? '';
            if (jsonStr.isNotEmpty) {
              final cfg = jsonDecode(jsonStr) as Map<String, dynamic>;
              final jitter = (cfg['jitter'] as num?)?.toInt() ?? _serverJitterMs;
              final samples = (cfg['samples'] as num?)?.toInt() ?? 120;
              final pcm = (cfg['pcm'] as bool?) ?? true;
              if (mounted) {
                setState(() {
                  _serverJitterMs = jitter;
                  _isRawPcm = pcm;
                  final frameExactMs = samples * 1000.0 / 48000.0;
                  _serverCaptureMs = frameExactMs + (pcm ? 0.0 : 2.5);
                });
              }
            }
          } catch (_) {}
          break;
      }
      return null;
    });
  }

  void _desktopChanged() {
    if (mounted) setState(() {});
  }

  @override
  void dispose() {
    _disposing = true;
    _desktopControls.removeListener(_desktopChanged);
    _disconnect();
    _desktopControls.dispose();
    _pulseController.dispose();
    _shutdownRetryFocus.dispose();
    _channel.setMethodCallHandler(null);
    super.dispose();
  }

  void _handleNativeDisconnect(String reason) {
    _disconnect(reason: reason);
  }

  // --- Persistence ---

  Future<Directory> get _appDir async {
    final dir = Directory('/data/data/com.audiobridge.audiobridge/files');
    if (!await dir.exists()) await dir.create(recursive: true);
    return dir;
  }

  Future<File> get _historyFile async {
    final dir = await _appDir;
    return File('${dir.path}/connection_history.json');
  }

  Future<void> _loadOrCreateDeviceId() async {
    try {
      final dir = await _appDir;
      final file = File('${dir.path}/device_id.txt');
      if (await file.exists()) {
        _deviceId = (await file.readAsString()).trim();
      } else {
        final rng = Random.secure();
        final bytes = List<int>.generate(16, (_) => rng.nextInt(256));
        _deviceId = bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join();
        _deviceId = '${_deviceId.substring(0, 8)}-${_deviceId.substring(8, 12)}-'
            '${_deviceId.substring(12, 16)}-${_deviceId.substring(16, 20)}-'
            '${_deviceId.substring(20)}';
        await file.writeAsString(_deviceId);
      }
    } catch (e) {
      _deviceId = 'unknown';
    }
  }

  Future<void> _loadDeviceName() async {
    try {
      final dir = await _appDir;
      final file = File('${dir.path}/device_name.txt');
      if (await file.exists()) {
        final name = (await file.readAsString()).trim();
        if (name.isNotEmpty) {
          setState(() => _deviceName = name);
        }
      }
    } catch (_) {}
  }

  Future<void> _requestPermissions() async {
    try {
      await _channel.invokeMethod('requestPermissions');
    } catch (_) {}
  }

  Future<void> _saveDeviceName(String name) async {
    try {
      final dir = await _appDir;
      final file = File('${dir.path}/device_name.txt');
      await file.writeAsString(name);
      if (mounted) setState(() => _deviceName = name);
    } catch (_) {}
  }

  void _showEditDeviceNameDialog() {
    final controller = TextEditingController(text: _deviceName);
    String? nameError;
    showDialog(
      context: context,
      builder: (ctx) => StatefulBuilder(builder: (context, updateDialog) => AlertDialog(
        scrollable: true,
        title: const Text('Device Name'),
        content: TextField(
          controller: controller,
          autofocus: true,
          maxLength: 30,
          onChanged: (value) {
            if (nameError != null && value.trim().isNotEmpty) updateDialog(() => nameError = null);
          },
          decoration: InputDecoration(
            labelText: 'Device name',
            hintText: 'Enter device name',
            errorText: nameError,
          ),
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          TextButton(
            onPressed: () {
              final name = controller.text.trim();
              if (name.isEmpty) {
                updateDialog(() => nameError = 'Enter a device name.');
                return;
              }
              _saveDeviceName(name);
              Navigator.pop(ctx);
            },
            child: const Text('Save'),
          ),
        ],
      )),
    );
  }

  Future<void> _loadHistory() async {
    try {
      final file = await _historyFile;
      if (await file.exists()) {
        final content = await file.readAsString();
        final List<dynamic> list = jsonDecode(content);
        setState(() {
          _knownServers = list.map((e) => ServerInfo.fromJson(e)).toList();
        });
      }
    } catch (e) {
      // First launch or corrupt file
    }
  }

  Future<void> _saveHistory() async {
    try {
      final file = await _historyFile;
      await file.writeAsString(jsonEncode(_knownServers.map((s) => s.toJson()).toList()));
    } catch (e) {}
  }

  void _recordConnection(String name, String mac, String ip, {String pskHex = ''}) {
    final existing = _knownServers.where((s) => s.macAddress == mac).toList();
    if (existing.isNotEmpty) {
      existing.first.name = name;
      existing.first.lastIp = ip;
      existing.first.lastConnected = DateTime.now();
      if (pskHex.isNotEmpty) {
        existing.first.pskHex = pskHex;
      }
    } else {
      _knownServers.add(ServerInfo(
        name: name,
        macAddress: mac,
        lastIp: ip,
        lastConnected: DateTime.now(),
        pskHex: pskHex,
      ));
    }
    _saveHistory();
  }

  /// Look up stored PSK for a server by MAC address
  String _getStoredPsk(String mac) {
    if (mac.isEmpty) return '';
    final matches = _knownServers.where((s) => s.macAddress == mac).toList();
    if (matches.isNotEmpty) return matches.first.pskHex;
    return '';
  }

  // --- Discovery ---

  Future<void> _startDiscovery() async {
    if (_state == ConnectionState_.discovering) {
      _stopDiscovery();
      setState(() {
        _state = ConnectionState_.disconnected;
        _statusMessage = 'Scan stopped';
      });
      return;
    }

    setState(() {
      _state = ConnectionState_.discovering;
      _statusMessage = 'Scanning for servers...';
      _discoveredServers.clear();
      for (var s in _knownServers) {
        s.active = false;
        s.macVerified = false;
      }
    });
    if (!MediaQuery.disableAnimationsOf(context)) _pulseController.repeat();
    final epoch = ++_discoveryEpoch;

    try {
      final socket = await RawDatagramSocket.bind(
        InternetAddress.anyIPv4,
        0,
        reuseAddress: true,
      );
      if (!mounted || epoch != _discoveryEpoch) {
        socket.close();
        return;
      }
      _discoverySocket = socket;
      socket.broadcastEnabled = true;

      socket.listen((event) {
        if (!mounted || epoch != _discoveryEpoch) return;
        if (event == RawSocketEvent.read) {
          final datagram = socket.receive();
          if (datagram != null) {
            final msg = String.fromCharCodes(datagram.data);
            if (msg.startsWith('AB_OFFER|')) {
              final parts = msg.split('|');
              if (parts.length >= 3) {
                final name = parts[1];
                final port = int.tryParse(parts[2]) ?? 4012;
                final mac = parts.length >= 4 ? parts[3] : '';
                final ip = datagram.address.address;

                _discoveredServers[mac.isNotEmpty ? mac : ip] = _DiscoveredServer(
                  name: name,
                  ip: ip,
                  port: port,
                  mac: mac,
                );

                _updateKnownServerStatus(name, mac, ip);
                setState(() {});
              }
            }
          }
        }
      });

      _sendDiscovery();
      _discoveryTimer = Timer.periodic(
        const Duration(seconds: 2),
        (_) => _sendDiscovery(),
      );

      _timeoutTimer = Timer(const Duration(seconds: 30), () {
        if (_state == ConnectionState_.discovering) {
          _stopDiscovery();
          setState(() {
            _state = ConnectionState_.disconnected;
            _statusMessage = 'Scan timed out';
          });
        }
      });
    } catch (e) {
      if (!mounted || epoch != _discoveryEpoch) return;
      _stopDiscovery();
      setState(() {
        _state = ConnectionState_.disconnected;
        _statusMessage = 'Discovery error: $e';
      });
    }
  }

  void _updateKnownServerStatus(String name, String mac, String ip) {
    if (mac.isEmpty) return;

    for (var server in _knownServers) {
      if (server.macAddress == mac) {
        server.active = true;
        server.macVerified = true;
        server.lastIp = ip;
        server.name = name;
      } else if (server.name == name && server.macAddress != mac) {
        server.active = true;
        server.macVerified = false;
      }
    }
  }

  Future<void> _sendDiscovery() async {
    final sock = _discoverySocket;
    if (sock == null) return;
    final data = Uint8List.fromList('AB_DISCOVER'.codeUnits);
    final targets = <String>{'255.255.255.255'};

    try {
      final interfaces = await NetworkInterface.list(
        type: InternetAddressType.IPv4,
        includeLinkLocal: false,
      );
      for (final iface in interfaces) {
        for (final addr in iface.addresses) {
          final parts = addr.address.split('.');
          if (parts.length == 4 && parts[0] != '127') {
            targets.add('${parts[0]}.${parts[1]}.${parts[2]}.255');
          }
        }
      }
    } catch (_) {}

    for (final s in _knownServers) {
      if (s.lastIp.isNotEmpty) {
        targets.add(s.lastIp);
      }
    }

    for (final host in targets) {
      try {
        sock.send(data, InternetAddress(host), 4011);
      } catch (_) {}
    }
  }

  void _stopDiscovery() {
    ++_discoveryEpoch;
    _discoveryTimer?.cancel();
    _discoveryTimer = null;
    _timeoutTimer?.cancel();
    _timeoutTimer = null;
    _discoverySocket?.close();
    _discoverySocket = null;
    _pulseController.stop();
    _pulseController.reset();
  }

  // --- Connection ---

  Future<void> _connectToServer(InternetAddress address, int port, String name, String mac) async {
    if (_state == ConnectionState_.connecting || _state == ConnectionState_.pairPending) return;
    final epoch = ++_connectionEpoch;
    _desktopControls.reset();
    _micAllowed = true;
    _connectedPhones = 1;
    setState(() {
      _state = ConnectionState_.connecting;
      _statusMessage = 'Connecting to $name...';
      _micBusy = false;
      _micUnknown = false;
      _micMessage = '';
      _isRecording = _resumeMicrophone;
      _micDesired = _resumeMicrophone;
      _playbackMessage = '';
      _serverName = name;
      _serverMac = mac;
      _serverAddress = address.address;
      _isPaused = false;
      _isMuted = false;
    });

    _stopDiscovery();

    try {
      // Look up stored PSK for this server
      final storedPsk = _getStoredPsk(mac);

      // Delegate entire connection lifecycle to native
      final result = await _channel.invokeMethod<String>('connect', {
        'ip': address.address,
        'port': port,
        'deviceName': _deviceName,
        'deviceId': _deviceId,
        'pskHex': storedPsk,
      });
      if (!mounted || epoch != _connectionEpoch) return;

      if (result == null || (result != 'accepted' && !result.startsWith('accepted|'))) {
        String message;
        if (result == 'timeout') {
          message = 'Server did not respond';
        } else if (result != null && result.startsWith('rejected|')) {
          message = result.substring(9);
        } else if (result == 'pair_pending') {
          // This shouldn't happen since native blocks until accept/reject
          message = 'Approval timed out';
        } else if (result == 'dtls_failed') {
          message = 'Encryption handshake failed';
        } else if (result != null && result.startsWith('error|')) {
          message = result.substring(6);
        } else {
          message = 'Connection failed';
        }
        setState(() {
          _state = ConnectionState_.disconnected;
          _statusMessage = message;
        });
        return;
      }

      // Extract PSK if this was a first pairing
      String newPsk = '';
      if (result.startsWith('accepted|') && result.length > 9) {
        newPsk = result.substring(9);
      }
      // Check actual DTLS state from native (not just PSK presence)
      try {
        _isEncrypted = await _channel.invokeMethod<bool>('isDtlsActive') ?? false;
      } catch (_) {
        _isEncrypted = false;
      }
      if (!mounted || epoch != _connectionEpoch) return;
      if (!_isEncrypted) throw StateError('Encrypted connection required');

      // Start audio playback
      final started = await _channel.invokeMethod<bool>('startAudio', {'serverName': name});
      if (!mounted || epoch != _connectionEpoch) return;
      if (started != true) throw StateError('Could not start audio playback');
      await _channel.invokeMethod('setVolume', _volume);
      await _channel.invokeMethod('setMuted', _isMuted);
      if (!mounted || epoch != _connectionEpoch) return;

      if (_isRecording && _micAllowed) {
        await _toggleMic(true, duringConnection: true);
        if (!mounted || epoch != _connectionEpoch) return;
      }

      // Record connection with PSK
      _recordConnection(name, mac, address.address, pskHex: newPsk.isNotEmpty ? newPsk : storedPsk);

      setState(() {
        _state = ConnectionState_.connected;
        _statusMessage = 'Connected to $_serverName';
      });

      _updateServiceState();
      _lastPacketTime = DateTime.now();

      // Start keepalive and ping timers (using native send methods)
      _keepaliveTimer = Timer.periodic(const Duration(milliseconds: 1000), (timer) {
        _channel.invokeMethod('sendKeepalive');
        // Send ping every 2 seconds (every other keepalive)
        if (timer.tick % 2 == 0) {
          _channel.invokeMethod('sendPing');
        }
      });

      // Poll latency breakdown and RTT from native
      _latencyTimer = Timer.periodic(const Duration(milliseconds: 500), (_) async {
        try {
          final results = await Future.wait([
            _channel.invokeMethod<Map>('getLatencyBreakdown'),
            _channel.invokeMethod<double>('getRtt'),
          ]);
          final breakdown = results[0] as Map?;
          final rtt = results[1] as double? ?? 0.0;
          if (breakdown != null && mounted) {
            setState(() {
              _rttMs = rtt;
              _jitterBufferMs = (breakdown['jitterBufferMs'] as num?)?.toDouble() ?? 0.0;
              _outputBufferMs = (breakdown['outputBufferMs'] as num?)?.toDouble() ?? 0.0;
              _latencyMs = (_rttMs / 2) + _jitterBufferMs + _outputBufferMs + _serverCaptureMs;
            });
          }
        } catch (_) {}
      });

      // Check native connection status periodically for timeout detection
      _timeoutTimer = Timer.periodic(const Duration(seconds: 2), (_) async {
        try {
          final connected = await _channel.invokeMethod<bool>('isConnected') ?? false;
          if (!connected && _state == ConnectionState_.connected) {
            _handleNativeDisconnect('timeout');
          }
        } catch (_) {}
      });
    } catch (e) {
      if (!mounted || epoch != _connectionEpoch) return;
      for (final method in ['stopRecording', 'stopAudio', 'disconnect']) {
        try { await _channel.invokeMethod(method); } catch (_) {}
      }
      if (!mounted || epoch != _connectionEpoch) return;
      setState(() {
        _state = ConnectionState_.disconnected;
        _statusMessage = 'Connection error: $e';
      });
      if (mounted) {
        ScaffoldMessenger.of(context).showSnackBar(
          SnackBar(
            content: Text('Connection error: $e'),
            behavior: SnackBarBehavior.floating,
          ),
        );
      }
    }
  }

  void _showLatencyBreakdown(ColorScheme cs) {
    final networkOneWay = _rttMs / 2;
    final total =
        networkOneWay + _serverCaptureMs + _jitterBufferMs + _outputBufferMs;

    showModalBottomSheet(
      context: context,
      useSafeArea: true,
      isScrollControlled: true,
      constraints: BoxConstraints(
        maxHeight: MediaQuery.sizeOf(context).height * 0.85,
      ),
      backgroundColor: cs.surfaceContainerHigh,
      shape: const RoundedRectangleBorder(
        borderRadius: BorderRadius.vertical(top: Radius.circular(20)),
      ),
      builder: (context) {
        return SingleChildScrollView(
          child: Padding(
            padding: const EdgeInsets.fromLTRB(24, 20, 24, 32),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Center(
                  child: Container(
                    width: 40,
                    height: 4,
                    decoration: BoxDecoration(
                      color: cs.onSurface.withOpacity(0.2),
                      borderRadius: BorderRadius.circular(2),
                    ),
                  ),
                ),
                const SizedBox(height: 16),
                Text(
                  'Latency Breakdown',
                  style: Theme.of(
                    context,
                  ).textTheme.titleMedium?.copyWith(fontWeight: FontWeight.bold),
                ),
                const SizedBox(height: 16),
                _latencyRow('Network (one-way)', networkOneWay, cs),
                _latencyRow(
                  'Server frame (${_isRawPcm ? 'Raw PCM' : 'Opus +2.5ms'})',
                  _serverCaptureMs,
                  cs,
                ),
                _latencyRow(
                  'Jitter buffer${_serverJitterMs > 0 ? ' (target: ${_serverJitterMs}ms)' : ''}',
                  _jitterBufferMs,
                  cs,
                ),
                _latencyRow('Audio output buffer', _outputBufferMs, cs),
                const SizedBox(height: 8),
                Divider(color: cs.outlineVariant.withOpacity(0.3)),
                const SizedBox(height: 8),
                _latencyRow('Estimated total', total, cs, bold: true),
                const SizedBox(height: 12),
                _latencyRow('Network RTT', _rttMs, cs, dim: true),
              ],
            ),
          ),
        );
      },
    );
  }

  Widget _buildLevelBar(String label, double level, Color color, ColorScheme cs) {
    return Row(
      children: [
        SizedBox(
          width: 68,
          child: Text(
            label,
            style: Theme.of(context).textTheme.labelSmall?.copyWith(
              color: cs.onSurface.withOpacity(0.5),
            ),
          ),
        ),
        Expanded(
          child: ClipRRect(
            borderRadius: BorderRadius.circular(3),
            child: SizedBox(
              height: 6,
              child: LinearProgressIndicator(
                value: level.clamp(0.0, 1.0),
                backgroundColor: cs.surfaceContainerHighest,
                valueColor: AlwaysStoppedAnimation(
                  level > 0.8 ? Colors.red : (level > 0.5 ? Colors.amber : color),
                ),
              ),
            ),
          ),
        ),
      ],
    );
  }

  Widget _latencyRow(String label, double ms, ColorScheme cs, {bool bold = false, bool dim = false}) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 4),
      child: Wrap(
        alignment: WrapAlignment.spaceBetween,
        spacing: 12,
        runSpacing: 4,
        children: [
          Text(label,
            style: Theme.of(context).textTheme.bodyMedium?.copyWith(
              color: cs.onSurfaceVariant,
              fontWeight: bold ? FontWeight.bold : FontWeight.normal,
            ),
          ),
          Text('${ms.toStringAsFixed(1)} ms',
            style: Theme.of(context).textTheme.bodyMedium?.copyWith(
              fontFamily: 'monospace',
              fontWeight: bold ? FontWeight.bold : FontWeight.w500,
              color: dim ? cs.onSurfaceVariant : cs.onSurface,
            ),
          ),
        ],
      ),
    );
  }

  // --- Media Controls ---

  Future<T?> _microphoneCall<T>(String method, [Object? arguments]) async {
    final epoch = _connectionEpoch;
    final starting = method == 'startRecording';
    var expired = false;
    if (starting) _micStartsPending++;
    final operation = _channel.invokeMethod<T>(method, arguments);
    unawaited(operation.then<void>((_) async {
      if (starting) _micStartsPending--;
      if (expired || epoch != _connectionEpoch) await _recoverLateMicrophone(epoch);
    }, onError: (Object error, StackTrace stack) async {
      if (starting) _micStartsPending--;
      if (expired || epoch != _connectionEpoch) await _recoverLateMicrophone(epoch);
    }));
    return operation.timeout(const Duration(seconds: 4), onTimeout: () {
      expired = true;
      throw TimeoutException('Microphone request is still pending');
    });
  }

  Future<Map?> _readAudioState() async {
    try {
      return await _channel.invokeMethod<Map>('getAudioState')
          .timeout(const Duration(seconds: 1));
    } catch (_) {
      return null;
    }
  }

  Future<void> _recoverLateMicrophone(int epoch) async {
    if (_disposing || !mounted) return;
    if (epoch != _connectionEpoch && _state == ConnectionState_.connected) return;
    if (!_micDesired || !_micAllowed ||
        (_state != ConnectionState_.connected && _state != ConnectionState_.connecting)) {
      try {
        await _channel.invokeMethod('stopRecording').timeout(const Duration(seconds: 4));
      } catch (_) {}
    }
    if (_state == ConnectionState_.disconnectFailed || _state == ConnectionState_.disconnecting) {
      await _reconcileShutdown(_connectionEpoch);
      return;
    }
    final audio = await _readAudioState();
    if (!mounted || _disposing || epoch != _connectionEpoch) return;
    setState(() {
      final actual = audio?['recording'];
      if (actual is bool) _isRecording = actual;
      _micUnknown = actual is! bool || _micStartsPending > 0;
      _micMessage = _micUnknown
          ? 'Microphone outcome is still unknown. Stop microphone or disconnect to recover.'
          : 'Late microphone request settled. Current microphone: ${_isRecording ? 'On' : 'Off'}.${_isRecording && !_micDesired ? ' Retry stopping, or disconnect.' : ''}';
    });
  }

  Future<void> _toggleMic(bool enable, {bool duringConnection = false}) async {
    if (enable && !_micAllowed) return;
    if (_micBusy || _serverAddress.isEmpty ||
        (_state != ConnectionState_.connected &&
            !(duringConnection && _state == ConnectionState_.connecting))) {
      return;
    }
    final epoch = _connectionEpoch;
    var stoppedForOwnership = false;
    _micDesired = enable;
    setState(() {
      _micBusy = true;
      _micMessage = enable ? 'Starting phone microphone...' : 'Stopping phone microphone...';
    });
    try {
      if (enable) {
        final started = await _microphoneCall<bool>('startRecording', {
          'ip': _serverAddress, 'port': 4012, 'micSource': _micSource,
        });
        if (started != true) throw StateError('Audio input did not start');
        if (!mounted || epoch != _connectionEpoch) return;
        if (!_micAllowed) {
          stoppedForOwnership = true;
          _micDesired = false;
          await _microphoneCall('stopRecording');
        }
      } else {
        await _microphoneCall('stopRecording');
      }
      if (mounted && !_disposing && epoch == _connectionEpoch) {
        setState(() {
          _isRecording = enable && !stoppedForOwnership;
          _micUnknown = _micStartsPending > 0;
          _micMessage = _micUnknown
              ? 'A microphone start is still pending. Stop microphone or disconnect to recover.'
              : stoppedForOwnership
              ? 'Phone microphone stopped because microphone access changed.'
              : enable ? 'Phone microphone is on.' : 'Phone microphone is off.';
        });
      }
    } catch (error) {
      if (error is TimeoutException && enable) _micDesired = false;
      final audio = await _readAudioState();
      if (mounted && !_disposing && epoch == _connectionEpoch) {
        setState(() {
          final actual = audio?['recording'];
          if (actual is bool) _isRecording = actual;
          _micUnknown = actual is! bool || _micStartsPending > 0;
          final requestedState = enable && !stoppedForOwnership;
          final operation = requestedState ? 'start' : 'stop';
          _micMessage = _micUnknown
              ? 'Could not confirm microphone $operation. State is unknown. Stop microphone to confirm it is off, or disconnect.'
              : actual == requestedState
              ? 'Phone microphone is ${_isRecording ? 'on' : 'off'}. Current state verified after a request error.'
              : 'Could not $operation phone microphone. Current microphone: ${_isRecording ? 'On' : 'Off'}. ${_isRecording ? 'Retry stopping, or disconnect.' : 'You can retry starting it.'}';
        });
      }
    } finally {
      if (mounted && !_disposing && epoch == _connectionEpoch) setState(() => _micBusy = false);
      if (_state == ConnectionState_.connected && epoch == _connectionEpoch) _updateServiceState();
    }
  }

  Future<void> _switchMicSource(String source) async {
    if (source == _micSource || _micBusy || _micUnknown) return;
    final epoch = _connectionEpoch;
    final restart = _isRecording && _state == ConnectionState_.connected;
    if (restart) {
      await _toggleMic(false);
      if (!mounted || epoch != _connectionEpoch || _isRecording || _micUnknown) {
        return;
      }
    }
    setState(() => _micSource = source);
    if (restart && _state == ConnectionState_.connected) await _toggleMic(true);
  }

  Future<void> _refreshPlayback(int epoch, String status) async {
    final audio = await _readAudioState();
    if (!mounted || _disposing || epoch != _connectionEpoch || _state != ConnectionState_.connected) return;
    setState(() {
      final volume = audio?['volume'];
      final muted = audio?['muted'];
      if (volume is num) {
        _volume = volume.toDouble().clamp(0.0, 1.0);
        if (_volume > 0) _lastAudibleVolume = _volume;
      }
      if (muted is bool) _isMuted = muted;
      _playbackMessage = volume is num && muted is bool
          ? status
          : 'Playback outcome is unknown. Values are last known; retry mute or volume, or disconnect.';
    });
  }

  Future<void> _playbackCall(String method, Object value, int epoch) async {
    var expired = false;
    final operation = _channel.invokeMethod(method, value);
    unawaited(operation.then<void>((_) async {
      if (expired) await _refreshPlayback(epoch, 'Delayed playback request settled. Current values verified.');
    }, onError: (Object error, StackTrace stack) {}));
    await operation.timeout(const Duration(seconds: 3), onTimeout: () {
      expired = true;
      throw TimeoutException('Playback change is still pending');
    });
  }

  Future<void> _applyPlayback(double volume, bool muted) async {
    if (_state != ConnectionState_.connected) return;
    _playbackRequested = (volume: volume, muted: muted);
    if (_playbackApplying) return;
    final epoch = _connectionEpoch;
    _playbackApplying = true;
    try {
      while (_playbackRequested != null && epoch == _connectionEpoch && _state == ConnectionState_.connected) {
        final requested = _playbackRequested!;
        _playbackRequested = null;
        setState(() => _playbackMessage = 'Applying playback change. Displayed values are last confirmed.');
        var failed = false;
        try {
          await _playbackCall('setVolume', requested.volume, epoch);
          if (epoch != _connectionEpoch) return;
          await _playbackCall('setMuted', requested.muted, epoch);
        } catch (_) {
          failed = true;
        }
        final audio = await _readAudioState();
        if (!mounted || _disposing || epoch != _connectionEpoch || _state != ConnectionState_.connected) return;
        final actualVolume = audio?['volume'];
        final actualMuted = audio?['muted'];
        setState(() {
          if (actualVolume is num) {
            _volume = actualVolume.toDouble().clamp(0.0, 1.0);
            if (_volume > 0) _lastAudibleVolume = _volume;
          }
          if (actualMuted is bool) _isMuted = actualMuted;
          _playbackMessage = actualVolume is! num || actualMuted is! bool
              ? 'Playback outcome is unknown. Values are last known; retry mute or volume, or disconnect.'
              : !failed && (_volume - requested.volume).abs() < 0.001 && _isMuted == requested.muted
              ? 'Playback change confirmed.'
              : 'Could not apply playback change. Current volume: ${(_volume * 100).round()} percent; ${_isMuted ? 'muted' : 'unmuted'}. Retry the control or disconnect.';
        });
      }
    } finally {
      if (epoch == _connectionEpoch) _playbackApplying = false;
    }
  }

  Future<void> _toggleMute() async {
    final current = _playbackRequested ?? (volume: _volume, muted: _isMuted);
    final unmute = current.muted || current.volume == 0;
    await _applyPlayback(unmute && current.volume == 0 ? _lastAudibleVolume : current.volume, !unmute);
  }

  Future<void> _setVolume(double volume) => _applyPlayback(volume, volume == 0);

  void _mediaPlayPause() {
    _sendMediaCommand(0x10); // CTRL_MEDIA_PLAY_PAUSE
  }

  void _sendMediaCommand(int cmd) {
    if (_state != ConnectionState_.connected || _serverAddress.isEmpty) return;
    if (!_desktopControls.canEdit) return;
    _desktopControls.send(16, cmd);
    // Request fresh media info after a short delay for the action to take effect
    Future.delayed(const Duration(milliseconds: 300), _requestMediaInfo);
  }

  void _requestMediaInfo() {
    if (_state != ConnectionState_.connected || _serverAddress.isEmpty) return;
    _sendControlCommand(0x13); // CTRL_MEDIA_INFO_REQ
  }

  void _sendControlCommand(int cmd) {
    try {
      _channel.invokeMethod('sendControl', cmd);
    } catch (_) {}
  }

  void _updateServiceState() {
    try {
      _channel.invokeMethod('updateServiceState', {
        'micMuted': !_isRecording,
        'paused': _isPaused,
        'mediaPlaying': _nowPlayingStatus == 1,
        'serverName': _serverName,
      });
    } catch (_) {}
  }

  Future<void> _reconcileShutdown(int epoch) async {
    final results = await Future.wait<Object?>([
      _readAudioState(),
      _channel.invokeMethod<bool>('isConnected').timeout(const Duration(seconds: 1)).catchError((Object error) => null),
    ]);
    if (!mounted || _disposing || epoch != _connectionEpoch ||
        (_state != ConnectionState_.disconnecting && _state != ConnectionState_.disconnectFailed)) {
      return;
    }
    final audio = results[0] as Map?;
    if (audio?['recording'] == false && audio?['playing'] == false &&
        results[1] == false && _micStartsPending == 0) {
      setState(() {
        _state = ConnectionState_.disconnected;
        _isRecording = false;
        _playbackApplying = false;
        _playbackRequested = null;
        _playbackMessage = '';
        _shutdownMessage = '';
        _latencyMs = 0;
        _serverName = '';
        _serverAddress = '';
        _serverMac = '';
        _statusMessage = 'Disconnected';
        _micBusy = false;
        _micUnknown = false;
        _micMessage = '';
        _isPaused = false;
        _isMuted = false;
        _isEncrypted = false;
        _volume = 1.0;
        _rttMs = 0.0;
        _outputLevel = 0.0;
        _inputLevel = 0.0;
        _jitterBufferMs = 0.0;
        _outputBufferMs = 0.0;
        _serverJitterMs = 0;
        _nowPlayingTitle = '';
        _nowPlayingArtist = '';
        _nowPlayingAlbum = '';
        _nowPlayingStatus = 0;
        _nowPlayingPositionMs = 0;
        _nowPlayingDurationMs = 0;
      });
    } else {
      setState(() {
        final actual = audio?['recording'];
        if (actual is bool) _isRecording = actual;
        _micUnknown = actual is! bool || _micStartsPending > 0;
        _state = ConnectionState_.disconnectFailed;
        _shutdownMessage = 'Audio shutdown is not confirmed. Microphone or playback may still be running. Retry stopping audio and disconnecting; if it cannot be confirmed, stop AudioBridge from Android app settings.';
      });
    }
  }

  Future<void> _disconnect({String? reason}) async {
    if (!_disposing && _state == ConnectionState_.disconnecting) return;
    _desktopControls.reset(notify: false);
    final epoch = ++_connectionEpoch;
    _resumeMicrophone = _isRecording;
    _micDesired = false;
    _keepaliveTimer?.cancel();
    _keepaliveTimer = null;
    _latencyTimer?.cancel();
    _latencyTimer = null;
    _timeoutTimer?.cancel();
    _timeoutTimer = null;
    _stopDiscovery();
    _playbackRequested = null;
    if (mounted && !_disposing) {
      setState(() {
        _state = ConnectionState_.disconnecting;
        _micBusy = false;
        _shutdownMessage = reason == null
            ? 'Stopping microphone, playback and connection...'
            : 'Connection lost. Stopping microphone and playback...';
      });
    }
    await Future.wait(['stopRecording', 'stopAudio', 'disconnect'].map((method) async {
      var expired = false;
      final operation = _channel.invokeMethod(method);
      unawaited(operation.then<void>((_) async {
        if (expired && mounted && !_disposing && epoch == _connectionEpoch) await _reconcileShutdown(epoch);
      }, onError: (Object error, StackTrace stack) {}));
      try {
        await operation.timeout(const Duration(seconds: 3), onTimeout: () {
          expired = true;
          throw TimeoutException('Audio shutdown is still pending');
        });
      } catch (_) {}
    }));
    if (!_disposing) await _reconcileShutdown(epoch);
  }

  void _connectToDiscovered(_DiscoveredServer server) {
    _connectToServer(
      InternetAddress(server.ip),
      server.port,
      server.name,
      server.mac,
    );
  }

  void _reconnectToServer(ServerInfo server) {
    _connectToServer(
      InternetAddress(server.lastIp),
      4012,
      server.name,
      server.macAddress,
    );
  }

  String _timeAgo(DateTime dt) {
    final age = DateTime.now().difference(dt);
    if (age.inDays > 30) return '${(age.inDays / 30).floor()}mo ago';
    if (age.inDays > 0) return '${age.inDays}d ago';
    if (age.inHours > 0) return '${age.inHours}h ago';
    if (age.inMinutes > 0) return '${age.inMinutes}m ago';
    return 'just now';
  }

  // --- UI ---

  @override
  Widget build(BuildContext context) {
    final cs = Theme.of(context).colorScheme;

    return Scaffold(
      backgroundColor: cs.surface,
      appBar: AppBar(
        title: const Text('AudioBridge · 0.2.0'),
        actions: [IconButton(
          tooltip: 'App updates',
          icon: const Icon(Icons.system_update_rounded),
          onPressed: () => showDialog<void>(context: context, builder: (context) => AppUpdateDialog(
            beforeInstall: () async {
              await _disconnect();
              return mounted && _state == ConnectionState_.disconnected;
            },
          )),
        )],
      ),
      body: SafeArea(
        child: _state == ConnectionState_.disconnecting ||
                _state == ConnectionState_.disconnectFailed
            ? _buildShutdownView(cs)
            : _state == ConnectionState_.connected
            ? _buildConnectedView(cs)
            : _buildMainView(cs),
      ),
    );
  }

  Widget _buildShutdownView(ColorScheme cs) {
    final pending = _state == ConnectionState_.disconnecting;
    return SingleChildScrollView(
      padding: const EdgeInsets.all(24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Semantics(header: true, child: Text(
            pending ? 'Disconnecting...' : 'Shutdown not confirmed',
            style: Theme.of(context).textTheme.headlineSmall,
          )),
          const SizedBox(height: 16),
          Semantics(liveRegion: true, child: Text(_shutdownMessage,
            key: const ValueKey('shutdownStatus'))),
          const SizedBox(height: 16),
          Text(_micUnknown
              ? 'Microphone state unknown. Last confirmed: ${_isRecording ? 'On' : 'Off'}.'
              : 'Last verified microphone: ${_isRecording ? 'On' : 'Off'}.'),
          const SizedBox(height: 16),
          if (pending) const Center(child: CircularProgressIndicator(
            semanticsLabel: 'Stopping audio and disconnecting')),
          if (!pending) FilledButton.icon(
            key: const ValueKey('retryShutdown'),
            focusNode: _shutdownRetryFocus,
            autofocus: true,
            onPressed: _disconnect,
            icon: const Icon(Icons.stop_rounded),
            label: const Text('Stop audio and retry disconnect'),
          ),
        ],
      ),
    );
  }

  // ---- Connected View ----
  Widget _buildConnectedView(ColorScheme cs) {
    return Center(
      child: SingleChildScrollView(
        padding: const EdgeInsets.symmetric(horizontal: 32, vertical: 24),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            // Speaker icon
            Container(
              width: 100,
              height: 100,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                gradient: _isPaused
                    ? LinearGradient(
                        colors: [
                          cs.surfaceContainerHighest,
                          cs.surfaceContainerHighest,
                        ],
                      )
                    : LinearGradient(
                        colors: [cs.primary, cs.tertiary],
                        begin: Alignment.topLeft,
                        end: Alignment.bottomRight,
                      ),
                boxShadow: _isPaused
                    ? []
                    : [
                        BoxShadow(
                          color: cs.primary.withOpacity(0.3),
                          blurRadius: 24,
                          spreadRadius: 2,
                        ),
                      ],
              ),
              child: Icon(
                _isPaused
                    ? Icons.pause_rounded
                    : (_isMuted || _volume == 0.0
                          ? Icons.volume_off_rounded
                          : (_volume < 0.5
                                ? Icons.volume_down_rounded
                                : Icons.volume_up_rounded)),
                size: 48,
                color: _isPaused ? cs.onSurfaceVariant : Colors.white,
              ),
            ),
            const SizedBox(height: 24),

            Semantics(
              liveRegion: true,
              child: Text(
                _isPaused ? 'Paused' : 'Streaming',
                semanticsLabel: _isPaused
                    ? 'Playback paused'
                    : 'Streaming from $_serverName',
                style: Theme.of(context).textTheme.headlineSmall?.copyWith(
                  fontWeight: FontWeight.w300,
                  color: cs.onSurfaceVariant,
                ),
              ),
            ),
            const SizedBox(height: 4),
            Text(
              _serverName,
              textAlign: TextAlign.center,
              style: Theme.of(
                context,
              ).textTheme.headlineMedium?.copyWith(fontWeight: FontWeight.bold),
            ),
            const SizedBox(height: 6),
            // Server address + encryption badge
            Wrap(
              alignment: WrapAlignment.center,
              crossAxisAlignment: WrapCrossAlignment.center,
              spacing: 8,
              runSpacing: 8,
              children: [
                Text(
                  _serverAddress,
                  style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                    color: cs.onSurfaceVariant,
                    fontFamily: 'monospace',
                  ),
                ),
                const SizedBox(width: 8),
                Container(
                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                  decoration: BoxDecoration(
                    color: _isEncrypted
                        ? Colors.green.withOpacity(0.15)
                        : Colors.orange.withOpacity(0.15),
                    borderRadius: BorderRadius.circular(6),
                    border: Border.all(
                      color: _isEncrypted
                          ? Colors.green.withOpacity(0.4)
                          : Colors.orange.withOpacity(0.4),
                      width: 0.5,
                    ),
                  ),
                  child: Row(
                    mainAxisSize: MainAxisSize.min,
                    children: [
                      Icon(
                        _isEncrypted
                            ? Icons.lock_rounded
                            : Icons.lock_open_rounded,
                        size: 11,
                        color: _isEncrypted
                            ? Colors.green.shade300
                            : Colors.orange.shade200,
                      ),
                      const SizedBox(width: 3),
                      Text(
                        _isEncrypted ? 'DTLS' : 'Open',
                        style: TextStyle(
                          fontSize: 14,
                          fontWeight: FontWeight.w600,
                          color: _isEncrypted
                              ? Colors.green.shade300
                              : Colors.orange.shade200,
                        ),
                      ),
                    ],
                  ),
                ),
              ],
            ),
            const SizedBox(height: 28),

            // Latency pill (tappable for breakdown)
            LabeledSurfaceButton(
              key: const ValueKey('latencyBreakdown'),
              label:
                  'Show latency breakdown. Estimated latency: ${_latencyMs.toStringAsFixed(0)} milliseconds.',
              onPressed: () => _showLatencyBreakdown(cs),
              child: Container(
                padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 10),
                decoration: BoxDecoration(
                  color: cs.surfaceContainerHighest,
                  borderRadius: BorderRadius.circular(12),
                  border: Border.all(color: cs.outlineVariant.withOpacity(0.2)),
                ),
                child: Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Icon(Icons.speed_rounded, size: 16, color: cs.primary),
                    const SizedBox(width: 8),
                    Text(
                      '${_latencyMs.toStringAsFixed(0)} ms',
                      style: Theme.of(context).textTheme.titleMedium?.copyWith(
                        fontFamily: 'monospace',
                        fontWeight: FontWeight.w600,
                      ),
                    ),
                    const SizedBox(width: 6),
                    Icon(
                      Icons.expand_more_rounded,
                      size: 16,
                      color: cs.onSurfaceVariant,
                    ),
                  ],
                ),
              ),
            ),
            const SizedBox(height: 24),

            // Now playing info
            if (_nowPlayingTitle.isNotEmpty) ...[
              LabeledSurfaceButton(
                key: const ValueKey('refreshMediaInfo'),
                label:
                    'Refresh now playing. $_nowPlayingTitle. $_nowPlayingArtist',
                semanticValue: _nowPlayingDurationMs > 0
                    ? '${_spokenDuration(_nowPlayingPositionMs)} elapsed; ${_spokenDuration(_nowPlayingDurationMs)} total; ${((_nowPlayingPositionMs / _nowPlayingDurationMs).clamp(0.0, 1.0) * 100).round()} percent complete'
                    : null,
                onPressed: _requestMediaInfo,
                child: Container(
                  width: double.infinity,
                  margin: const EdgeInsets.symmetric(horizontal: 20),
                  padding: const EdgeInsets.all(16),
                  decoration: BoxDecoration(
                    color: cs.surfaceContainerHighest,
                    borderRadius: BorderRadius.circular(14),
                    border: Border.all(color: cs.outlineVariant.withOpacity(0.2)),
                  ),
                  child: Column(
                    children: [
                      Text(
                        _nowPlayingTitle,
                        style: Theme.of(context).textTheme.titleMedium?.copyWith(
                          fontWeight: FontWeight.w700,
                        ),
                        textAlign: TextAlign.center,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                      ),
                      if (_nowPlayingArtist.isNotEmpty) ...[
                        const SizedBox(height: 4),
                        Text(
                          _nowPlayingArtist,
                          style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                            color: cs.onSurfaceVariant,
                          ),
                          textAlign: TextAlign.center,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                        ),
                      ],
                      if (_nowPlayingDurationMs > 0) ...[
                        const SizedBox(height: 12),
                        ClipRRect(
                          borderRadius: BorderRadius.circular(2),
                          child: LinearProgressIndicator(
                            value: (_nowPlayingPositionMs / _nowPlayingDurationMs)
                                .clamp(0.0, 1.0),
                            minHeight: 4,
                            backgroundColor: cs.outlineVariant.withOpacity(0.3),
                            valueColor: AlwaysStoppedAnimation(cs.primary),
                          ),
                        ),
                        const SizedBox(height: 6),
                        Wrap(
                          alignment: WrapAlignment.spaceBetween,
                          spacing: 16,
                          runSpacing: 4,
                          children: [
                            Text(
                              _formatDuration(_nowPlayingPositionMs),
                              style: Theme.of(context).textTheme.bodyMedium
                                  ?.copyWith(
                                    color: cs.onSurfaceVariant,
                                    fontFamily: 'monospace',
                                    fontSize: 14,
                                  ),
                            ),
                            Text(
                              _formatDuration(_nowPlayingDurationMs),
                              style: Theme.of(context).textTheme.bodyMedium
                                  ?.copyWith(
                                    color: cs.onSurfaceVariant,
                                    fontFamily: 'monospace',
                                    fontSize: 14,
                                  ),
                            ),
                          ],
                        ),
                      ],
                    ],
                  ),
                ),
              ),
              const SizedBox(height: 16),
            ],

            if (_connectedPhones > 1)
              Text(
                'Streaming to $_connectedPhones phones',
                style: Theme.of(context).textTheme.bodyMedium,
              ),
            PhoneAudioControls(
              canControlMedia: _desktopControls.canEdit,
              mediaBusy: _desktopControls.busy,
              mediaUnavailableMessage: _desktopControls.busy
                  ? 'Waiting for the PC command to finish. Media controls are temporarily paused; phone audio controls remain available.'
                  : !_desktopControls.supported
                  ? 'Waiting for a fresh PC permission status. Media editing is disabled; phone audio controls remain available.'
                  : 'Read only: PC media controls require Admin permission. Phone audio controls remain available.',
              mediaPlaying: _nowPlayingStatus == 1,
              recording: _isRecording,
              micAllowed: _micAllowed,
              micBusy: _micBusy,
              micUnknown: _micUnknown,
              micMessage: _micMessage,
              playbackMessage: _playbackMessage,
              mediaMessage: _desktopControls.mediaMessage,
              muted: _isMuted,
              volume: _volume,
              micSource: _micSource,
              onPrevious: () => _sendMediaCommand(0x12),
              onPlayPause: _mediaPlayPause,
              onNext: () => _sendMediaCommand(0x11),
              onToggleMic: () => _toggleMic(_micUnknown ? false : !_isRecording),
              onToggleMute: _toggleMute,
              onVolume: _setVolume,
              onMicSource: _switchMicSource,
              onDisconnect: _disconnect,
            ),
            const SizedBox(height: 24),
            DesktopSettingsPanel(controls: _desktopControls),
          ],
        ),
      ),
    );
  }

  String _spokenDuration(int ms) {
    final totalSeconds = max(0, ms ~/ 1000);
    final hours = totalSeconds ~/ 3600;
    final minutes = totalSeconds % 3600 ~/ 60;
    final seconds = totalSeconds % 60;
    return [
      if (hours > 0) '$hours ${hours == 1 ? 'hour' : 'hours'}',
      if (minutes > 0) '$minutes ${minutes == 1 ? 'minute' : 'minutes'}',
      if (seconds > 0 || totalSeconds == 0)
        '$seconds ${seconds == 1 ? 'second' : 'seconds'}',
    ].join(', ');
  }

  String _formatDuration(int ms) {
    final totalSeconds = ms ~/ 1000;
    final minutes = totalSeconds ~/ 60;
    final seconds = totalSeconds % 60;
    return '$minutes:${seconds.toString().padLeft(2, '0')}';
  }

  // ---- Main View (disconnected / scanning) ----
  Widget _buildMainView(ColorScheme cs) {
    final reduceMotion = MediaQuery.disableAnimationsOf(context);
    final isScanning = _state == ConnectionState_.discovering;
    final isConnecting = _state == ConnectionState_.connecting;
    final isPairPending = _state == ConnectionState_.pairPending;
    final recentServers = _knownServers.reversed.take(10).toList();

    return SingleChildScrollView(
      child: Column(
        children: [
          // Header
          Padding(
            padding: const EdgeInsets.fromLTRB(24, 20, 24, 0),
            child: Wrap(
              crossAxisAlignment: WrapCrossAlignment.center,
              children: [
                Container(
                  width: 40,
                  height: 40,
                  decoration: BoxDecoration(
                    borderRadius: BorderRadius.circular(10),
                    gradient: LinearGradient(
                      colors: [cs.primary, cs.tertiary],
                      begin: Alignment.topLeft,
                      end: Alignment.bottomRight,
                    ),
                  ),
                  child: const Icon(
                    Icons.headphones_rounded,
                    size: 22,
                    color: Colors.white,
                  ),
                ),
                const SizedBox(width: 12),
                Text(
                  'AudioBridge',
                  style: Theme.of(
                    context,
                  ).textTheme.titleLarge?.copyWith(fontWeight: FontWeight.bold),
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),

          // Device name (tappable to edit)
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 24),
            child: LabeledSurfaceButton(
              key: const ValueKey('editDeviceName'),
              label: 'Edit device name. Current name: $_deviceName',
              onPressed: _showEditDeviceNameDialog,
              child: Container(
                width: double.infinity,
                padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
                decoration: BoxDecoration(
                  color: cs.surfaceContainerHighest,
                  borderRadius: BorderRadius.circular(10),
                  border: Border.all(color: cs.outlineVariant.withOpacity(0.2)),
                ),
                child: Row(
                  children: [
                    Icon(
                      Icons.badge_outlined,
                      size: 18,
                      color: cs.onSurfaceVariant,
                    ),
                    const SizedBox(width: 10),
                    Expanded(
                      child: Text(
                        _deviceName,
                        style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                          fontWeight: FontWeight.w500,
                        ),
                      ),
                    ),
                    Icon(
                      Icons.edit_outlined,
                      size: 16,
                      color: cs.onSurfaceVariant,
                    ),
                  ],
                ),
              ),
            ),
          ),
          const SizedBox(height: 16),

          if (!isScanning)
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 24, vertical: 16),
              child: Semantics(
                liveRegion: true,
                child: Text(
                  _statusMessage,
                  semanticsLabel: isPairPending
                      ? _statusMessage.replaceAllMapped(
                          RegExp(r'[0-9]{6}'),
                          (match) => match[0]!.split('').join(' '),
                        )
                      : null,
                  textAlign: TextAlign.center,
                  style: isPairPending
                      ? Theme.of(context).textTheme.titleLarge
                      : Theme.of(context).textTheme.bodyMedium,
                ),
              ),
            ),

          // Scan button
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 24),
            child: SizedBox(
              width: double.infinity,
              child: FilledButton.icon(
                onPressed: (isConnecting || isPairPending)
                    ? null
                    : _startDiscovery,
                icon: reduceMotion && (isScanning || isConnecting)
                    ? Icon(
                        isScanning
                            ? Icons.radar_rounded
                            : Icons.hourglass_empty_rounded,
                      )
                    : isScanning
                    ? SizedBox(
                        width: 20,
                        height: 20,
                        child: CircularProgressIndicator(
                          strokeWidth: 2.5,
                          color: cs.onPrimary,
                        ),
                      )
                    : isConnecting
                    ? SizedBox(
                        width: 20,
                        height: 20,
                        child: CircularProgressIndicator(
                          strokeWidth: 2.5,
                          color: cs.onSurfaceVariant,
                        ),
                      )
                    : const Icon(Icons.radar_rounded),
                label: Text(
                  isConnecting
                      ? 'Connecting...'
                      : isPairPending
                      ? 'Awaiting approval...'
                      : isScanning
                      ? 'Stop Scanning'
                      : 'Scan for Servers',
                  style: const TextStyle(
                    fontSize: 16,
                    fontWeight: FontWeight.w600,
                  ),
                ),
                style: FilledButton.styleFrom(
                  minimumSize: const Size(48, 56),
                  padding: const EdgeInsets.all(16),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(14),
                  ),
                ),
              ),
            ),
          ),

          if (isScanning)
            Padding(
              padding: const EdgeInsets.only(top: 8),
              child: Text(
                'Tap again to stop',
                style: Theme.of(
                  context,
                ).textTheme.bodySmall?.copyWith(color: cs.onSurfaceVariant),
              ),
            ),

          const SizedBox(height: 20),

          // Content
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 24),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                if (isScanning && _discoveredServers.isNotEmpty) ...[
                  _buildSectionHeader(
                    cs,
                    'Available Servers',
                    Icons.wifi_rounded,
                  ),
                  const SizedBox(height: 8),
                  ..._discoveredServers.values.map(
                    (server) => _buildDiscoveredServerCard(cs, server),
                  ),
                  const SizedBox(height: 20),
                ],

                if (isScanning && _discoveredServers.isEmpty) ...[
                  Padding(
                    padding: const EdgeInsets.symmetric(vertical: 32),
                    child: Center(
                      child: Column(
                        children: [
                          Icon(
                            Icons.radar_rounded,
                            size: 48,
                            color: cs.onSurface.withOpacity(0.15),
                          ),
                          const SizedBox(height: 12),
                          Text(
                            'Looking for servers on your network...',
                            style: Theme.of(context).textTheme.bodyMedium
                                ?.copyWith(color: cs.onSurfaceVariant),
                          ),
                        ],
                      ),
                    ),
                  ),
                ],

                if (recentServers.isNotEmpty &&
                    !isConnecting &&
                    !isPairPending) ...[
                  _buildSectionHeader(cs, 'Recent', Icons.history_rounded),
                  const SizedBox(height: 8),
                  ...recentServers.map((server) => _buildHistoryCard(cs, server)),
                  const SizedBox(height: 24),
                ],

                if (recentServers.isEmpty &&
                    !isScanning &&
                    !isConnecting &&
                    !isPairPending)
                  Padding(
                    padding: const EdgeInsets.symmetric(vertical: 48),
                    child: Center(
                      child: Column(
                        children: [
                          Icon(
                            Icons.cast_rounded,
                            size: 64,
                            color: cs.onSurface.withOpacity(0.12),
                          ),
                          const SizedBox(height: 16),
                          Text(
                            'No previous connections',
                            style: Theme.of(context).textTheme.bodyLarge
                                ?.copyWith(color: cs.onSurfaceVariant),
                          ),
                          const SizedBox(height: 6),
                          Text(
                            'Scan to find AudioBridge servers\non your local network',
                            textAlign: TextAlign.center,
                            style: Theme.of(context).textTheme.bodySmall
                                ?.copyWith(color: cs.onSurfaceVariant),
                          ),
                        ],
                      ),
                    ),
                  ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildSectionHeader(ColorScheme cs, String title, IconData icon) {
    return Row(
      children: [
        Icon(icon, size: 16, color: cs.onSurfaceVariant),
        const SizedBox(width: 6),
        Text(
          title,
          style: Theme.of(context).textTheme.labelLarge?.copyWith(
                color: cs.onSurfaceVariant,
                fontWeight: FontWeight.w600,
                letterSpacing: 0.5,
              ),
        ),
      ],
    );
  }

  Widget _buildDiscoveredServerCard(ColorScheme cs, _DiscoveredServer server) {
    final knownEntry = _knownServers
        .where((s) => s.macAddress == server.mac)
        .toList();
    final isKnown = knownEntry.isNotEmpty;
    final nameMismatch = _knownServers.any(
      (s) =>
          s.name == server.name &&
          s.macAddress != server.mac &&
          server.mac.isNotEmpty,
    );

    return MergeSemantics(
      child: Semantics(
        button: true,
        child: Container(
          margin: const EdgeInsets.only(bottom: 8),
          decoration: BoxDecoration(
            color: cs.surfaceContainerHigh,
            borderRadius: BorderRadius.circular(14),
            border: Border.all(
              color: nameMismatch
                  ? Colors.orange.withOpacity(0.4)
                  : isKnown
                  ? cs.primary.withOpacity(0.3)
                  : cs.outlineVariant.withOpacity(0.2),
            ),
          ),
          child: InkWell(
            onTap: () => _connectToDiscovered(server),
            borderRadius: BorderRadius.circular(14),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
              child: Row(
                children: [
                  Container(
                    width: 42,
                    height: 42,
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      color: nameMismatch
                          ? Colors.orange.withOpacity(0.15)
                          : isKnown
                          ? cs.primary.withOpacity(0.15)
                          : cs.surfaceContainerHighest,
                    ),
                    child: Icon(
                      nameMismatch
                          ? Icons.warning_amber_rounded
                          : isKnown
                          ? Icons.verified_rounded
                          : Icons.computer_rounded,
                      size: 20,
                      color: nameMismatch
                          ? Colors.orange
                          : isKnown
                          ? cs.primary
                          : cs.onSurfaceVariant,
                    ),
                  ),
                  const SizedBox(width: 14),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          server.name,
                          style: Theme.of(context).textTheme.titleSmall?.copyWith(
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                        const SizedBox(height: 2),
                        Text(
                          nameMismatch
                              ? '${server.ip} · MAC changed'
                              : isKnown
                              ? '${server.ip} · Verified'
                              : server.ip,
                          style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                            color: nameMismatch
                                ? Colors.orange.withOpacity(0.8)
                                : cs.onSurfaceVariant,
                          ),
                        ),
                      ],
                    ),
                  ),
                  Icon(
                    Icons.arrow_forward_ios_rounded,
                    size: 16,
                    color: cs.onSurfaceVariant,
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }

  Widget _buildHistoryCard(ColorScheme cs, ServerInfo server) {
    return MergeSemantics(
      child: Semantics(
        button: true,
        child: Container(
          margin: const EdgeInsets.only(bottom: 8),
          decoration: BoxDecoration(
            color: cs.surfaceContainerHigh,
            borderRadius: BorderRadius.circular(14),
            border: Border.all(color: cs.outlineVariant.withOpacity(0.15)),
          ),
          child: InkWell(
            onTap: () => _reconnectToServer(server),
            borderRadius: BorderRadius.circular(14),
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 14),
              child: Row(
                children: [
                  Container(
                    width: 42,
                    height: 42,
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      color: cs.surfaceContainerHighest,
                    ),
                    child: Icon(
                      Icons.computer_rounded,
                      size: 20,
                      color: cs.onSurfaceVariant,
                    ),
                  ),
                  const SizedBox(width: 14),
                  Expanded(
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text(
                          server.name,
                          style: Theme.of(context).textTheme.titleSmall?.copyWith(
                            fontWeight: FontWeight.w600,
                          ),
                        ),
                        const SizedBox(height: 2),
                        Text(
                          '${server.lastIp} · ${_timeAgo(server.lastConnected)}',
                          style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                            color: cs.onSurfaceVariant,
                          ),
                        ),
                      ],
                    ),
                  ),
                  Icon(
                    Icons.arrow_forward_ios_rounded,
                    size: 16,
                    color: cs.onSurfaceVariant,
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

class _DiscoveredServer {
  final String name;
  final String ip;
  final int port;
  final String mac;

  _DiscoveredServer({
    required this.name,
    required this.ip,
    required this.port,
    required this.mac,
  });
}
