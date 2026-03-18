import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'dart:math';

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
}

/// Represents a previously seen server
class ServerInfo {
  String name;
  String macAddress;
  String lastIp;
  DateTime lastConnected;
  bool active; // currently discovered on LAN
  bool macVerified; // MAC matches stored record

  ServerInfo({
    required this.name,
    required this.macAddress,
    required this.lastIp,
    required this.lastConnected,
    this.active = false,
    this.macVerified = false,
  });

  Map<String, dynamic> toJson() => {
        'name': name,
        'macAddress': macAddress,
        'lastIp': lastIp,
        'lastConnected': lastConnected.toIso8601String(),
      };

  factory ServerInfo.fromJson(Map<String, dynamic> json) => ServerInfo(
        name: json['name'] ?? '',
        macAddress: json['macAddress'] ?? '',
        lastIp: json['lastIp'] ?? '',
        lastConnected: DateTime.tryParse(json['lastConnected'] ?? '') ?? DateTime.now(),
      );
}

class AudioBridgePage extends StatefulWidget {
  const AudioBridgePage({super.key});

  @override
  State<AudioBridgePage> createState() => _AudioBridgePageState();
}

class _AudioBridgePageState extends State<AudioBridgePage> with SingleTickerProviderStateMixin {
  static const _channel = MethodChannel('com.audiobridge/audio');

  ConnectionState_ _state = ConnectionState_.disconnected;
  String _statusMessage = 'Tap Connect to find a server';
  String _serverName = '';
  String _serverAddress = '';
  String _serverMac = '';
  double _latencyMs = 0.0;
  String _deviceId = '';
  String _deviceName = 'AudioBridge';
  bool _isRecording = true; // mic on by default
  bool _isMuted = false; // audio output mute
  bool _isPaused = false; // stream paused
  String _micSource = 'auto'; // 'auto', 'phone', 'bluetooth'

  // Now playing info from server
  String _nowPlayingTitle = '';
  String _nowPlayingArtist = '';
  String _nowPlayingAlbum = '';
  int _nowPlayingStatus = 0; // 0=stopped, 1=playing, 2=paused
  int _nowPlayingPositionMs = 0;
  int _nowPlayingDurationMs = 0;

