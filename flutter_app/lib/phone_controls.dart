import 'package:flutter/material.dart';

class LabeledSurfaceButton extends StatelessWidget {
  const LabeledSurfaceButton({
    super.key,
    required this.label,
    this.semanticValue,
    required this.onPressed,
    required this.child,
  });

  final String label;
  final String? semanticValue;
  final VoidCallback onPressed;
  final Widget child;

  @override
  Widget build(BuildContext context) => MergeSemantics(
    child: Semantics(
      label: label,
      value: semanticValue,
      child: TextButton(
        onPressed: onPressed,
        style: TextButton.styleFrom(
          minimumSize: const Size(48, 48),
          padding: EdgeInsets.zero,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(12),
          ),
        ),
        child: ExcludeSemantics(child: child),
      ),
    ),
  );
}

class PhoneAudioControls extends StatefulWidget {
  const PhoneAudioControls({
    super.key,
    required this.canControlMedia,
    this.mediaBusy = false,
    this.mediaUnavailableMessage =
        'PC media controls require current Admin permission. Phone audio controls remain available.',
    required this.mediaPlaying,
    required this.recording,
    required this.micAllowed,
    required this.micBusy,
    this.micUnknown = false,
    this.micMessage = '',
    this.playbackMessage = '',
    this.mediaMessage = '',
    required this.muted,
    required this.volume,
    required this.micSource,
    required this.onPrevious,
    required this.onPlayPause,
    required this.onNext,
    required this.onToggleMic,
    required this.onToggleMute,
    required this.onVolume,
    required this.onMicSource,
    required this.onDisconnect,
  });

  final bool canControlMedia;
  final bool mediaBusy;
  final String mediaUnavailableMessage;
  final bool mediaPlaying;
  final bool recording;
  final bool micAllowed;
  final bool micBusy;
  final bool micUnknown;
  final String micMessage;
  final String playbackMessage;
  final String mediaMessage;
  final bool muted;
  final double volume;
  final String micSource;
  final VoidCallback onPrevious;
  final VoidCallback onPlayPause;
  final VoidCallback onNext;
  final VoidCallback onToggleMic;
  final VoidCallback onToggleMute;
  final ValueChanged<double> onVolume;
  final ValueChanged<String> onMicSource;
  final VoidCallback onDisconnect;

  @override
  State<PhoneAudioControls> createState() => _PhoneAudioControlsState();
}

class _PhoneAudioControlsState extends State<PhoneAudioControls> {
  final FocusNode _microphoneFocus = FocusNode();
  final FocusNode _sourceFocus = FocusNode();
  final FocusNode _previousFocus = FocusNode();
  final FocusNode _playPauseFocus = FocusNode();
  final FocusNode _nextFocus = FocusNode();
  FocusNode? _pendingFocus;
  FocusNode? _pendingMediaFocus;

  @override
  void didUpdateWidget(covariant PhoneAudioControls oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (widget.micBusy && !oldWidget.micBusy) {
      _pendingFocus =
          _microphoneFocus.hasFocus
              ? _microphoneFocus
              : _sourceFocus.hasFocus
              ? _sourceFocus
              : null;
    } else if (!widget.micBusy && oldWidget.micBusy) {
      _restoreFocus(_pendingFocus);
      _pendingFocus = null;
    }
    if (widget.mediaBusy && !oldWidget.mediaBusy) {
      _pendingMediaFocus = null;
      for (final focus in [_previousFocus, _playPauseFocus, _nextFocus]) {
        if (focus.hasFocus) _pendingMediaFocus = focus;
      }
    } else if (!widget.mediaBusy && oldWidget.mediaBusy) {
      _restoreFocus(_pendingMediaFocus);
      _pendingMediaFocus = null;
    }
  }

