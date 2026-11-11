import 'dart:convert';
import 'package:audiobridge/desktop_controls.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('desktop-controls-test');
  late DesktopControlState controls;
  late List<MethodCall> calls;
  Map<String, dynamic> snapshot({
    bool admin = false,
    int revision = 1,
    bool pcm = true,
  }) => {
    'version': 1,
    'admin': admin,
    'settings': {
      'revision': revision,
      'jitter': 10,
      'frame': 2,
      'pcm': pcm,
      'loopback': true,
      'inputs': ['Default', 'Speakers'],
      'outputs': ['Default', 'Cable'],
      'input': 0,
      'output': 0,
      'noise': 1,
      'strength': 100,
      'gate': 1,
      'threshold': 50,
      'hold': 300,
      'lowCut': 0,
      'captureReady': true,
      'voiceReady': true,
    },
  };
  setUp(() {
    calls = [];
    controls = DesktopControlState(channel);
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          calls.add(call);
          return true;
        });
  });
  tearDown(() {
    controls.dispose();
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null);
  });

  test('Older snapshots cannot restore revoked Admin access', () {
    controls.receiveState(jsonEncode(snapshot(admin: false, revision: 2)));
    controls.receiveState(jsonEncode(snapshot(admin: true, revision: 1)));
    expect(controls.admin, isFalse);
    expect(controls.canEdit, isFalse);
    expect(controls.settings['revision'], 2);
    controls.reset();
    controls.receiveState(jsonEncode(snapshot(admin: true, revision: 1)));
    expect(controls.canEdit, isTrue);
  });

  testWidgets('Old servers and readers cannot edit or send commands', (
    tester,
  ) async {
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: AnimatedBuilder(
            animation: controls,
            builder:
                (context, child) => SingleChildScrollView(
                  child: DesktopSettingsPanel(controls: controls),
                ),
          ),
        ),
      ),
    );
    expect(find.textContaining('unsupported or awaiting'), findsOneWidget);
    expect(controls.canEdit, isFalse);
    await controls.send(3, 0);
    controls.receiveState(jsonEncode(snapshot()));
    await tester.pump();
    expect(
      find.textContaining('Read only: ask the desktop owner'),
      findsOneWidget,
    );
    await tester.tap(find.text('Audio routing and quality'));
    await tester.pumpAndSettle();
    for (final widget in tester.widgetList<DropdownButtonFormField<int>>(
      find.byType(DropdownButtonFormField<int>),
    )) {
      expect(widget.onChanged, isNull);
    }
    await controls.send(16, 0x10);
    expect(calls, isEmpty);
    controls.reset(notify: false);
  });

  testWidgets(
    'Admin waits for application acknowledgement AND matching snapshot',
    (tester) async {
      controls.receiveState(jsonEncode(snapshot(admin: true)));
      expect(controls.canEdit, isTrue);
      await controls.send(3, 0);
      final arguments = calls.single.arguments as Map;
      expect(arguments['revision'], 1);
      expect(arguments['command'], 3);
      expect(controls.busy, isTrue);
      expect(controls.settings['pcm'], isTrue);
      controls.receiveResult(
        jsonEncode({'request': arguments['request'], 'ok': true}),
      );
      expect(controls.busy, isTrue);
      controls.receiveState(
        jsonEncode(snapshot(admin: true, revision: 2, pcm: false)),
      );
      expect(controls.busy, isFalse);
      expect(controls.message, contains('applied and confirmed'));
      expect(controls.settings['pcm'], isFalse);
      controls.reset(notify: false);
    },
  );

  testWidgets('Snapshots alone never claim successful application', (
    tester,
  ) async {
    controls.receiveState(jsonEncode(snapshot(admin: true)));
    await controls.send(3, 0);
    controls.receiveState(
      jsonEncode(snapshot(admin: true, revision: 2, pcm: false)),
    );
    expect(controls.busy, isTrue);
    await tester.pump(const Duration(seconds: 2));
    controls.receiveState(
      jsonEncode(snapshot(admin: true, revision: 2, pcm: false)),
    );
    await tester.pump(const Duration(seconds: 2));
    expect(controls.busy, isFalse);
    expect(controls.message, contains('No application confirmation'));
    controls.reset(notify: false);
  });

  testWidgets(
    'Failed application keeps confirmed values and rejects duplicate writes',
    (tester) async {
      controls.receiveState(jsonEncode(snapshot(admin: true)));
      await controls.send(3, 0);
      await controls.send(3, 1);
      expect(calls.length, 1);
      controls.receiveResult(
        '{"request":1,"ok":false,"message":"Device failed"}',
      );
      expect(controls.busy, isFalse);
      expect(controls.settings['pcm'], isTrue);
      expect(controls.message, 'Device failed');
      controls.reset(notify: false);
    },
  );

  testWidgets(
    'Connected downgrade cancels pending editing; late success cannot elevate',
    (tester) async {
      controls.receiveState(jsonEncode(snapshot(admin: true)));
      await controls.send(1, 20);
      controls.receiveState(jsonEncode(snapshot()));
      controls.receiveResult('{"request":1,"ok":true}');
      expect(controls.canEdit, isFalse);
      expect(controls.busy, isFalse);
      expect(controls.message, contains('permission changed'));
      await controls.send(1, 30);
      expect(calls.length, 1);
      controls.reset(notify: false);
    },
  );

  testWidgets('Permissions expire safely and reset on reconnect', (
    tester,
  ) async {
    controls.receiveState(jsonEncode(snapshot(admin: true)));
    await tester.pump(const Duration(seconds: 3));
    expect(controls.supported, isFalse);
    expect(controls.canEdit, isFalse);
    controls.receiveState(jsonEncode(snapshot(admin: true)));
    controls.reset();
    expect(controls.settings, isEmpty);
    expect(controls.canEdit, isFalse);
    controls.receiveState(
      '{"version":99,"admin":true,"settings":{"revision":1}}',
    );
    expect(controls.supported, isFalse);
  });

  testWidgets(
    'Semantic media result is required; unrelated acknowledgements ignored',
    (tester) async {
      controls.receiveState(jsonEncode(snapshot(admin: true)));
      await controls.send(16, 0x10);
      controls.receiveResult('{"request":99,"ok":true}');
      expect(controls.busy, isTrue);
      controls.receiveResult(
        '{"request":1,"ok":false,"message":"No active media session"}',
      );
      expect(controls.message, 'No active media session');
      await controls.send(16, 0x11);
      controls.receiveResult('{"request":2,"ok":true}');
      expect(controls.message, contains('media action applied'));
      controls.reset(notify: false);
    },
  );

  testWidgets(
    'Admin panel exposes narrow audio and voice settings without role controls',
    (tester) async {
      controls.receiveState(jsonEncode(snapshot(admin: true)));
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SingleChildScrollView(
              child: DesktopSettingsPanel(controls: controls),
            ),
          ),
        ),
      );
      expect(find.textContaining('Only the desktop owner'), findsOneWidget);
      await tester.tap(find.text('Audio routing and quality'));
      await tester.pumpAndSettle();
      expect(find.text('Capture mode'), findsOneWidget);
      expect(find.text('Codec'), findsOneWidget);
      expect(find.text('Frame duration'), findsOneWidget);
      expect(find.text('Jitter target'), findsOneWidget);
      expect(find.text('Control level'), findsNothing);
      expect(tester.takeException(), isNull);
      controls.reset(notify: false);
    },
  );

  for (final outcome in ['success', 'failure', 'timeout']) {
    testWidgets('PC setting keyboard focus survives delayed $outcome', (
      tester,
    ) async {
      controls.receiveState(jsonEncode(snapshot(admin: true)));
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: AnimatedBuilder(
              animation: controls,
              builder:
                  (context, child) => SingleChildScrollView(
                    child: DesktopSettingsPanel(controls: controls),
                  ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('Audio routing and quality'));
      await tester.pumpAndSettle();
      final field = find.byKey(const ValueKey('desktop-3-1-false'));
      await tester.ensureVisible(field);
      final widget = tester.widget<DropdownButtonFormField<int>>(field);
      final focus =
          tester
              .widget<DropdownButton<int>>(
                find.descendant(
                  of: field,
                  matching: find.byType(DropdownButton<int>),
                ),
              )
              .focusNode!;
      focus.requestFocus();
      await tester.pump();
      widget.onChanged!(0);
      await tester.pump();
      expect(controls.busy, isTrue);
      if (outcome == 'success') {
        controls.receiveResult('{"request":1,"ok":true}');
        controls.receiveState(
          jsonEncode(snapshot(admin: true, revision: 2, pcm: false)),
        );
      } else if (outcome == 'failure') {
        controls.receiveResult(
          '{"request":1,"ok":false,"message":"Not applied"}',
        );
      } else {
        await tester.pump(const Duration(seconds: 2));
        controls.receiveState(jsonEncode(snapshot(admin: true)));
        await tester.pump(const Duration(milliseconds: 2100));
      }
      await tester.pumpAndSettle();
      expect(controls.busy, isFalse);
      expect(focus.hasFocus, isTrue);
      controls.reset(notify: false);
    });
  }

  testWidgets('PC settings and endpoint choices reflow at 200 percent text', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(320, 640);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    controls.receiveState(jsonEncode(snapshot(admin: true)));
    await tester.pumpWidget(
      MaterialApp(
        home: MediaQuery(
          data: const MediaQueryData(textScaler: TextScaler.linear(2)),
          child: Scaffold(
            body: SingleChildScrollView(
              child: DesktopSettingsPanel(controls: controls),
            ),
          ),
        ),
      ),
    );
    await tester.ensureVisible(find.text('Audio routing and quality'));
    await tester.tap(find.text('Audio routing and quality'));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    for (final name in [
      'desktop-4-1-false',
      'desktop-5-0-false',
      'desktop-3-1-false',
    ]) {
      await tester.ensureVisible(find.byKey(ValueKey(name)));
      await tester.pumpAndSettle();
      expect(tester.takeException(), isNull);
      await expectLater(tester, meetsGuideline(labeledTapTargetGuideline));
      await expectLater(tester, meetsGuideline(androidTapTargetGuideline));
    }
    controls.reset(notify: false);
  });

  testWidgets(
    'Endpoint identity, output readiness and cleanup units are readable',
    (tester) async {
      final state = snapshot(admin: true);
      final settings = Map<String, dynamic>.from(state['settings'] as Map);
      state['settings'] = settings;
      settings.addAll({
        'output': 9,
        'outputName': 'CABLE Input (VB-Audio Virtual Cable)',
        'outputReady': false,
        'strength': 6,
      });
      controls.receiveState(jsonEncode(state));
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: SingleChildScrollView(
              child: DesktopSettingsPanel(controls: controls),
            ),
          ),
        ),
      );
      expect(
        find.textContaining('Phone microphone destination is unavailable'),
        findsOneWidget,
      );
      await tester.ensureVisible(find.text('Audio routing and quality'));
      await tester.tap(find.text('Audio routing and quality'));
      await tester.pumpAndSettle();
      final output = find.byKey(const ValueKey('desktop-6-9-false'));
      await tester.ensureVisible(output);
      expect(
        find.text('Current: CABLE Input (VB-Audio Virtual Cable)'),
        findsOneWidget,
      );
      expect(find.text('Current: 9'), findsNothing);
      await tester.ensureVisible(find.text('Voice cleanup'));
      await tester.tap(find.text('Voice cleanup'));
      await tester.pumpAndSettle();
      await tester.ensureVisible(
        find.byKey(const ValueKey('desktop-8-6-false')),
      );
      expect(find.text('Gentle · 6 dB'), findsOneWidget);
      await expectLater(tester, meetsGuideline(labeledTapTargetGuideline));
      await expectLater(tester, meetsGuideline(androidTapTargetGuideline));
      await expectLater(tester, meetsGuideline(textContrastGuideline));
      expect(tester.takeException(), isNull);
      controls.reset(notify: false);
    },
  );

  testWidgets('Completed PC change does not steal deliberately moved focus', (
    tester,
  ) async {
    final nextFocus = FocusNode();
    addTearDown(nextFocus.dispose);
    controls.receiveState(jsonEncode(snapshot(admin: true)));
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: AnimatedBuilder(
            animation: controls,
            builder:
                (context, child) => SingleChildScrollView(
                  child: Column(
                    children: [
                      DesktopSettingsPanel(controls: controls),
                      TextButton(
                        focusNode: nextFocus,
                        onPressed: () {},
                        child: const Text('Other action'),
                      ),
                    ],
                  ),
                ),
          ),
        ),
      ),
    );
    await tester.tap(find.text('Audio routing and quality'));
    await tester.pumpAndSettle();
    final field = find.byKey(const ValueKey('desktop-3-1-false'));
    final focus =
        tester
            .widget<DropdownButton<int>>(
              find.descendant(
                of: field,
                matching: find.byType(DropdownButton<int>),
              ),
            )
            .focusNode!;
    focus.requestFocus();
    await tester.pump();
    tester.widget<DropdownButtonFormField<int>>(field).onChanged!(0);
    await tester.pump();
    nextFocus.requestFocus();
    await tester.pump();
    controls.receiveResult('{"request":1,"ok":true}');
    controls.receiveState(
      jsonEncode(snapshot(admin: true, revision: 2, pcm: false)),
    );
    await tester.pumpAndSettle();
    expect(nextFocus.hasFocus, isTrue);
    expect(focus.hasFocus, isFalse);
    controls.reset(notify: false);
  });

  testWidgets('Route readiness changes have a stable live status summary', (
    tester,
  ) async {
    controls.receiveState(jsonEncode(snapshot(admin: true)));
    await tester.pumpWidget(
      MaterialApp(
        home: Scaffold(
          body: AnimatedBuilder(
            animation: controls,
            builder:
                (context, child) => SingleChildScrollView(
                  child: DesktopSettingsPanel(controls: controls),
                ),
          ),
        ),
      ),
    );
    final state = snapshot(admin: true);
    final settings = Map<String, dynamic>.from(state['settings'] as Map);
    state['settings'] = settings;
    settings.addAll({
      'captureReady': false,
      'outputReady': false,
      'voiceReady': false,
    });
    controls.receiveState(jsonEncode(state));
    await tester.pump();
    final notice = find.byKey(const ValueKey('desktop-readiness'));
    final semantics = tester.getSemantics(notice);
    final data = semantics.getSemanticsData();
    expect(data.flagsCollection.isLiveRegion, isTrue);
    expect(data.label, contains('PC capture is unavailable'));
    expect(data.label, contains('Phone microphone destination is unavailable'));
    expect(data.label, contains('Voice cleanup is unavailable'));
    controls.receiveState(jsonEncode(state));
    await tester.pump();
    expect(tester.getSemantics(notice).id, semantics.id);
    expect(tester.getSemantics(notice).getSemanticsData().label, data.label);
    controls.reset(notify: false);
  });

  testWidgets(
    'Pending and failed dropdown changes display only confirmed settings',
    (tester) async {
      controls.receiveState(jsonEncode(snapshot(admin: true)));
      await tester.pumpWidget(
        MaterialApp(
          home: Scaffold(
            body: AnimatedBuilder(
              animation: controls,
              builder:
                  (context, child) => SingleChildScrollView(
                    child: DesktopSettingsPanel(controls: controls),
                  ),
            ),
          ),
        ),
      );
      await tester.tap(find.text('Audio routing and quality'));
      await tester.pumpAndSettle();
      await tester.ensureVisible(
        find.byKey(const ValueKey('desktop-3-1-false')),
      );
      await tester.tap(find.byKey(const ValueKey('desktop-3-1-false')));
      await tester.pumpAndSettle();
      await tester.tap(find.text('Opus').last);
      await tester.pump();
      expect(controls.busy, isTrue);
      final pending = find.byKey(const ValueKey('desktop-3-1-true'));
      expect(pending, findsOneWidget);
      expect(
        tester.widget<DropdownButtonFormField<int>>(pending).initialValue,
        1,
      );
      controls.receiveResult(
        '{"request":1,"ok":false,"message":"Codec failed"}',
      );
      await tester.pump();
      final confirmed = find.byKey(const ValueKey('desktop-3-1-false'));
      expect(
        tester.widget<DropdownButtonFormField<int>>(confirmed).initialValue,
        1,
      );
      expect(controls.settings['pcm'], isTrue);
      expect(find.text('Codec failed'), findsOneWidget);
      controls.reset(notify: false);
    },
  );
}
