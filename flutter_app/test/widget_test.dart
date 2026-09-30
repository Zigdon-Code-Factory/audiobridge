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
}
