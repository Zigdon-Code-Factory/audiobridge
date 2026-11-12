import 'dart:async';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

class AppUpdateDialog extends StatefulWidget {
  const AppUpdateDialog({super.key, required this.beforeInstall});
  final Future<bool> Function() beforeInstall;

  @override
  State<AppUpdateDialog> createState() => _AppUpdateDialogState();
}

class _AppUpdateDialogState extends State<AppUpdateDialog> {
  static const _channel = MethodChannel('com.audiobridge/updates');
  Map<String, dynamic> _state = {};
  Timer? _timer;
  bool _reading = false;
  bool _acting = false;
  bool _confirming = false;
  bool _installWhenReady = false;
  String _error = '';
  final _actionFocus = FocusNode();
  final _statusFocus = FocusNode(debugLabel: 'Update status');

  String get _phase => _state['phase'] as String? ?? 'idle';
  bool get _busy =>
      _acting || _confirming || _phase == 'checking' || _phase == 'downloading';

  @override
  void initState() {
    super.initState();
    _readState();
    _timer = Timer.periodic(
      const Duration(milliseconds: 500),
      (_) => _readState(),
    );
  }

  @override
  void dispose() {
    _timer?.cancel();
    _actionFocus.dispose();
    _statusFocus.dispose();
    super.dispose();
  }

  Future<void> _readState() async {
    if (_reading) return;
    _reading = true;
    try {
      final state = await _channel
          .invokeMapMethod<String, dynamic>('getUpdateState')
          .timeout(const Duration(seconds: 5));
      if (!mounted) return;
      setState(() => _state = state ?? {});
      if (_actionFocus.hasFocus &&
          ['checking', 'downloading'].contains(_phase)) {
        _statusFocus.requestFocus();
      }
      if (_phase == 'ready' && _installWhenReady && !_acting) {
        _installWhenReady = false;
        await _launchInstaller();
      }
    } catch (_) {
      if (mounted) {
        setState(
          () =>
              _error =
                  'Could not read update status. Close this window and try again.',
        );
      }
    } finally {
      _reading = false;
    }
  }

  Future<void> _command(String method) async {
    if (_acting) return;
    final restoreFocus = _actionFocus.hasFocus;
    setState(() {
      _acting = true;
      _error = '';
    });
    try {
      await _channel
          .invokeMethod<void>(method)
          .timeout(const Duration(seconds: 5));
      await _readState();
    } catch (_) {
      _installWhenReady = false;
      if (mounted) {
        setState(
          () =>
              _error =
                  'The update request could not be confirmed. Check again or retry; your installed app is unchanged.',
        );
      }
    } finally {
      if (mounted) {
        setState(() => _acting = false);
        final focus = FocusManager.instance.primaryFocus;
        if (restoreFocus &&
            (focus == null ||
                focus is FocusScopeNode ||
                focus == _actionFocus)) {
          (_busy ? _statusFocus : _actionFocus).requestFocus();
        }
      }
    }
  }

  Future<void> _launchInstaller() async {
    if (_acting) return;
    final restoreFocus = _actionFocus.hasFocus;
    setState(() {
      _acting = true;
      _error = '';
    });
    try {
      if (!await widget.beforeInstall()) {
        if (mounted) {
          setState(
            () =>
                _error =
                    'Audio shutdown was not confirmed. Recover the connection before installing the update.',
          );
        }
        return;
      }
      if (!mounted) return;
      await _channel
          .invokeMethod<void>('launchInstaller')
          .timeout(const Duration(seconds: 5));
    } catch (_) {
      if (mounted) {
        setState(
          () =>
              _error =
                  'Could not open the Android installer. Retry installation; your current app is unchanged.',
        );
      }
    } finally {
      if (mounted) {
        setState(() => _acting = false);
        final focus = FocusManager.instance.primaryFocus;
        if (restoreFocus && (focus == null || focus is FocusScopeNode)) {
          _actionFocus.requestFocus();
        }
      }
    }
  }

  Future<void> _update() async {
    if (_busy) return;
    if (_phase == 'ready') {
      await _launchInstaller();
      return;
    }
    setState(() => _confirming = true);
    final approved = await showDialog<bool>(
      context: context,
      builder:
          (context) => AlertDialog(
            title: const Text('Update app?'),
            scrollable: true,
            insetPadding: const EdgeInsets.symmetric(
              horizontal: 16,
              vertical: 24,
            ),
            content: const Text(
              'The update comes from the official AudioBridge GitHub release and is verified before installation. Audio will disconnect before the Android installer opens. Your pairings and settings are kept. Android may ask you to allow updates from this app, then confirm installation.',
            ),
            actions: [
              TextButton(
                onPressed: () => Navigator.pop(context, false),
                child: const Text('Cancel'),
              ),
              FilledButton(
                onPressed: () => Navigator.pop(context, true),
                child: const Text('Install update'),
              ),
            ],
          ),
    );
    if (mounted) setState(() => _confirming = false);
    if (approved != true || !mounted) return;
    _installWhenReady = true;
    await _command('installUpdate');
  }

  @override
  Widget build(BuildContext context) {
    final progress = (_state['progress'] as num? ?? 0).toDouble().clamp(
      0.0,
      1.0,
    );
    final available = _phase == 'available' || _phase == 'ready';
    return AlertDialog(
      title: const Text('App updates'),
      scrollable: true,
      insetPadding: const EdgeInsets.symmetric(horizontal: 16, vertical: 24),
      content: SizedBox(
        width: 420,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          mainAxisSize: MainAxisSize.min,
          children: [
            Text('Installed: ${_state['currentVersion'] ?? '0.2.0'}'),
            if (_state['version'] != null)
              Text('Latest release: ${_state['version']}'),
            const SizedBox(height: 12),
            Focus(
              focusNode: _statusFocus,
              skipTraversal: true,
              child: Semantics(
                liveRegion: true,
                child: Text(
                  _error.isNotEmpty
                      ? _error
                      : _state['message'] as String? ??
                          'Check the official GitHub release for a newer version.',
                  key: const ValueKey('updateStatus'),
                ),
              ),
            ),
            if (_phase == 'checking')
              const Padding(
                padding: EdgeInsets.symmetric(vertical: 12),
                child: LinearProgressIndicator(
                  semanticsLabel: 'Checking for updates',
                ),
              ),
            if (_phase == 'downloading')
              Padding(
                padding: const EdgeInsets.symmetric(vertical: 12),
                child: LinearProgressIndicator(
                  value: progress,
                  semanticsLabel: 'Update download progress',
                  semanticsValue: '${(progress * 100).round()} percent',
                ),
              ),
            const SizedBox(height: 12),
            Text(
              'Only updates this phone. Windows has its own update button.',
              style: Theme.of(context).textTheme.bodyMedium,
            ),
          ],
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('Close'),
        ),
        FilledButton(
          key: const ValueKey('updateAction'),
          focusNode: _actionFocus,
          onPressed:
              _busy
                  ? null
                  : available
                  ? _update
                  : () => _command('checkForUpdates'),
          child: Text(
            available
                ? _phase == 'ready'
                    ? 'Continue installation'
                    : 'Update now'
                : 'Check for updates',
          ),
        ),
      ],
    );
  }
}
