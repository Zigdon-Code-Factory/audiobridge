import 'dart:async';
import 'dart:convert';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

class DesktopControlState extends ChangeNotifier {
  DesktopControlState(this.channel);
  final MethodChannel channel;
  Map<String, dynamic> settings = {};
  bool supported = false;
  bool admin = false;
  String message = '';
  String mediaMessage = '';
  int _sequence = 0;
  int? _pending;
  int _command = 0;
  int _value = 0;
  int _sentRevision = 0;
  bool _applied = false;
  Timer? _timeout;
  Timer? _freshness;
  bool get busy => _pending != null;
  bool get canEdit => supported && admin && !busy;
  static const fields = {
    1: 'jitter',
    2: 'frame',
    3: 'pcm',
    4: 'loopback',
    5: 'input',
    6: 'output',
    7: 'noise',
    8: 'strength',
    9: 'gate',
    10: 'threshold',
    11: 'hold',
    12: 'lowCut',
  };

  void reset({bool notify = true}) {
    _timeout?.cancel();
    _freshness?.cancel();
    settings = {};
    supported = false;
    admin = false;
    _pending = null;
    message = '';
    mediaMessage = '';
    if (notify) notifyListeners();
  }

  void receiveState(String json) {
    try {
      final state = jsonDecode(json) as Map<String, dynamic>;
      if (state['version'] != 1 || state['settings'] is! Map) return;
      final snapshot = Map<String, dynamic>.from(state['settings'] as Map);
      if (snapshot['revision'] is! int || (snapshot['revision'] as int) <= 0) {
        return;
      }
      if ((snapshot['revision'] as int) < (settings['revision'] as int? ?? 0)) {
        return;
      }
      settings = snapshot;
      supported = true;
      admin = state['admin'] == true;
      _freshness?.cancel();
      _freshness = Timer(const Duration(seconds: 3), () {
        supported = false;
        admin = false;
        _finish('Permission status is stale. Waiting for the desktop.');
      });
      if (!admin && busy) {
        _finish(
          'Read only: permission changed. Ask the desktop owner for Admin.',
        );
      } else {
        _confirmApplied();
        notifyListeners();
      }
    } catch (_) {}
  }

  void receiveResult(String json) {
    try {
      final result = jsonDecode(json) as Map<String, dynamic>;
      if (_pending == null || result['request'] != _pending) return;
      if (result['ok'] != true) {
        _finish(
          result['message'] as String? ?? 'The desktop rejected this change.',
        );
      } else {
        _applied = true;
        _confirmApplied();
      }
    } catch (_) {}
  }

  void _confirmApplied() {
    if (!busy || !_applied) return;
    if (_command == 16) {
      _finish('PC media action applied.');
      return;
    }
    final actual = settings[fields[_command]];
    final numeric = actual is bool ? (actual ? 1 : 0) : actual;
    if ((settings['revision'] as int? ?? 0) >= _sentRevision &&
        numeric == _value) {
      _finish('Change applied and confirmed by the desktop.');
    }
  }

  void _finish(String status) {
    _timeout?.cancel();
    _pending = null;
    message = status;
    if (_command == 16) mediaMessage = status;
    notifyListeners();
  }

  Future<void> send(int command, int value) async {
    if (!canEdit || (!fields.containsKey(command) && command != 16)) return;
    _sequence = _sequence % 0x7ffffffe + 1;
    final request = _sequence;
    _pending = request;
    _command = command;
    _value = value;
    _sentRevision = settings['revision'] as int;
    _applied = false;
    message = 'Waiting for the desktop to apply and confirm...';
    if (command == 16) mediaMessage = 'Waiting for the PC media action...';
    notifyListeners();
    _timeout = Timer(const Duration(seconds: 4), () {
      _finish(
        'No application confirmation. Check current settings before retrying.',
      );
    });
    try {
      final sent = await channel.invokeMethod<bool>('sendDesktopControl', {
        'request': request,
        'revision': _sentRevision,
        'command': command,
        'value': value,
      });
      if (sent != true && _pending == request) {
        _finish('Request could not be sent. Settings are unchanged.');
      }
    } catch (_) {
      if (_pending == request) {
        _finish(
          'Desktop controls are unavailable. Reconnect with an updated app.',
        );
      }
    }
  }

  @override
  void dispose() {
    _timeout?.cancel();
    _freshness?.cancel();
    super.dispose();
  }
}

class DesktopSettingsPanel extends StatefulWidget {
  const DesktopSettingsPanel({super.key, required this.controls});
  final DesktopControlState controls;

  @override
  State<DesktopSettingsPanel> createState() => _DesktopSettingsPanelState();
}

class _DesktopSettingsPanelState extends State<DesktopSettingsPanel> {
  final Map<int, FocusNode> _controlFocus = {};
  bool _wasBusy = false;
  FocusNode? _pendingFocus;

