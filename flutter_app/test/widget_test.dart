import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:audiobridge/main.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('com.audiobridge/audio');
  testWidgets('Shows disconnected AudioBridge and cleans up audio', (tester) async {
    final calls = <String>[];
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
      calls.add(call.method);
      return null;
    });
    addTearDown(() {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, null);
    });
    await tester.pumpWidget(const AudioBridgeApp());
    await tester.pump();
    expect(find.text('AudioBridge'), findsWidgets);
    expect(find.text('Scan for Servers'), findsOneWidget);
    expect(calls, contains('requestPermissions'));
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    expect(calls, containsAll(['stopAudio', 'stopRecording', 'disconnect']));
    expect(tester.takeException(), isNull);
  });
  test('Server history round-trips identity and pairing key', () {
    final server = ServerInfo(name: 'PC', macAddress: 'aa:bb:cc:dd:ee:ff',
        lastIp: '192.168.1.5', lastConnected: DateTime.utc(2026, 9, 7),
        pskHex: '0123456789abcdef', active: true, macVerified: true);
    final restored = ServerInfo.fromJson(server.toJson());
    expect(restored.toJson(), server.toJson());
    expect(restored.active, isFalse);
    expect(restored.macVerified, isFalse);
    expect(ServerInfo.fromJson({}).pskHex, isEmpty);
  });
  testWidgets('Handles native stream config, frame size, and pair pending callbacks cleanly', (tester) async {
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async => null);
    addTearDown(() {
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .setMockMethodCallHandler(channel, null);
    });
    await tester.pumpWidget(const AudioBridgeApp());
    await tester.pump();

    const codec = StandardMethodCodec();
    Future<void> emitNative(String method, Object? args) async {
      final data = codec.encodeMethodCall(MethodCall(method, args));
      await TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
          .handlePlatformMessage(channel.name, data, (_) {});
      await tester.pump();
    }

    await emitNative('onStreamConfig', '{"jitter":10,"samples":120,"pcm":true}');
    await emitNative('onFrameSizeUpdate', '2');
    await emitNative('onStreamConfig', '{"jitter":15,"samples":240,"pcm":false}');
    await emitNative('onSettingsUpdate', '12');
    await emitNative('onPairPending', '');
    expect(find.text('Awaiting approval...'), findsOneWidget);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
    expect(tester.takeException(), isNull);
  });
}
