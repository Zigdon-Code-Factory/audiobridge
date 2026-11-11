import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' show Tristate;
import 'package:audiobridge/main.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

const channel = MethodChannel('com.audiobridge/audio');

final class AppStorage extends IOOverrides {
  AppStorage(this.root);
  final String root;
  String resolve(String path) =>
      path.replaceFirst('/data/data/com.audiobridge.audiobridge/files', root);
  @override
  Directory createDirectory(String path) {
    final directory = super.createDirectory(resolve(path));
    return directory.path.startsWith(root)
        ? ImmediateDirectory(directory)
        : directory;
  }

  @override
  File createFile(String path) {
    final file = super.createFile(resolve(path));
    return file.path.startsWith(root) ? ImmediateFile(file) : file;
  }
}

final class ImmediateDirectory implements Directory {
  ImmediateDirectory(this.directory);
  final Directory directory;
  @override
  String get path => directory.path;
  @override
  Future<bool> exists() async => directory.existsSync();
  @override
  Future<Directory> create({bool recursive = false}) async {
    directory.createSync(recursive: recursive);
    return this;
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

final class ImmediateFile implements File {
  ImmediateFile(this.file);
  final File file;
  @override
  String get path => file.path;
  @override
  Future<bool> exists() async => file.existsSync();
  @override
  Future<String> readAsString({Encoding encoding = utf8}) async =>
      file.readAsStringSync(encoding: encoding);
  @override
  Future<File> writeAsString(
    String contents, {
    FileMode mode = FileMode.write,
    Encoding encoding = utf8,
    bool flush = false,
  }) async {
    file.writeAsStringSync(
      contents,
      mode: mode,
      encoding: encoding,
      flush: flush,
    );
    return this;
  }

  @override
  dynamic noSuchMethod(Invocation invocation) => super.noSuchMethod(invocation);
}

class NativeAudioFixture {
  bool recording = true;
  bool muted = false;
  double volume = 1;
  bool rejectStop = false;
  bool rejectStart = false;
  bool rejectState = false;
  bool rejectStopAudio = false;
  bool rejectDisconnect = false;
  bool rejectVolume = false;
  bool rejectMute = false;
  bool connected = false;
  bool playing = false;
  final List<String> calls = [];
  Completer<void>? stopPending;
  Completer<void>? startPending;

  Future<Object?> handle(MethodCall call) async {
    calls.add(call.method);
    switch (call.method) {
      case 'connect':
        connected = true;
        return 'accepted';
      case 'isDtlsActive':
        return connected;
      case 'isConnected':
        return connected;
      case 'startAudio':
        playing = true;
        return true;
      case 'stopAudio':
        if (rejectStopAudio) {
          throw PlatformException(code: 'playback_stop_failed');
        }
        playing = false;
        return null;
      case 'disconnect':
        if (rejectDisconnect) {
          throw PlatformException(code: 'disconnect_failed');
        }
        connected = false;
        return null;
      case 'startRecording':
        await startPending?.future;
        if (rejectStart) throw PlatformException(code: 'input_failed');
        recording = true;
        return true;
      case 'stopRecording':
        await stopPending?.future;
        if (rejectStop) throw PlatformException(code: 'stop_failed');
        recording = false;
        return null;
      case 'getAudioState':
        if (rejectState) throw PlatformException(code: 'state_failed');
        return {
          'recording': recording,
          'playing': playing,
          'muted': muted,
          'volume': volume,
        };
      case 'setVolume':
        if (rejectVolume) throw PlatformException(code: 'volume_failed');
        volume = call.arguments as double;
        return null;
      case 'setMuted':
        if (rejectMute) throw PlatformException(code: 'mute_failed');
        muted = call.arguments as bool;
        return null;
      case 'getRtt':
        return 0.0;
      case 'getLatencyBreakdown':
        return <String, Object>{};
      case 'sendDesktopControl':
        return true;
      default:
        return null;
    }
  }
}

Future<void> emitNative(
  WidgetTester tester,
  String method,
  Object arguments,
) async {
  const codec = StandardMethodCodec();
  await TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
      .handlePlatformMessage(
        channel.name,
        codec.encodeMethodCall(MethodCall(method, arguments)),
        (_) {},
      );
  await tester.pump();
}

Future<void> withConnectedApp(
  WidgetTester tester,
  Future<void> Function(NativeAudioFixture) check, {
  double scale = 1,
  bool reducedMotion = false,
}) async {
  final temporary = Directory.systemTemp.createTempSync(
    'audiobridge-accessibility-',
  );
  final history = File('${temporary.path}/connection_history.json');
  history.writeAsStringSync(
    jsonEncode([
      ServerInfo(
        name: 'Test PC',
        macAddress: 'test-mac',
        lastIp: '127.0.0.1',
        lastConnected: DateTime.now(),
      ).toJson(),
    ]),
  );
  final previous = IOOverrides.current;
  IOOverrides.global = AppStorage(temporary.path);
  final native = NativeAudioFixture();
  final messenger =
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
  messenger.setMockMethodCallHandler(channel, native.handle);
  try {
    await tester.pumpWidget(
      MaterialApp(
        theme: ThemeData(
          colorSchemeSeed: const Color(0xFF7C4DFF),
          brightness: Brightness.dark,
          useMaterial3: true,
        ),
        builder:
            (context, child) => MediaQuery(
              data: MediaQuery.of(context).copyWith(
                textScaler: TextScaler.linear(scale),
                disableAnimations: reducedMotion,
              ),
              child: child!,
            ),
        home: const AudioBridgePage(),
      ),
    );
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 50)),
    );
    await tester.pump();
    await tester.ensureVisible(find.text('Test PC'));
    await tester.tap(find.text('Test PC'));
    await tester.pump();
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 30)),
    );
    await tester.pump();
    expect(find.text('Streaming'), findsOneWidget);
    await check(native);
  } finally {
    native.rejectStop = false;
    native.rejectStart = false;
    native.rejectState = false;
    native.rejectStopAudio = false;
    native.rejectDisconnect = false;
    native.rejectVolume = false;
    native.rejectMute = false;
    if (native.startPending?.isCompleted == false) {
      native.startPending!.complete();
    }
    native.startPending = null;
    if (native.stopPending?.isCompleted == false) {
      native.stopPending!.complete();
    }
    native.stopPending = null;
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.runAsync(
      () => Future<void>.delayed(const Duration(milliseconds: 30)),
    );
    await tester.pump();
    messenger.setMockMethodCallHandler(channel, null);
    IOOverrides.global = previous;
    final parent = Directory.systemTemp.absolute.path;
    if (!temporary.absolute.path.startsWith(
      '$parent${Platform.pathSeparator}',
    )) {
      throw StateError('Unexpected temporary storage path');
    }
    temporary.deleteSync(recursive: true);
  }
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('Reduced motion keeps both PC settings expansions operable', (
    tester,
  ) async {
    await withConnectedApp(tester, (native) async {
      await emitNative(
        tester,
        'onDesktopState',
        jsonEncode({
          'version': 1,
          'admin': true,
          'settings': {
            'revision': 1,
            'pcm': true,
            'loopback': true,
            'jitter': 30,
            'frame': 10,
            'input': 0,
            'output': 0,
            'inputs': ['System default'],
            'outputs': ['System default'],
            'captureReady': true,
            'outputReady': true,
            'voiceReady': true,
            'noise': 1,
            'strength': 6,
            'gate': 1,
            'threshold': 50,
            'hold': 300,
            'lowCut': 0,
          },
        }),
      );
      for (final section in ['Audio routing and quality', 'Voice cleanup']) {
        await tester.ensureVisible(find.text(section));
        await tester.tap(find.text(section));
        await tester.pump();
        final control = find.byKey(
          ValueKey(
            section.startsWith('Audio')
                ? 'desktop-3-1-false'
                : 'desktop-7-1-false',
          ),
        );
        await tester.ensureVisible(control);
        await tester.pump();
        expect(control.hitTestable(), findsOneWidget);
        final data = tester.getSemantics(control).getSemanticsData();
        expect(
          data.label,
          contains(section.startsWith('Audio') ? 'Codec' : 'Noise suppression'),
        );
      }
      expect(tester.takeException(), isNull);
    }, reducedMotion: true);
  });

  for (final outcome in ['success', 'failure', 'timeout']) {
    testWidgets(
      'Microphone preserves keyboard position through delayed $outcome',
      (tester) async {
        await withConnectedApp(tester, (native) async {
          final microphone = find.byKey(const ValueKey('phoneMicrophone'));
          await tester.ensureVisible(microphone);
          final focus = tester.widget<OutlinedButton>(microphone).focusNode!;
          focus.requestFocus();
          await tester.pump();
          native.stopPending = Completer<void>();
          native.rejectStop = outcome == 'failure';
          await tester.sendKeyEvent(LogicalKeyboardKey.enter);
          await tester.pump();
          expect(tester.widget<OutlinedButton>(microphone).onPressed, isNull);
          if (outcome == 'timeout') {
            await tester.pump(const Duration(seconds: 4));
          } else {
            native.stopPending!.complete();
            await tester.pump();
          }
          await tester.pump();
          expect(focus.hasFocus, isTrue);
          if (outcome != 'success') {
            expect(native.recording, isTrue);
            final data = tester.getSemantics(microphone).getSemanticsData();
            expect(data.flagsCollection.isToggled, Tristate.isTrue);
            expect(
              find.textContaining('Could not stop phone microphone'),
              findsOneWidget,
            );
            expect(find.textContaining('Failed to start'), findsNothing);
          }
          await tester.sendKeyEvent(LogicalKeyboardKey.tab);
          await tester.pump();
          final mute =
              tester
                  .getSemantics(find.byKey(const ValueKey('mutePlayback')))
                  .getSemanticsData();
          expect(mute.flagsCollection.isFocused, Tristate.isTrue);
        });
      },
    );
  }

  testWidgets('Microphone completion does not steal deliberately moved focus', (
    tester,
  ) async {
    await withConnectedApp(tester, (native) async {
      final microphone = find.byKey(const ValueKey('phoneMicrophone'));
      await tester.ensureVisible(microphone);
      final focus = tester.widget<OutlinedButton>(microphone).focusNode!;
      focus.requestFocus();
      await tester.pump();
      native.stopPending = Completer<void>();
      await tester.sendKeyEvent(LogicalKeyboardKey.enter);
      await tester.pump();
      await tester.sendKeyEvent(LogicalKeyboardKey.tab);
      await tester.pump();
      final other = FocusManager.instance.primaryFocus;
      expect(other, isNot(focus));
      native.stopPending!.complete();
      await tester.pump();
      await tester.pump();
      expect(FocusManager.instance.primaryFocus, other);
    });
  });

  testWidgets(
    'Unverifiable microphone stop exposes unknown state and safe recovery',
    (tester) async {
      await withConnectedApp(tester, (native) async {
        native.rejectStop = true;
        native.rejectState = true;
        final microphone = find.byKey(const ValueKey('phoneMicrophone'));
        await tester.ensureVisible(microphone);
        await tester.tap(microphone);
        await tester.pump();
        await tester.pump();
        final data = tester.getSemantics(microphone).getSemanticsData();
        expect(data.flagsCollection.isToggled, Tristate.none);
        expect(data.value, contains('State unknown'));
        expect(find.text('Stop microphone'), findsOneWidget);
        expect(
          tester
              .getSemantics(find.byKey(const ValueKey('phoneMicrophoneStatus')))
              .getSemanticsData()
              .flagsCollection
              .isLiveRegion,
          isTrue,
        );
        native.rejectStop = false;
        native.rejectState = false;
        await tester.tap(microphone);
        await tester.pump();
        expect(native.recording, isFalse);
        expect(find.text('Mic Off'), findsOneWidget);
      });
    },
  );

  for (final applied in [true, false]) {
    testWidgets('PC media preserves focus after applied=$applied', (
      tester,
    ) async {
      await withConnectedApp(tester, (native) async {
        await emitNative(
          tester,
          'onDesktopState',
          jsonEncode({
            'version': 1,
            'admin': true,
            'settings': {'revision': 1},
          }),
        );
        final media = find.byKey(const ValueKey('mediaPlayPause'));
        await tester.ensureVisible(media);
        final focus = tester.widget<IconButton>(media).focusNode!;
        focus.requestFocus();
        await tester.pump();
        await tester.sendKeyEvent(LogicalKeyboardKey.enter);
        await tester.pump();
        expect(tester.widget<IconButton>(media).onPressed, isNull);
        expect(
          find.textContaining('Waiting for the PC command to finish'),
          findsOneWidget,
        );
        await emitNative(
          tester,
          'onDesktopResult',
          jsonEncode({
            'request': 1,
            'ok': applied,
            'message': 'Media rejected',
          }),
        );
        await tester.pump();
        expect(focus.hasFocus, isTrue);
        await tester.pump(const Duration(milliseconds: 350));
        expect(tester.takeException(), isNull);
      });
    });
  }

  testWidgets('Failed microphone start remains confirmed off with a retry', (
    tester,
  ) async {
    await withConnectedApp(tester, (native) async {
      final microphone = find.byKey(const ValueKey('phoneMicrophone'));
      await tester.ensureVisible(microphone);
      await tester.tap(microphone);
      await tester.pump();
      expect(native.recording, isFalse);
      native.rejectStart = true;
      await tester.tap(microphone);
      await tester.pump();
      expect(find.text('Mic Off'), findsOneWidget);
      expect(
        find.textContaining('Could not start phone microphone'),
        findsOneWidget,
      );
      expect(
        tester
            .getSemantics(microphone)
            .getSemanticsData()
            .flagsCollection
            .isToggled,
        Tristate.isFalse,
      );
      native.rejectStart = false;
      await tester.tap(microphone);
      await tester.pump();
      expect(native.recording, isTrue);
      expect(find.text('Mic On'), findsOneWidget);
    });
  });

  testWidgets('Media progress has non-live spoken units and long times wrap', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(320, 568);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await withConnectedApp(
      tester,
      (native) async {
        await emitNative(
          tester,
          'onMediaInfo',
          jsonEncode({
            't': 'A long program',
            'a': 'Artist',
            'p': 65000,
            'd': 600000,
            's': 1,
          }),
        );
        final media = find.byKey(const ValueKey('refreshMediaInfo'));
        await tester.ensureVisible(media);
        final data = tester.getSemantics(media).getSemanticsData();
        expect(
          data.value,
          contains('1 minute, 5 seconds elapsed; 10 minutes total'),
        );
        expect(data.flagsCollection.isLiveRegion, isFalse);
        await emitNative(
          tester,
          'onMediaInfo',
          jsonEncode({
            't': 'A long program',
            'a': 'Artist',
            'p': 6300000,
            'd': 7200000,
            's': 1,
          }),
        );
        await tester.ensureVisible(media);
        await tester.pump();
        expect(find.text('105:00'), findsOneWidget);
        expect(find.text('120:00'), findsOneWidget);
        expect(tester.takeException(), isNull);
      },
      scale: 2,
      reducedMotion: true,
    );
  });

  testWidgets('Failed source changes retain the confirmed microphone source', (
    tester,
  ) async {
    await withConnectedApp(tester, (native) async {
      final source = find.byKey(const ValueKey('micSource'));
      native.rejectStop = true;
      await tester.ensureVisible(source);
      await tester.tap(source);
      await tester.pumpAndSettle();
      await tester.tap(find.text('Mic: Bluetooth').last);
      await tester.pumpAndSettle();
      expect(tester.widget<DropdownButton<String>>(source).value, 'auto');
      expect(native.recording, isTrue);
      expect(
        find.textContaining('Could not stop phone microphone'),
        findsOneWidget,
      );
      native.rejectStop = false;
      await tester.tap(source);
      await tester.pumpAndSettle();
      await tester.tap(find.text('Mic: Bluetooth').last);
      await tester.pumpAndSettle();
      expect(tester.widget<DropdownButton<String>>(source).value, 'bluetooth');
      expect(native.recording, isTrue);
    }, reducedMotion: true);
  });

  for (final nativeDisconnect in [false, true]) {
    testWidgets(
      'Reconnect clears obsolete microphone work: native=$nativeDisconnect',
      (tester) async {
        await withConnectedApp(tester, (native) async {
          final microphone = find.byKey(const ValueKey('phoneMicrophone'));
          await tester.ensureVisible(microphone);
          native.stopPending = Completer<void>();
          await tester.tap(microphone);
          await tester.pump();
          expect(tester.widget<OutlinedButton>(microphone).onPressed, isNull);
          if (nativeDisconnect) {
            await emitNative(tester, 'onDisconnected', 'server_disconnect');
          } else {
            await tester.ensureVisible(find.text('Disconnect'));
            await tester.tap(find.text('Disconnect'));
            await tester.pump();
          }
          native.stopPending!.complete();
          await tester.pumpAndSettle();
          native.stopPending = null;
          await tester.ensureVisible(find.text('Test PC'));
          await tester.tap(find.text('Test PC'));
          await tester.pump();
          await tester.runAsync(
            () => Future<void>.delayed(const Duration(milliseconds: 30)),
          );
          await tester.pumpAndSettle();
          expect(find.text('Streaming'), findsOneWidget);
          expect(
            tester.widget<OutlinedButton>(microphone).onPressed,
            isNotNull,
          );
          final source = find.byKey(const ValueKey('micSource'));
          expect(
            tester.widget<DropdownButton<String>>(source).onChanged,
            isNotNull,
          );
          await tester.ensureVisible(microphone);
          await tester.tap(microphone);
          await tester.pumpAndSettle();
          expect(native.recording, isFalse);
          expect(find.text('Mic Off'), findsOneWidget);
        }, reducedMotion: true);
      },
    );
  }

  for (final stopResult in ['success', 'failure', 'unknown']) {
    testWidgets('Losing microphone ownership during start: $stopResult', (
      tester,
    ) async {
      await withConnectedApp(tester, (native) async {
        final microphone = find.byKey(const ValueKey('phoneMicrophone'));
        await tester.ensureVisible(microphone);
        await tester.tap(microphone);
        await tester.pumpAndSettle();
        expect(native.recording, isFalse);
        native.startPending = Completer<void>();
        await tester.tap(microphone);
        await tester.pump();
        await emitNative(
          tester,
          'onSessionInfo',
          jsonEncode({'micAllowed': false, 'clients': 2}),
        );
        native.rejectStop = stopResult != 'success';
        native.rejectState = stopResult == 'unknown';
        native.startPending!.complete();
        await tester.pumpAndSettle();
        native.startPending = null;
        if (stopResult == 'success') {
          expect(native.recording, isFalse);
          expect(
            find.text(
              'Phone microphone stopped because microphone access changed.',
            ),
            findsOneWidget,
          );
          expect(tester.widget<OutlinedButton>(microphone).onPressed, isNull);
        } else {
          expect(native.recording, isTrue);
          expect(
            tester.widget<OutlinedButton>(microphone).onPressed,
            isNotNull,
          );
          final data = tester.getSemantics(microphone).getSemanticsData();
          expect(
            data.flagsCollection.isToggled,
            stopResult == 'unknown' ? Tristate.none : Tristate.isTrue,
          );
          expect(find.text('Stop microphone'), findsOneWidget);
          native.rejectStop = false;
          native.rejectState = false;
          await tester.tap(microphone);
          await tester.pumpAndSettle();
          expect(native.recording, isFalse);
          expect(tester.widget<OutlinedButton>(microphone).onPressed, isNull);
        }
      }, reducedMotion: true);
    });
  }

  testWidgets('Device name remains operable with large text and keyboard', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(320, 568);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    addTearDown(tester.view.resetViewInsets);
    await withConnectedApp(
      tester,
      (native) async {
        await tester.ensureVisible(find.text('Disconnect'));
        await tester.tap(find.text('Disconnect'));
        await tester.pumpAndSettle();
        tester.view.viewInsets = const FakeViewPadding(bottom: 250);
        final edit = find.byKey(const ValueKey('editDeviceName'));
        await tester.ensureVisible(edit);
        await tester.tap(edit);
        await tester.pumpAndSettle();
        expect(tester.takeException(), isNull);
        final save = find.widgetWithText(TextButton, 'Save');
        await tester.ensureVisible(save);
        expect(save.hitTestable(), findsOneWidget);
        final cancel = find.widgetWithText(TextButton, 'Cancel');
        await tester.ensureVisible(cancel);
        await tester.tap(cancel);
        await tester.pumpAndSettle();
        expect(find.byType(AlertDialog), findsNothing);
        expect(tester.takeException(), isNull);
      },
      scale: 2,
      reducedMotion: true,
    );
  });

  for (final rejectCleanup in [false, true]) {
    testWidgets(
      'Late start retains stop recovery and reconciles: rejectCleanup=$rejectCleanup',
      (tester) async {
        await withConnectedApp(tester, (native) async {
          final mic = find.byKey(const ValueKey('phoneMicrophone'));
          await tester.ensureVisible(mic);
          await tester.tap(mic);
          await tester.pumpAndSettle();
          native.startPending = Completer<void>();
          await tester.tap(mic);
          await tester.pump();
          await tester.pump(const Duration(seconds: 4));
          await tester.pump();
          expect(native.recording, isFalse);
          expect(find.text('Stop microphone'), findsOneWidget);
          expect(
            tester
                .getSemantics(mic)
                .getSemanticsData()
                .flagsCollection
                .isToggled,
            Tristate.none,
          );
          native.rejectStop = rejectCleanup;
          native.startPending!.complete();
          await tester.pumpAndSettle();
          native.startPending = null;
          expect(native.recording, rejectCleanup);
          expect(
            tester
                .getSemantics(mic)
                .getSemanticsData()
                .flagsCollection
                .isToggled,
            rejectCleanup ? Tristate.isTrue : Tristate.isFalse,
          );
          if (rejectCleanup) {
            expect(tester.widget<OutlinedButton>(mic).onPressed, isNotNull);
            native.rejectStop = false;
            await tester.tap(mic);
            await tester.pumpAndSettle();
            expect(native.recording, isFalse);
          }
        }, reducedMotion: true);
      },
    );
  }

  testWidgets(
    'Initial connection ownership cleanup failure retains an enabled stop',
    (tester) async {
      await withConnectedApp(tester, (native) async {
        await tester.ensureVisible(find.text('Disconnect'));
        await tester.tap(find.text('Disconnect'));
        await tester.pumpAndSettle();
        native.startPending = Completer<void>();
        await tester.ensureVisible(find.text('Test PC'));
        await tester.tap(find.text('Test PC'));
        await tester.pump();
        native.rejectStop = true;
        await emitNative(
          tester,
          'onSessionInfo',
          jsonEncode({'micAllowed': false, 'clients': 2}),
        );
        native.startPending!.complete();
        await tester.runAsync(
          () => Future<void>.delayed(const Duration(milliseconds: 30)),
        );
        await tester.pumpAndSettle();
        native.startPending = null;
        expect(find.text('Streaming'), findsOneWidget);
        final mic = find.byKey(const ValueKey('phoneMicrophone'));
        expect(native.recording, isTrue);
        expect(tester.widget<OutlinedButton>(mic).onPressed, isNotNull);
        expect(
          tester.getSemantics(mic).getSemanticsData().flagsCollection.isToggled,
          Tristate.isTrue,
        );
        expect(
          find.textContaining('Could not stop phone microphone'),
          findsOneWidget,
        );
        native.rejectStop = false;
        await tester.ensureVisible(mic);
        await tester.tap(mic);
        await tester.pumpAndSettle();
        expect(native.recording, isFalse);
      }, reducedMotion: true);
    },
  );

  testWidgets('Initial microphone failure remains beside its retry control', (
    tester,
  ) async {
    await withConnectedApp(tester, (native) async {
      await tester.ensureVisible(find.text('Disconnect'));
      await tester.tap(find.text('Disconnect'));
      await tester.pumpAndSettle();
      native.rejectStart = true;
      await tester.ensureVisible(find.text('Test PC'));
      await tester.tap(find.text('Test PC'));
      await tester.runAsync(
        () => Future<void>.delayed(const Duration(milliseconds: 30)),
      );
      await tester.pumpAndSettle();
      await tester.pump(const Duration(seconds: 5));
      final status = find.byKey(const ValueKey('phoneMicrophoneStatus'));
      expect(status, findsOneWidget);
      expect(
        find.textContaining('Could not start phone microphone'),
        findsOneWidget,
      );
      expect(
        tester
            .getSemantics(status)
            .getSemanticsData()
            .flagsCollection
            .isLiveRegion,
        isTrue,
      );
      expect(find.text('Mic Off'), findsOneWidget);
    }, reducedMotion: true);
  });

  testWidgets('Disconnect bounds a hung stop and attempts remaining cleanup', (
    tester,
  ) async {
    await withConnectedApp(tester, (native) async {
      native.stopPending = Completer<void>();
      native.calls.clear();
      await tester.ensureVisible(find.text('Disconnect'));
      await tester.tap(find.text('Disconnect'));
      await tester.pump();
      expect(find.text('Disconnecting...'), findsOneWidget);
      expect(find.text('Streaming'), findsNothing);
      expect(
        native.calls,
        containsAll(['stopRecording', 'stopAudio', 'disconnect']),
      );
      await tester.pump(const Duration(seconds: 3));
      await tester.pumpAndSettle();
      expect(find.text('Shutdown not confirmed'), findsOneWidget);
      expect(find.byKey(const ValueKey('retryShutdown')), findsOneWidget);
      expect(native.recording, isTrue);
      native.stopPending!.complete();
      await tester.pumpAndSettle();
      native.stopPending = null;
      expect(native.recording, isFalse);
      expect(find.text('Scan for Servers'), findsOneWidget);
    }, reducedMotion: true);
  });

  testWidgets(
    'Failed shutdown keeps recovery until native state confirms stopping',
    (tester) async {
      await withConnectedApp(tester, (native) async {
        native.rejectStop = true;
        native.rejectStopAudio = true;
        native.rejectDisconnect = true;
        await tester.ensureVisible(find.text('Disconnect'));
        await tester.tap(find.text('Disconnect'));
        await tester.pumpAndSettle();
        expect(find.text('Shutdown not confirmed'), findsOneWidget);
        expect(find.text('Scan for Servers'), findsNothing);
        expect(native.recording, isTrue);
        final status = find.byKey(const ValueKey('shutdownStatus'));
        expect(
          tester
              .getSemantics(status)
              .getSemanticsData()
              .flagsCollection
              .isLiveRegion,
          isTrue,
        );
        native.rejectStop = false;
        native.rejectStopAudio = false;
        native.rejectDisconnect = false;
        await tester.tap(find.byKey(const ValueKey('retryShutdown')));
        await tester.pumpAndSettle();
        expect(native.recording, isFalse);
        expect(native.playing, isFalse);
        expect(native.connected, isFalse);
        expect(find.text('Scan for Servers'), findsOneWidget);
      }, reducedMotion: true);
    },
  );

  for (final mute in [false, true]) {
    testWidgets(
      'Rejected playback change reconciles actual state: mute=$mute',
      (tester) async {
        await withConnectedApp(tester, (native) async {
          final slider = find.byKey(const ValueKey('playbackVolume'));
          await tester.ensureVisible(slider);
          if (mute) {
            native.rejectMute = true;
            await tester.ensureVisible(
              find.byKey(const ValueKey('mutePlayback')),
            );
            await tester.tap(find.byKey(const ValueKey('mutePlayback')));
          } else {
            native.rejectVolume = true;
            tester.widget<Slider>(slider).onChanged!(0.2);
          }
          await tester.pumpAndSettle();
          expect(native.volume, 1);
          expect(native.muted, isFalse);
          expect(tester.widget<Slider>(slider).value, 1);
          expect(find.byTooltip('Mute playback'), findsOneWidget);
          expect(
            find.textContaining('Could not apply playback change'),
            findsOneWidget,
          );
          final status = find.byKey(const ValueKey('phonePlaybackStatus'));
          expect(
            tester
                .getSemantics(status)
                .getSemanticsData()
                .flagsCollection
                .isLiveRegion,
            isTrue,
          );
        }, reducedMotion: true);
      },
    );
  }

  testWidgets('Media rejection stays next to its action in a narrow viewport', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(320, 568);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    await withConnectedApp(tester, (native) async {
      await emitNative(
        tester,
        'onDesktopState',
        jsonEncode({
          'version': 1,
          'admin': true,
          'settings': {'revision': 1},
        }),
      );
      final play = find.byKey(const ValueKey('mediaPlayPause'));
      await tester.ensureVisible(play);
      await tester.tap(play);
      await tester.pump();
      await emitNative(
        tester,
        'onDesktopResult',
        jsonEncode({
          'request': 1,
          'ok': false,
          'message': 'No active media session.',
        }),
      );
      await tester.pumpAndSettle();
      final status = find.byKey(const ValueKey('phoneMediaStatus'));
      expect(tester.widget<Text>(status).data, 'No active media session.');
      expect(
        tester.getRect(status).overlaps(const Rect.fromLTWH(0, 0, 320, 568)),
        isTrue,
      );
      expect(
        tester
            .getSemantics(status)
            .getSemanticsData()
            .flagsCollection
            .isLiveRegion,
        isTrue,
      );
    }, reducedMotion: true);
  });

  testWidgets('Blank device names keep the editor open with validation', (
    tester,
  ) async {
    await withConnectedApp(tester, (native) async {
      await tester.ensureVisible(find.text('Disconnect'));
      await tester.tap(find.text('Disconnect'));
      await tester.pumpAndSettle();
      await tester.tap(find.byKey(const ValueKey('editDeviceName')));
      await tester.pumpAndSettle();
      await tester.enterText(find.byType(TextField), '   ');
      await tester.tap(find.text('Save'));
      await tester.pumpAndSettle();
      expect(find.byType(AlertDialog), findsOneWidget);
      expect(find.text('Enter a device name.'), findsOneWidget);
      await tester.tap(find.text('Cancel'));
      await tester.pumpAndSettle();
    }, reducedMotion: true);
  });

  testWidgets('Unmute at zero restores the last audible volume', (
    tester,
  ) async {
    await withConnectedApp(tester, (native) async {
      final slider = find.byKey(const ValueKey('playbackVolume'));
      await tester.ensureVisible(slider);
      tester.widget<Slider>(slider).onChanged!(0.4);
      await tester.pump();
      tester.widget<Slider>(slider).onChanged!(0);
      await tester.pump();
      expect(native.volume, 0);
      expect(native.muted, isTrue);
      await tester.ensureVisible(find.byTooltip('Unmute playback'));
      await tester.tap(find.byTooltip('Unmute playback'));
      await tester.pump();
      expect(native.volume, 0.4);
      expect(native.muted, isFalse);
      expect(find.byTooltip('Mute playback'), findsOneWidget);
      expect(tester.widget<Slider>(slider).value, 0.4);
    });
  });
}