  @override
  void dispose() {
    for (final focus in _controlFocus.values) {
      focus.dispose();
    }
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final controls = widget.controls;
    if (controls.busy && !_wasBusy) {
      _pendingFocus = null;
      for (final focus in _controlFocus.values) {
        if (focus.hasFocus) _pendingFocus = focus;
      }
    } else if (!controls.busy && _wasBusy) {
      final target = _pendingFocus;
      WidgetsBinding.instance.addPostFrameCallback((_) {
        if (mounted &&
            controls.canEdit &&
            target != null &&
            FocusManager.instance.primaryFocus is FocusScopeNode) {
          target.requestFocus();
        }
      });
      _pendingFocus = null;
    }
    _wasBusy = controls.busy;
    final settings = controls.settings;
    final reduceMotion = MediaQuery.disableAnimationsOf(context);
    final readiness = [
      if (settings.isNotEmpty)
        settings['captureReady'] == true
            ? 'PC capture is running.'
            : 'PC capture is unavailable. Check the desktop route.',
      if (settings['outputReady'] == false)
        'Phone microphone destination is unavailable. Incoming audio is not reaching that destination; choose a working route on the desktop.',
      if (settings.isNotEmpty && settings['voiceReady'] != true)
        'Voice cleanup is unavailable on the desktop. Saved preferences are inactive.',
    ];
    Widget select(
      String label,
      int command,
      Map<int, String> choices,
      int fallback,
    ) {
      final field = settings[DesktopControlState.fields[command]];
      final current =
          field is bool
              ? (field ? 1 : 0)
              : (field as num?)?.toInt() ?? fallback;
      final options = {...choices};
      final endpointName = settings[command == 5 ? 'inputName' : 'outputName'];
      options.putIfAbsent(
        current,
        () =>
            (command == 5 || command == 6) && endpointName is String
                ? 'Current: $endpointName'
                : 'Current: $current',
      );
      return Padding(
        padding: const EdgeInsets.only(top: 12),
        child: DropdownButtonFormField<int>(
          key: ValueKey('desktop-$command-$current-${controls.busy}'),
          focusNode: _controlFocus.putIfAbsent(
            command,
            () => FocusNode(debugLabel: label),
          ),
          initialValue: current,
          isExpanded: true,
          isDense: false,
          itemHeight: null,
          decoration: InputDecoration(
            labelText: label,
            border: const OutlineInputBorder(),
          ),
          items:
              options.entries
                  .map(
                    (entry) => DropdownMenuItem(
                      value: entry.key,
                      child: Text(entry.value),
                    ),
                  )
                  .toList(),
          onChanged:
              controls.canEdit &&
                      (command < 7 || settings['voiceReady'] == true)
                  ? (value) {
                    if (value != null) controls.send(command, value);
                  }
                  : null,
        ),
      );
    }

    Map<int, String> numeric(List<int> values, String unit) => {
      for (final value in values) value: '$value$unit',
    };
    Map<int, String> devices(String key) {
      final names = settings[key] as List? ?? const ['System default'];
      return {
        for (var index = 0; index < names.length && index <= 8; ++index)
          index: names[index].toString(),
      };
    }

    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Semantics(
              header: true,
              child: Text(
                'PC settings',
                style: Theme.of(context).textTheme.titleMedium,
              ),
            ),
            const SizedBox(height: 8),
            Semantics(
              liveRegion: true,
              child: Text(
                !controls.supported
                    ? 'Desktop controls unsupported or awaiting a fresh permission snapshot. Update the desktop app; editing and PC media controls stay disabled.'
                    : controls.admin
                    ? 'Admin: PC settings and media controls enabled. Only the desktop owner can change device permissions.'
                    : 'Read only: ask the desktop owner to grant Admin in Paired devices. Listening, phone volume, playback, microphone and disconnect remain available.',
              ),
            ),
            if (readiness.isNotEmpty)
              Semantics(
                liveRegion: true,
                child: Text(
                  readiness.join('\n'),
                  key: const ValueKey('desktop-readiness'),
                ),
              ),
            if (controls.message.isNotEmpty)
              Padding(
                padding: const EdgeInsets.only(top: 8),
                child: Semantics(
                  liveRegion: true,
                  child: Text(
                    controls.message,
                    key: const ValueKey('desktop-status'),
                  ),
                ),
              ),
            if (controls.busy && !reduceMotion) const LinearProgressIndicator(),
            ExpansionTile(
              expansionAnimationStyle:
                  reduceMotion ? AnimationStyle.noAnimation : null,
              title: const Text('Audio routing and quality'),
              children: [
                select('Capture mode', 4, {
                  1: 'System audio',
                  0: 'Recording device',
                }, 1),
                select('Send PC audio from', 5, devices('inputs'), 0),
                select('Return phone microphone to', 6, devices('outputs'), 0),
                select('Codec', 3, {1: 'Raw PCM', 0: 'Opus'}, 1),
                select('Frame duration', 2, {
                  2: '2.5 ms',
                  5: '5 ms',
                  10: '10 ms',
                  20: '20 ms',
                }, 2),
                select(
                  'Jitter target',
                  1,
                  numeric([0, 5, 10, 15, 20, 30, 40, 60, 80, 100, 150], ' ms'),
                  10,
                ),
              ],
            ),
            ExpansionTile(
              expansionAnimationStyle:
                  reduceMotion ? AnimationStyle.noAnimation : null,
              title: const Text('Voice cleanup'),
              children: [
                select('Noise suppression', 7, {0: 'Off', 1: 'On'}, 1),
                select('Noise strength', 8, {
                  0: 'Off · 0 dB',
                  6: 'Gentle · 6 dB',
                  10: 'Light · 10 dB',
                  15: 'Medium · 15 dB',
                  20: 'Strong · 20 dB',
                  30: 'Intense · 30 dB',
                  100: 'Maximum',
                }, 100),
                select('Speech gate', 9, {0: 'Off', 1: 'On'}, 1),
                select(
                  'Gate threshold',
                  10,
                  numeric([10, 20, 30, 40, 50, 60, 70, 80, 90], '%'),
                  50,
                ),
                select(
                  'Gate hold',
                  11,
                  numeric([100, 200, 300, 500, 750, 1000, 1500], ' ms'),
                  300,
                ),
                select('Low cut', 12, {0: 'Off', 1: 'On'}, 0),
              ],
            ),
          ],
        ),
      ),
    );
  }
}