  RawDatagramSocket? _discoverySocket;
  RawDatagramSocket? _audioSocket;
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
    _pulseController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1500),
    );
    _loadHistory();
    _loadOrCreateDeviceId();
    _loadDeviceName();

    // Handle media actions from notification
    _channel.setMethodCallHandler((call) async {
      if (call.method == 'onMediaAction') {
        final action = call.arguments as String?;
        switch (action) {
          case 'toggleMic':
            _toggleMic(!_isRecording);
            break;
          case 'togglePause':
            _mediaPlayPause();
            break;
          case 'disconnect':
            _disconnect();
            break;
        }
      }
      return null;
    });
  }

  @override
  void dispose() {
    _pulseController.dispose();
    _disconnect();
    super.dispose();
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

  Future<void> _saveDeviceName(String name) async {
    try {
      final dir = await _appDir;
      final file = File('${dir.path}/device_name.txt');
      await file.writeAsString(name);
      setState(() => _deviceName = name);
    } catch (_) {}
  }

  void _showEditDeviceNameDialog() {
    final controller = TextEditingController(text: _deviceName);
    showDialog(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Device Name'),
        content: TextField(
          controller: controller,
          autofocus: true,
          maxLength: 30,
          decoration: const InputDecoration(
            hintText: 'Enter device name',
          ),
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx),
            child: const Text('Cancel'),
          ),
          TextButton(
            onPressed: () {
              final name = controller.text.trim();
              if (name.isNotEmpty) {
                _saveDeviceName(name);
              }
              Navigator.pop(ctx);
            },
            child: const Text('Save'),
          ),
        ],
      ),
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

  void _recordConnection(String name, String mac, String ip) {
    final existing = _knownServers.where((s) => s.macAddress == mac).toList();
    if (existing.isNotEmpty) {
      existing.first.name = name;
      existing.first.lastIp = ip;
      existing.first.lastConnected = DateTime.now();
    } else {
      _knownServers.add(ServerInfo(
        name: name,
        macAddress: mac,
        lastIp: ip,
        lastConnected: DateTime.now(),
      ));
    }
    _saveHistory();
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
    _pulseController.repeat();

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
    _pulseController.stop();
    _pulseController.reset();
  }

  // --- Connection ---

  Future<void> _connectToServer(InternetAddress address, int port, String name, String mac) async {
    setState(() {
      _state = ConnectionState_.connecting;
      _statusMessage = 'Connecting to $name...';
      _serverName = name;
      _serverMac = mac;
      _serverAddress = address.address;
      _isPaused = false;
      _isMuted = false;
    });

    _stopDiscovery();

    try {
      _audioSocket = await RawDatagramSocket.bind(InternetAddress.anyIPv4, 0);
      final completer = Completer<String>();
      _audioSocket!.listen((event) {
        if (event == RawSocketEvent.read) {
          final datagram = _audioSocket!.receive();
          if (datagram != null) {
            _lastPacketTime = DateTime.now();
            final msg = String.fromCharCodes(datagram.data);
            if (msg == 'AB_ACCEPT' && !completer.isCompleted) {
              completer.complete('accepted');
              return;
            }
            if (msg == 'AB_PAIR_PENDING' && !completer.isCompleted) {
              if (mounted) {
                setState(() {
                  _state = ConnectionState_.pairPending;
                  _statusMessage = 'Waiting for server approval...';
                });
              }
              return;
            }
            if (msg.startsWith('AB_REJECT') && !completer.isCompleted) {
              final reason = msg.contains('|') ? msg.split('|')[1] : 'Connection denied';
              completer.complete('rejected:$reason');
              return;
            }
            _handlePacket(datagram.data);
          }
        }
      });

      final connectMsg = Uint8List.fromList('AB_CONNECT|$_deviceName|$_deviceId'.codeUnits);
      _audioSocket!.send(connectMsg, address, port);

      final result = await completer.future.timeout(
        const Duration(seconds: 35),
        onTimeout: () => 'timeout',
      );

      if (result != 'accepted') {
        String message;
        if (result == 'timeout') {
          message = 'Server did not respond';
        } else if (result.startsWith('rejected:')) {
          message = result.substring(9);
        } else {
          message = 'Connection failed';
        }
        setState(() {
          _state = ConnectionState_.disconnected;
          _statusMessage = message;
        });
        _audioSocket?.close();
        _audioSocket = null;
        return;
      }

      await _channel.invokeMethod('startAudio', {'serverName': name});
      if (_isRecording) {
        try {
          await _channel.invokeMethod('startRecording', {
            'ip': address.address,
            'port': port,
          });
        } catch (_) {}
      }

      _recordConnection(name, mac, address.address);

      setState(() {
        _state = ConnectionState_.connected;
        _statusMessage = 'Connected to $_serverName';
      });

      _updateServiceState();

      _keepaliveTimer = Timer.periodic(const Duration(milliseconds: 1000), (_) {
        _sendKeepalive(address, port);
      });

      _latencyTimer = Timer.periodic(const Duration(milliseconds: 500), (_) async {
        try {
          final latency = await _channel.invokeMethod<double>('getLatency');
          if (latency != null && mounted) {
            setState(() => _latencyMs = latency);
          }
        } catch (_) {}
      });

      _timeoutTimer = Timer.periodic(const Duration(seconds: 1), (_) {
        if (_lastPacketTime != null &&
            DateTime.now().difference(_lastPacketTime!).inSeconds > 5) {
          _disconnect();
          if (mounted) {
            ScaffoldMessenger.of(context).showSnackBar(
              const SnackBar(
                content: Text('Server disconnected (timeout)'),
                behavior: SnackBarBehavior.floating,
              ),
            );
          }
        }
      });
    } catch (e) {
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

  void _handlePacket(Uint8List data) {
    if (data.length < 16) return;

    final version = data[0];
    final type = data[1];

    if (version != 0x01) return;

    if (type == 0x01) {
      _channel.invokeMethod('feedAudio', data);
    } else if (type == 0x03) {
      if (data.length >= 17 && data[16] == 0x03) {
        _disconnect();
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            const SnackBar(
              content: Text('Server disconnected'),
              behavior: SnackBarBehavior.floating,
            ),
          );
        }
      }
    } else if (type == 0x05 && data.length > 16) {
      // Media info packet
      try {
        final jsonStr = utf8.decode(data.sublist(16));
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
        }
      } catch (_) {}
    }
  }

  void _sendKeepalive(InternetAddress address, int port) {
    final packet = Uint8List(16);
    final view = ByteData.view(packet.buffer);
    packet[0] = 0x01;
    packet[1] = 0x02;
    view.setUint32(2, 0, Endian.little);
    view.setUint64(6, 0, Endian.little);
    view.setUint16(14, 0, Endian.little);
    _audioSocket?.send(packet, address, port);
  }

  // --- Media Controls ---

  Future<void> _toggleMic(bool enable) async {
    setState(() {
      _isRecording = enable;
    });

    if (_state != ConnectionState_.connected || _serverAddress.isEmpty) return;

    if (enable) {
      try {
        await _channel.invokeMethod('startRecording', {
          'ip': _serverAddress,
          'port': 4012,
          'micSource': _micSource,
        });
      } catch (e) {
        if (mounted) {
          ScaffoldMessenger.of(context).showSnackBar(
            SnackBar(content: Text('Failed to start mic: $e')),
          );
        }
      }
    } else {
      try {
        await _channel.invokeMethod('stopRecording');
      } catch (_) {}
    }
    _updateServiceState();
  }

  Future<void> _switchMicSource(String source) async {
    if (source == _micSource) return;
    setState(() {
      _micSource = source;
    });
    // Restart recording with new source if currently recording
    if (_isRecording && _state == ConnectionState_.connected) {
      try {
        await _channel.invokeMethod('stopRecording');
        await _channel.invokeMethod('startRecording', {
          'ip': _serverAddress,
          'port': 4012,
          'micSource': _micSource,
        });
      } catch (_) {}
    }
  }

  Future<void> _toggleMute() async {
    setState(() {
      _isMuted = !_isMuted;
    });
    try {
      await _channel.invokeMethod('setMuted', _isMuted);
    } catch (_) {}
  }

  void _mediaPlayPause() {
    _sendMediaCommand(0x10); // CTRL_MEDIA_PLAY_PAUSE
  }

  void _sendMediaCommand(int cmd) {
    if (_state != ConnectionState_.connected || _serverAddress.isEmpty) return;
    _sendControlCommand(cmd);
    // Request fresh media info after a short delay for the action to take effect
    Future.delayed(const Duration(milliseconds: 300), _requestMediaInfo);
  }

  void _requestMediaInfo() {
    if (_state != ConnectionState_.connected || _serverAddress.isEmpty) return;
    _sendControlCommand(0x13); // CTRL_MEDIA_INFO_REQ
  }

  void _sendControlCommand(int cmd) {
    final packet = Uint8List(17);
    packet[0] = 0x01; // version
    packet[1] = 0x03; // control
    packet[14] = 1; // payload length
    packet[15] = 0;
    packet[16] = cmd;

    try {
      _audioSocket?.send(packet, InternetAddress(_serverAddress), 4012);
    } catch (_) {}
  }

  void _updateServiceState() {
    try {
      _channel.invokeMethod('updateServiceState', {
        'micMuted': !_isRecording,
        'paused': _isPaused,
        'serverName': _serverName,
      });
    } catch (_) {}
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
      await _channel.invokeMethod('stopRecording');
    } catch (_) {}

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
        _serverName = '';
        _serverAddress = '';
        _serverMac = '';
        _statusMessage = 'Disconnected';
        _isPaused = false;
        _isMuted = false;
        _nowPlayingTitle = '';
        _nowPlayingArtist = '';
        _nowPlayingAlbum = '';
        _nowPlayingStatus = 0;
        _nowPlayingPositionMs = 0;
        _nowPlayingDurationMs = 0;
      });
    }
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
      body: SafeArea(
        child: _state == ConnectionState_.connected
            ? _buildConnectedView(cs)
            : _buildMainView(cs),
      ),
    );
  }

  // ---- Connected View ----
  Widget _buildConnectedView(ColorScheme cs) {
    return Center(
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 32),
        child: Column(
          mainAxisAlignment: MainAxisAlignment.center,
          children: [
            // Speaker icon
            Container(
              width: 100,
              height: 100,
              decoration: BoxDecoration(
                shape: BoxShape.circle,
                gradient: _isPaused
                    ? LinearGradient(colors: [cs.surfaceContainerHighest, cs.surfaceContainerHighest])
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
                    : (_isMuted ? Icons.volume_off_rounded : Icons.volume_up_rounded),
                size: 48,
                color: _isPaused ? cs.onSurface.withOpacity(0.4) : Colors.white,
              ),
            ),
            const SizedBox(height: 24),

            Text(
              _isPaused ? 'Paused' : 'Streaming',
              style: Theme.of(context).textTheme.headlineSmall?.copyWith(
                    fontWeight: FontWeight.w300,
                    color: cs.onSurface.withOpacity(0.5),
                  ),
            ),
            const SizedBox(height: 4),
            Text(
              _serverName,
              style: Theme.of(context).textTheme.headlineMedium?.copyWith(
                    fontWeight: FontWeight.bold,
                  ),
            ),
            const SizedBox(height: 2),
            Text(
              _serverAddress,
              style: Theme.of(context).textTheme.bodySmall?.copyWith(
                    color: cs.onSurface.withOpacity(0.35),
                    fontFamily: 'monospace',
                  ),
            ),
            const SizedBox(height: 28),

            // Latency pill
            Container(
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
                ],
              ),
            ),
            const SizedBox(height: 24),

            // Now playing info
            if (_nowPlayingTitle.isNotEmpty) ...[
              GestureDetector(
              onTap: _requestMediaInfo,
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
                              color: cs.onSurface.withOpacity(0.6),
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
                          value: (_nowPlayingPositionMs / _nowPlayingDurationMs).clamp(0.0, 1.0),
                          minHeight: 4,
                          backgroundColor: cs.outlineVariant.withOpacity(0.3),
                          valueColor: AlwaysStoppedAnimation(cs.primary),
                        ),
                      ),
                      const SizedBox(height: 6),
                      Row(
                        mainAxisAlignment: MainAxisAlignment.spaceBetween,
                        children: [
                          Text(
                            _formatDuration(_nowPlayingPositionMs),
                            style: Theme.of(context).textTheme.bodySmall?.copyWith(
                                  color: cs.onSurface.withOpacity(0.5),
                                  fontFamily: 'monospace',
                                  fontSize: 11,
                                ),
                          ),
                          Text(
                            _formatDuration(_nowPlayingDurationMs),
                            style: Theme.of(context).textTheme.bodySmall?.copyWith(
                                  color: cs.onSurface.withOpacity(0.5),
                                  fontFamily: 'monospace',
                                  fontSize: 11,
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

            // Media transport controls
            Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                // Skip back
                _buildControlButton(
                  icon: Icons.skip_previous_rounded,
                  label: 'Prev',
                  isActive: true,
                  activeColor: cs.primary,
                  onTap: () => _sendMediaCommand(0x12), // CTRL_MEDIA_PREV
                  cs: cs,
                ),
                const SizedBox(width: 12),

                // Play/Pause button (larger, center)
                GestureDetector(
                  onTap: _mediaPlayPause,
                  child: Container(
                    width: 72,
                    height: 72,
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      color: cs.primary,
                      border: Border.all(
                        color: cs.primary,
                        width: 2,
                      ),
                    ),
                    child: Icon(
                      Icons.play_arrow_rounded,
                      size: 36,
                      color: cs.onPrimary,
                    ),
                  ),
                ),
                const SizedBox(width: 12),

                // Skip forward
                _buildControlButton(
                  icon: Icons.skip_next_rounded,
                  label: 'Next',
                  isActive: true,
                  activeColor: cs.primary,
                  onTap: () => _sendMediaCommand(0x11), // CTRL_MEDIA_NEXT
                  cs: cs,
                ),
              ],
            ),
            const SizedBox(height: 16),

            // Mic & Volume controls row
            Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                _buildControlButton(
                  icon: _isRecording ? Icons.mic_rounded : Icons.mic_off_rounded,
                  label: _isRecording ? 'Mic On' : 'Mic Off',
                  isActive: _isRecording,
                  activeColor: cs.primary,
                  onTap: () => _toggleMic(!_isRecording),
                  cs: cs,
                ),
                const SizedBox(width: 24),
                _buildControlButton(
                  icon: _isMuted ? Icons.volume_off_rounded : Icons.volume_up_rounded,
                  label: _isMuted ? 'Muted' : 'Sound',
                  isActive: !_isMuted,
                  activeColor: cs.primary,
                  onTap: _toggleMute,
                  cs: cs,
                ),
              ],
            ),

            const SizedBox(height: 20),

            // Mic source selector
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
              decoration: BoxDecoration(
                color: cs.surfaceContainerHighest,
                borderRadius: BorderRadius.circular(12),
                border: Border.all(color: cs.outlineVariant.withOpacity(0.2)),
              ),
              child: DropdownButtonHideUnderline(
                child: DropdownButton<String>(
                  value: _micSource,
                  isDense: true,
                  dropdownColor: cs.surfaceContainerHighest,
                  icon: Icon(Icons.expand_more_rounded, size: 20, color: cs.onSurface.withOpacity(0.5)),
                  style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                    color: cs.onSurface.withOpacity(0.7),
                  ),
                  items: const [
                    DropdownMenuItem(value: 'auto', child: Text('Mic: Auto (BT if available)')),
                    DropdownMenuItem(value: 'bluetooth', child: Text('Mic: Bluetooth')),
                    DropdownMenuItem(value: 'phone', child: Text('Mic: Phone')),
                  ],
                  onChanged: (v) {
                    if (v != null) _switchMicSource(v);
                  },
                ),
              ),
            ),

            const SizedBox(height: 28),

            // Disconnect button
            SizedBox(
              width: 180,
              height: 48,
              child: OutlinedButton.icon(
                onPressed: _disconnect,
                icon: const Icon(Icons.stop_rounded, size: 20),
                label: const Text('Disconnect', style: TextStyle(fontSize: 15)),
                style: OutlinedButton.styleFrom(
                  foregroundColor: cs.error,
                  side: BorderSide(color: cs.error.withOpacity(0.4)),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(12),
                  ),
                ),
              ),
            ),
          ],
        ),
      ),
    );
  }

  String _formatDuration(int ms) {
    final totalSeconds = ms ~/ 1000;
    final minutes = totalSeconds ~/ 60;
    final seconds = totalSeconds % 60;
    return '$minutes:${seconds.toString().padLeft(2, '0')}';
  }

  Widget _buildControlButton({
    required IconData icon,
    required String label,
    required bool isActive,
    required Color activeColor,
    required VoidCallback onTap,
    required ColorScheme cs,
  }) {
    return GestureDetector(
      onTap: onTap,
      child: Column(
        children: [
          Container(
            width: 56,
            height: 56,
            decoration: BoxDecoration(
              shape: BoxShape.circle,
              color: isActive
                  ? activeColor.withOpacity(0.15)
                  : cs.surfaceContainerHighest,
              border: Border.all(
                color: isActive
                    ? activeColor.withOpacity(0.4)
                    : cs.outlineVariant.withOpacity(0.2),
              ),
            ),
            child: Icon(
              icon,
              size: 24,
              color: isActive ? activeColor : cs.onSurface.withOpacity(0.4),
            ),
          ),
          const SizedBox(height: 6),
          Text(
            label,
            style: Theme.of(context).textTheme.labelSmall?.copyWith(
                  color: isActive ? cs.onSurface.withOpacity(0.7) : cs.onSurface.withOpacity(0.35),
                ),
          ),
        ],
      ),
    );
  }

  // ---- Main View (disconnected / scanning) ----
  Widget _buildMainView(ColorScheme cs) {
    final isScanning = _state == ConnectionState_.discovering;
    final isConnecting = _state == ConnectionState_.connecting;
    final isPairPending = _state == ConnectionState_.pairPending;
    final recentServers = _knownServers.reversed.take(10).toList();

    return Column(
      children: [
        // Header
        Padding(
          padding: const EdgeInsets.fromLTRB(24, 20, 24, 0),
          child: Row(
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
                child: const Icon(Icons.headphones_rounded, size: 22, color: Colors.white),
              ),
              const SizedBox(width: 12),
              Text(
                'AudioBridge',
                style: Theme.of(context).textTheme.titleLarge?.copyWith(
                      fontWeight: FontWeight.bold,
                    ),
              ),
            ],
          ),
        ),
        const SizedBox(height: 16),

        // Device name (tappable to edit)
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 24),
          child: GestureDetector(
            onTap: _showEditDeviceNameDialog,
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
                  Icon(Icons.badge_outlined, size: 18, color: cs.onSurface.withOpacity(0.5)),
                  const SizedBox(width: 10),
                  Expanded(
                    child: Text(
                      _deviceName,
                      style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                            fontWeight: FontWeight.w500,
                          ),
                    ),
                  ),
                  Icon(Icons.edit_outlined, size: 16, color: cs.onSurface.withOpacity(0.3)),
                ],
              ),
            ),
          ),
        ),
        const SizedBox(height: 16),

        // Scan button
        Padding(
          padding: const EdgeInsets.symmetric(horizontal: 24),
          child: SizedBox(
            width: double.infinity,
            height: 56,
            child: FilledButton.icon(
              onPressed: isConnecting ? null : _startDiscovery,
              icon: isScanning
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
                            color: cs.onSurface.withOpacity(0.4),
                          ),
                        )
                      : const Icon(Icons.radar_rounded),
              label: Text(
                isConnecting
                    ? 'Connecting...'
                    : isPairPending
                        ? 'Awaiting approval...'
                        : isScanning
                            ? 'Scanning...'
                            : 'Scan for Servers',
                style: const TextStyle(fontSize: 16, fontWeight: FontWeight.w600),
              ),
              style: FilledButton.styleFrom(
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
              style: Theme.of(context).textTheme.bodySmall?.copyWith(
                    color: cs.onSurface.withOpacity(0.4),
                  ),
            ),
          ),

        const SizedBox(height: 20),

        // Content
        Expanded(
          child: SingleChildScrollView(
            padding: const EdgeInsets.symmetric(horizontal: 24),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                if (isScanning && _discoveredServers.isNotEmpty) ...[
                  _buildSectionHeader(cs, 'Available Servers', Icons.wifi_rounded),
                  const SizedBox(height: 8),
                  ..._discoveredServers.values.map((server) => _buildDiscoveredServerCard(cs, server)),
                  const SizedBox(height: 20),
                ],

                if (isScanning && _discoveredServers.isEmpty) ...[
                  Padding(
                    padding: const EdgeInsets.symmetric(vertical: 32),
                    child: Center(
                      child: Column(
                        children: [
                          Icon(Icons.radar_rounded, size: 48, color: cs.onSurface.withOpacity(0.15)),
                          const SizedBox(height: 12),
                          Text(
                            'Looking for servers on your network...',
                            style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                                  color: cs.onSurface.withOpacity(0.4),
                                ),
                          ),
                        ],
                      ),
                    ),
                  ),
                ],

                if (recentServers.isNotEmpty && !isConnecting && !isPairPending) ...[
                  _buildSectionHeader(cs, 'Recent', Icons.history_rounded),
                  const SizedBox(height: 8),
                  ...recentServers.map((server) => _buildHistoryCard(cs, server)),
                  const SizedBox(height: 24),
                ],

                if (recentServers.isEmpty && !isScanning && !isConnecting && !isPairPending)
                  Padding(
                    padding: const EdgeInsets.symmetric(vertical: 48),
                    child: Center(
                      child: Column(
                        children: [
                          Icon(Icons.cast_rounded, size: 64, color: cs.onSurface.withOpacity(0.12)),
                          const SizedBox(height: 16),
                          Text(
                            'No previous connections',
                            style: Theme.of(context).textTheme.bodyLarge?.copyWith(
                                  color: cs.onSurface.withOpacity(0.3),
                                ),
                          ),
                          const SizedBox(height: 6),
                          Text(
                            'Scan to find AudioBridge servers\non your local network',
                            textAlign: TextAlign.center,
                            style: Theme.of(context).textTheme.bodySmall?.copyWith(
                                  color: cs.onSurface.withOpacity(0.25),
                                ),
                          ),
                        ],
                      ),
                    ),
                  ),
              ],
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildSectionHeader(ColorScheme cs, String title, IconData icon) {
    return Row(
      children: [
        Icon(icon, size: 16, color: cs.onSurface.withOpacity(0.4)),
        const SizedBox(width: 6),
        Text(
          title,
          style: Theme.of(context).textTheme.labelLarge?.copyWith(
                color: cs.onSurface.withOpacity(0.5),
                fontWeight: FontWeight.w600,
                letterSpacing: 0.5,
              ),
        ),
      ],
    );
  }

  Widget _buildDiscoveredServerCard(ColorScheme cs, _DiscoveredServer server) {
    final knownEntry = _knownServers.where((s) => s.macAddress == server.mac).toList();
    final isKnown = knownEntry.isNotEmpty;
    final nameMismatch = _knownServers.any(
      (s) => s.name == server.name && s.macAddress != server.mac && server.mac.isNotEmpty,
    );

    return Container(
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
                          : cs.onSurface.withOpacity(0.5),
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
                      style: Theme.of(context).textTheme.bodySmall?.copyWith(
                            color: nameMismatch
                                ? Colors.orange.withOpacity(0.8)
                                : cs.onSurface.withOpacity(0.45),
                          ),
                    ),
                  ],
                ),
              ),
              Icon(
                Icons.arrow_forward_ios_rounded,
                size: 16,
                color: cs.onSurface.withOpacity(0.3),
              ),
            ],
          ),
        ),
      ),
    );
  }

  Widget _buildHistoryCard(ColorScheme cs, ServerInfo server) {
    return Container(
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
                  color: cs.onSurface.withOpacity(0.4),
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
                      style: Theme.of(context).textTheme.bodySmall?.copyWith(
                            color: cs.onSurface.withOpacity(0.4),
                          ),
                    ),
                  ],
                ),
              ),
              Icon(
                Icons.arrow_forward_ios_rounded,
                size: 16,
                color: cs.onSurface.withOpacity(0.25),
              ),
            ],
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
