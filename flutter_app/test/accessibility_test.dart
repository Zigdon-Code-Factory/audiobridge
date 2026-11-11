import 'dart:ui' show SemanticsAction, Tristate;
import 'package:audiobridge/main.dart';
import 'package:audiobridge/phone_controls.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('com.audiobridge/audio');

  Widget audioControls({
    bool admin = true,
    bool micAllowed = true,
    bool micBusy = false,
    bool recording = true,
    double scale = 1,
    VoidCallback? onMic,
    ValueChanged<double>? onVolume,
  }) => MaterialApp(
    theme: ThemeData(
      colorSchemeSeed: const Color(0xFF7C4DFF),
      brightness: Brightness.dark,
      useMaterial3: true,
    ),
    home: MediaQuery(
      data: MediaQueryData(
        textScaler: TextScaler.linear(scale),
        disableAnimations: true,
      ),
      child: Scaffold(
        body: SingleChildScrollView(
          padding: const EdgeInsets.all(24),
          child: PhoneAudioControls(
            canControlMedia: admin,
            mediaPlaying: false,
            recording: recording,
            micAllowed: micAllowed,
            micBusy: micBusy,
            muted: false,
            volume: .5,
            micSource: 'auto',
            onPrevious: () {},
            onPlayPause: () {},
            onNext: () {},
            onToggleMic: onMic ?? () {},
            onToggleMute: () {},
            onVolume: onVolume ?? (_) {},
            onMicSource: (_) {},
            onDisconnect: () {},
          ),
        ),
      ),
    ),
  );

  testWidgets('Phone actions expose labels, roles, states, and volume units', (
    tester,
  ) async {
    await tester.pumpWidget(audioControls());
    final microphone =
        tester
            .getSemantics(find.byKey(const ValueKey('phoneMicrophone')))
            .getSemanticsData();
    expect(microphone.flagsCollection.isButton, isTrue);
    expect(microphone.flagsCollection.isToggled, isNot(Tristate.none));
    expect(microphone.flagsCollection.isToggled, Tristate.isTrue);
    expect(microphone.label, contains('Mic On'));
    expect(find.byTooltip('Mute playback'), findsOneWidget);
    expect(find.byTooltip('Previous track on PC'), findsOneWidget);
    expect(find.byTooltip('Next track on PC'), findsOneWidget);
    final slider =
        tester
            .getSemantics(find.byKey(const ValueKey('playbackVolume')))
            .getSemanticsData();
    expect(slider.label, 'App playback volume');
    expect(slider.value, '50 percent');
    expect(slider.hasAction(SemanticsAction.increase), isTrue);
    expect(slider.hasAction(SemanticsAction.decrease), isTrue);
    await expectLater(tester, meetsGuideline(labeledTapTargetGuideline));
    await expectLater(tester, meetsGuideline(androidTapTargetGuideline));
    await expectLater(tester, meetsGuideline(textContrastGuideline));
  });

  testWidgets(
    'Read-only media and unavailable microphone expose disabled state',
    (tester) async {
      await tester.pumpWidget(
        audioControls(admin: false, micAllowed: false, recording: false),
      );
      for (final key in ['mediaPlayPause', 'phoneMicrophone']) {
        final data =
            tester.getSemantics(find.byKey(ValueKey(key))).getSemanticsData();
        expect(data.flagsCollection.isEnabled, isNot(Tristate.none));
        expect(data.flagsCollection.isEnabled, Tristate.isFalse);
      }
      expect(
        tester
            .widget<IconButton>(find.byKey(const ValueKey('mutePlayback')))
            .onPressed,
        isNotNull,
      );
      expect(
        tester
            .widget<Slider>(find.byKey(const ValueKey('playbackVolume')))
            .onChanged,
        isNotNull,
      );
      await tester.pumpWidget(audioControls(micAllowed: false));
      expect(
        tester
            .getSemantics(find.byKey(const ValueKey('phoneMicrophone')))
            .getSemanticsData()
            .flagsCollection
            .isEnabled,
        Tristate.isTrue,
      );
      expect(find.text('Stop microphone'), findsOneWidget);
      await tester.pumpWidget(audioControls(micBusy: true));
      expect(find.text('Changing microphone…'), findsOneWidget);
      expect(
        tester
            .widget<OutlinedButton>(
              find.byKey(const ValueKey('phoneMicrophone')),
            )
            .onPressed,
        isNull,
      );
    },
  );

  testWidgets('Phone audio remains readable and operable at 200 percent text', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(320, 568);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await tester.pumpWidget(audioControls(scale: 2));
    for (final target in [
      find.byKey(const ValueKey('phoneMicrophone')),
      find.byKey(const ValueKey('mutePlayback')),
      find.byKey(const ValueKey('micSource')),
      find.byKey(const ValueKey('mediaPlayPause')),
      find.text('Disconnect'),
    ]) {
      await tester.ensureVisible(target);
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
      await expectLater(tester, meetsGuideline(androidTapTargetGuideline));
      await expectLater(tester, meetsGuideline(labeledTapTargetGuideline));
    }
    await tester.ensureVisible(find.byKey(const ValueKey('micSource')));
    await tester.tap(find.byKey(const ValueKey('micSource')));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    expect(find.text('Mic: Bluetooth'), findsOneWidget);
  });

  testWidgets('Microphone works using keyboard focus and activation', (
    tester,
  ) async {
    var microphoneActions = 0;
    await tester.pumpWidget(
      audioControls(
        onMic: () {
          microphoneActions++;
        },
      ),
    );
    await tester.sendKeyEvent(LogicalKeyboardKey.tab);
    await tester.pump();
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pump();
    expect(microphoneActions, 1);
  });

  testWidgets('Named surface actions are keyboard-operable buttons', (
    tester,
  ) async {
    var activations = 0;
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: LabeledSurfaceButton(
            key: const ValueKey('surface'),
            label: 'Show latency breakdown',
            onPressed: () {
              activations++;
            },
            child: const Text('12 ms'),
          ),
        ),
      ),
    );
    final data =
        tester
            .getSemantics(find.byKey(const ValueKey('surface')))
            .getSemanticsData();
    expect(data.label, 'Show latency breakdown');
    expect(data.flagsCollection.isButton, isTrue);
    await tester.sendKeyEvent(LogicalKeyboardKey.tab);
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pump();
    expect(activations, 1);
  });

  testWidgets('Discovery and pairing remain scrollable at large text', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(320, 568);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (_) async => null);
    addTearDown(
      () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, null),
    );
    await tester.pumpWidget(
      MaterialApp(
        builder:
            (context, child) => MediaQuery(
              data: MediaQuery.of(context).copyWith(
                textScaler: const TextScaler.linear(2),
                disableAnimations: true,
              ),
              child: child!,
            ),
        home: const AudioBridgePage(),
      ),
    );
    await tester.pump();
    expect(tester.takeException(), isNull);
    await tester.ensureVisible(find.text('Scan for Servers'));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    const codec = StandardMethodCodec();
    await TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .handlePlatformMessage(
          channel.name,
          codec.encodeMethodCall(const MethodCall('onPairPending', '123456')),
          (_) {},
        );
    await tester.pump();
    await tester.ensureVisible(find.textContaining('Pairing code: 123456'));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    final pairing =
        tester
            .getSemantics(find.textContaining('Pairing code: 123456'))
            .getSemanticsData();
    expect(pairing.flagsCollection.isLiveRegion, isTrue);
    expect(pairing.label, contains('1 2 3 4 5 6'));
    await tester.ensureVisible(find.text('Awaiting approval...'));
    await tester.pumpAndSettle();
    final button = find.ancestor(
      of: find.text('Awaiting approval...'),
      matching: find.byType(FilledButton),
    );
    expect(
      tester
          .getRect(button)
          .contains(
            tester.getBottomRight(find.text('Awaiting approval...')) -
                const Offset(1, 1),
          ),
      isTrue,
    );
    expect(tester.takeException(), isNull);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });
}