  void _restoreFocus(FocusNode? target) {
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final focused = FocusManager.instance.primaryFocus;
      if (mounted &&
          target != null &&
          target.canRequestFocus &&
          (focused == null || focused is FocusScopeNode)) {
        target.requestFocus();
      }
    });
  }

  @override
  void dispose() {
    _microphoneFocus.dispose();
    _sourceFocus.dispose();
    _previousFocus.dispose();
    _playPauseFocus.dispose();
    _nextFocus.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final percent = (widget.muted ? 0 : widget.volume * 100).round();
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        Semantics(
          header: true,
          child: Text(
            'Phone audio',
            style: Theme.of(context).textTheme.titleMedium,
          ),
        ),
        const SizedBox(height: 12),
        MergeSemantics(
          child: Tooltip(
            message:
                widget.micUnknown
                    ? 'Stop microphone to confirm it is off'
                    : widget.recording
                    ? 'Turn phone microphone off'
                    : !widget.micAllowed
                    ? 'Another phone owns microphone return'
                    : 'Turn phone microphone on',
            child: Semantics(
              toggled: widget.micUnknown ? null : widget.recording,
              value:
                  widget.micUnknown
                      ? 'State unknown. Last confirmed: ${widget.recording ? 'On' : 'Off'}'
                      : null,
              child: OutlinedButton.icon(
                focusNode: _microphoneFocus,
                key: const ValueKey('phoneMicrophone'),
                onPressed:
                    (widget.micAllowed ||
                                widget.recording ||
                                widget.micUnknown) &&
                            !widget.micBusy
                        ? widget.onToggleMic
                        : null,
                icon: Icon(
                  widget.recording ? Icons.mic_rounded : Icons.mic_off_rounded,
                ),
                label: Text(
                  widget.micBusy
                      ? 'Changing microphone…'
                      : widget.micUnknown ||
                          (!widget.micAllowed && widget.recording)
                      ? 'Stop microphone'
                      : !widget.micAllowed
                      ? 'Mic on first phone'
                      : widget.recording
                      ? 'Mic On'
                      : 'Mic Off',
                ),
                style: OutlinedButton.styleFrom(
                  minimumSize: const Size(48, 48),
                  padding: const EdgeInsets.all(12),
                ),
              ),
            ),
          ),
        ),
        if (widget.micMessage.isNotEmpty)
          Padding(
            padding: const EdgeInsets.only(top: 8),
            child: Semantics(
              liveRegion: true,
              child: Text(
                widget.micMessage,
                key: const ValueKey('phoneMicrophoneStatus'),
              ),
            ),
          ),
        const SizedBox(height: 12),
        Row(
          children: [
            Expanded(
              child: Text(
                'App volume',
                style: Theme.of(context).textTheme.labelLarge,
              ),
            ),
            IconButton(
              key: const ValueKey('mutePlayback'),
              tooltip:
                  widget.muted || widget.volume == 0
                      ? 'Unmute playback'
                      : 'Mute playback',
              onPressed: widget.onToggleMute,
              icon: Icon(
                widget.muted || widget.volume == 0
                    ? Icons.volume_off_rounded
                    : Icons.volume_up_rounded,
              ),
              constraints: const BoxConstraints(minWidth: 48, minHeight: 48),
            ),
            ExcludeSemantics(child: Text('$percent%')),
          ],
        ),
        MergeSemantics(
          child: Semantics(
            label: 'App playback volume',
            child: Slider(
              key: const ValueKey('playbackVolume'),
              value: widget.muted ? 0 : widget.volume,
              divisions: 100,
              semanticFormatterCallback:
                  (value) => '${(value * 100).round()} percent',
              onChanged: widget.onVolume,
            ),
          ),
        ),
        if (widget.playbackMessage.isNotEmpty)
          Semantics(
            liveRegion: true,
            child: Text(
              widget.playbackMessage,
              key: const ValueKey('phonePlaybackStatus'),
            ),
          ),
        const SizedBox(height: 12),
        ListenableBuilder(
          listenable: _sourceFocus,
          builder:
              (context, child) => InputDecorator(
                isFocused: _sourceFocus.hasFocus,
                decoration: InputDecoration(
                  labelText: 'Phone microphone source',
                  border: const OutlineInputBorder(),
                  enabled:
                      widget.micAllowed &&
                      !widget.micBusy &&
                      !widget.micUnknown,
                ),
                child: DropdownButtonHideUnderline(
                  child: DropdownButton<String>(
                    key: const ValueKey('micSource'),
                    focusNode: _sourceFocus,
                    value: widget.micSource,
                    isExpanded: true,
                    isDense: false,
                    itemHeight: null,
                    items: const [
                      DropdownMenuItem(
                        value: 'auto',
                        child: Text('Mic: Auto (BT if available)'),
                      ),
                      DropdownMenuItem(
                        value: 'bluetooth',
                        child: Text('Mic: Bluetooth'),
                      ),
                      DropdownMenuItem(
                        value: 'phone',
                        child: Text('Mic: Phone'),
                      ),
                    ],
                    onChanged:
                        widget.micAllowed &&
                                !widget.micBusy &&
                                !widget.micUnknown
                            ? (value) {
                              if (value != null) widget.onMicSource(value);
                            }
                            : null,
                  ),
                ),
              ),
        ),
        const SizedBox(height: 20),
        Semantics(
          header: true,
          child: Text(
            'PC media',
            style: Theme.of(context).textTheme.titleMedium,
          ),
        ),
        if (widget.mediaMessage.isNotEmpty)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 8),
            child: Semantics(
              liveRegion: true,
              child: Text(
                widget.mediaMessage,
                key: const ValueKey('phoneMediaStatus'),
              ),
            ),
          ),
        if (!widget.canControlMedia)
          Padding(
            padding: const EdgeInsets.symmetric(vertical: 8),
            child: Text(widget.mediaUnavailableMessage),
          ),
        const SizedBox(height: 8),
        Wrap(
          alignment: WrapAlignment.center,
          crossAxisAlignment: WrapCrossAlignment.center,
          spacing: 12,
          runSpacing: 12,
          children: [
            IconButton.outlined(
              focusNode: _previousFocus,
              tooltip: 'Previous track on PC',
              onPressed: widget.canControlMedia ? widget.onPrevious : null,
              icon: const Icon(Icons.skip_previous_rounded),
              constraints: const BoxConstraints(minWidth: 56, minHeight: 56),
            ),
            IconButton.filled(
              key: const ValueKey('mediaPlayPause'),
              focusNode: _playPauseFocus,
              tooltip: widget.mediaPlaying ? 'Pause media' : 'Play media',
              onPressed: widget.canControlMedia ? widget.onPlayPause : null,
              icon: Icon(
                widget.mediaPlaying
                    ? Icons.pause_rounded
                    : Icons.play_arrow_rounded,
              ),
              iconSize: 36,
              constraints: const BoxConstraints(minWidth: 64, minHeight: 64),
            ),
            IconButton.outlined(
              focusNode: _nextFocus,
              tooltip: 'Next track on PC',
              onPressed: widget.canControlMedia ? widget.onNext : null,
              icon: const Icon(Icons.skip_next_rounded),
              constraints: const BoxConstraints(minWidth: 56, minHeight: 56),
            ),
          ],
        ),
        const SizedBox(height: 24),
        OutlinedButton.icon(
          onPressed: widget.onDisconnect,
          icon: const Icon(Icons.stop_rounded),
          label: const Text('Disconnect'),
          style: OutlinedButton.styleFrom(
            minimumSize: const Size(48, 48),
            padding: const EdgeInsets.all(12),
            foregroundColor: Theme.of(context).colorScheme.error,
          ),
        ),
      ],
    );
  }
}
