import 'package:audiobridge/main.dart' as app;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

// Runs against a real, already-paired LAN server. Never auto-approves pairing.
// flutter test integration_test/phone_controls_test.dart -d SERIAL \
//   --dart-define=TEST_SERVER=SERVER_NAME --dart-define=SOAK_SECONDS=180
void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('com.audiobridge/audio');
  const server = String.fromEnvironment('TEST_SERVER');
  const soakSeconds = int.fromEnvironment('SOAK_SECONDS', defaultValue: 180);

  testWidgets(
    'phone controls, reconnects, and encrypted soak',
    (tester) async {
      expect(
        server,
        isNotEmpty,
        reason: 'Supply TEST_SERVER for an already-paired LAN server',
      );
      addTearDown(() async {
        for (final method in ['stopRecording', 'stopAudio', 'disconnect']) {
          try {
            await channel.invokeMethod(method);
          } catch (_) {}
        }
      });
      Future<void> until(
        bool Function() condition,
        String label, {
        int seconds = 20,
      }) async {
        final deadline = DateTime.now().add(Duration(seconds: seconds));
        while (!condition() && DateTime.now().isBefore(deadline)) {
          await tester.pump(const Duration(milliseconds: 200));
        }
        expect(condition(), isTrue, reason: label);
        expect(tester.takeException(), isNull, reason: label);
      }

      Future<void> tap(Finder finder) async {
        await tester.ensureVisible(finder);
        await tester.tap(finder);
        await tester.pump(const Duration(milliseconds: 400));
        expect(tester.takeException(), isNull);
      }

      Finder key(String value) => find.byKey(ValueKey(value));
      Future<Map> audioState() async =>
          (await channel.invokeMethod<Map>('getAudioState'))!;
      Future<void> connect({bool scan = false}) async {
        if (scan) await tap(find.text('Scan for Servers'));
        await until(
          () => find.text(server).evaluate().isNotEmpty,
          'Server is listed',
        );
        await tap(find.text(server).first);
        await until(
          () => find.text('DTLS').evaluate().isNotEmpty,
          'Authenticated connection',
          seconds: 120,
        );
        expect(await channel.invokeMethod<bool>('isConnected'), isTrue);
        expect(await channel.invokeMethod<bool>('isDtlsActive'), isTrue);
        expect((await audioState())['playing'], isTrue);
        expect((await audioState())['muted'], isFalse);
        expect((await audioState())['volume'], 1.0);
      }

      Future<void> disconnect() async {
        await tap(find.text('Disconnect'));
        await until(
          () => find.text('Scan for Servers').evaluate().isNotEmpty,
          'Disconnect returns to server list',
        );
        expect(await channel.invokeMethod<bool>('isConnected'), isFalse);
        expect((await audioState())['playing'], isFalse);
        expect((await audioState())['recording'], isFalse);
      }

      app.main();
      await tester.pump(const Duration(seconds: 1));
      await tap(key('editDeviceName'));
      final originalName =
          tester.widget<TextField>(find.byType(TextField)).controller!.text;
      await tester.enterText(find.byType(TextField), 'Cancelled name');
      await tap(find.text('Cancel'));
      await tap(key('editDeviceName'));
      expect(
        tester.widget<TextField>(find.byType(TextField)).controller!.text,
        originalName,
      );
      await tester.enterText(find.byType(TextField), '   ');
      await tap(find.text('Save'));
      await tap(key('editDeviceName'));
      expect(
        tester.widget<TextField>(find.byType(TextField)).controller!.text,
        originalName,
      );
      await tester.enterText(
        find.byType(TextField),
        'AudioBridge control test',
      );
      await tap(find.text('Save'));
      await until(
        () => find.text('AudioBridge control test').evaluate().isNotEmpty,
        'Name saved',
      );
      await tap(key('editDeviceName'));
      await tester.enterText(find.byType(TextField), originalName);
      await tap(find.text('Save'));
      debugPrint('PASS device-name cancel, blank validation, save, restore');

      for (var i = 0; i < 3; ++i) {
        await tap(find.text('Scan for Servers'));
        await until(
          () => find.text('Stop Scanning').evaluate().isNotEmpty,
          'Scan started',
        );
        await tap(find.text('Stop Scanning'));
        await until(
          () => find.text('Scan for Servers').evaluate().isNotEmpty,
          'Scan stopped',
        );
      }
      await connect(scan: true);
      debugPrint('PASS scan start/stop and discovered-server connect');

      await tap(key('latencyBreakdown'));
      expect(find.text('Latency Breakdown'), findsOneWidget);
      expect(find.text('Estimated total'), findsOneWidget);
      Navigator.of(tester.element(find.text('Latency Breakdown'))).pop();
      await tester.pump(const Duration(milliseconds: 400));
      debugPrint('PASS latency breakdown open/dismiss');

      for (var i = 0; i < 5; ++i) {
        await tap(find.text('Mic On'));
        await until(
          () => find.text('Mic Off').evaluate().isNotEmpty,
          'Mic stopped',
        );
        expect((await audioState())['recording'], isFalse);
        await tap(find.text('Mic Off'));
        await until(
          () => find.text('Mic On').evaluate().isNotEmpty,
          'Mic started',
        );
        expect((await audioState())['recording'], isTrue);
      }
      debugPrint('PASS microphone toggle x10');

      await tap(find.text('Mic On'));
      final messenger =
          TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
      messenger.setMockMethodCallHandler(channel, (call) async {
        if (call.method == 'startRecording') return false;
        if (call.method == 'isConnected' || call.method == 'isDtlsActive') {
          return true;
        }
        return null;
      });
      try {
        await tap(find.text('Mic Off'));
        expect(find.text('Mic Off'), findsOneWidget);
        expect(find.textContaining('Failed to start mic'), findsOneWidget);
      } finally {
        messenger.setMockMethodCallHandler(channel, null);
      }
      expect((await audioState())['recording'], isFalse);
      await tap(find.text('Mic Off'));
      await until(
        () => find.text('Mic On').evaluate().isNotEmpty,
        'Retry after injected start failure',
      );
      expect((await audioState())['recording'], isTrue);
      debugPrint('PASS injected microphone-start failure and real retry');

      await tap(key('mutePlayback'));
      expect(find.text('0%'), findsOneWidget);
      expect((await audioState())['muted'], isTrue);
      await tap(key('mutePlayback'));
      expect(find.text('100%'), findsOneWidget);
      expect((await audioState())['muted'], isFalse);
      for (final fraction in [0.25, 0.75, 0.0, 1.0]) {
        final slider = key('playbackVolume');
        await tester.ensureVisible(slider);
        final rect = tester.getRect(slider);
        // Material slider has padded track ends; endpoints must be reachable.
        await tester.tapAt(
          Offset(rect.left + 1 + (rect.width - 2) * fraction, rect.center.dy),
        );
        await tester.pump(const Duration(milliseconds: 300));
        final value = tester.widget<Slider>(slider).value;
        expect((await audioState())['volume'], closeTo(value, 0.001));
        expect((await audioState())['muted'], value == 0.0);
        if (fraction == 0.0 || fraction == 1.0) {
          expect(value, fraction);
        } else {
          expect(value, inInclusiveRange(fraction - 0.15, fraction + 0.15));
        }
      }
      debugPrint('PASS mute/unmute and volume 25/75/0/100 percent');

      for (final source in [
        'Mic: Phone',
        'Mic: Bluetooth',
        'Mic: Auto (BT if available)',
      ]) {
        await tap(key('micSource'));
        await tap(find.text(source).last);
        await until(
          () => find.text('Mic On').evaluate().isNotEmpty,
          'Mic active after source change',
        );
        expect(find.text(source), findsOneWidget);
        expect(await channel.invokeMethod<bool>('isConnected'), isTrue);
        expect((await audioState())['recording'], isTrue);
      }
      debugPrint(
        'PASS Phone/Bluetooth/Auto source controls (physical Bluetooth routing needs a headset)',
      );

      await tap(find.text('Next'));
      await tester.pump(const Duration(seconds: 1));
      await tap(find.text('Prev'));
      await tester.pump(const Duration(seconds: 1));
      await tap(key('mediaPlayPause'));
      await tester.pump(const Duration(seconds: 2));
      await tap(key('mediaPlayPause'));
      await tester.pump(const Duration(seconds: 2));
      // The media card is optional when the PC has no active media session.
      final mediaCard = find.byKey(const ValueKey('refreshMediaInfo'));
      if (mediaCard.evaluate().isNotEmpty) await tap(mediaCard);
      debugPrint(
        'PASS media buttons dispatched; desktop logs verify command reception',
      );

      for (var i = 0; i < 3; ++i) {
        await tap(key('mutePlayback'));
        expect((await audioState())['muted'], isTrue);
        await disconnect();
        await connect();
      }
      debugPrint('PASS disconnect and saved-key reconnect x3');
      for (var second = 0; second < soakSeconds; ++second) {
        await tester.pump(const Duration(seconds: 1));
        expect(
          await channel.invokeMethod<bool>('isConnected'),
          isTrue,
          reason: 'Soak second $second',
        );
        expect(
          await channel.invokeMethod<bool>('isDtlsActive'),
          isTrue,
          reason: 'DTLS second $second',
        );
        expect(tester.takeException(), isNull);
        if ((second + 1) % 30 == 0) {
          debugPrint(
            'SOAK ${second + 1}/$soakSeconds seconds connected with DTLS',
          );
        }
      }
      await disconnect();
      debugPrint(
        'PASS all foreground controls and $soakSeconds-second encrypted soak',
      );
    },
    timeout: const Timeout(Duration(minutes: 12)),
  );
}
