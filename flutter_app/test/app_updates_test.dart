import 'package:audiobridge/app_updates.dart';
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  const channel = MethodChannel('com.audiobridge/updates');
  late Map<String, dynamic> state;
  late List<String> calls;
  bool shutdownAllowed = true;
  bool rejectRequest = false;

  setUp(() {
    state = {
      'currentVersion': '0.2.0',
      'phase': 'idle',
      'message': 'Check for a newer version.',
    };
    calls = [];
    shutdownAllowed = true;
    rejectRequest = false;
    TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, (call) async {
          calls.add(call.method);
          if (call.method == 'getUpdateState') return state;
          if (rejectRequest) throw PlatformException(code: 'unavailable');
          if (call.method == 'checkForUpdates') {
            state = {
              ...state,
              'phase': 'checking',
              'message': 'Checking GitHub...',
            };
          }
          if (call.method == 'installUpdate') {
            state = {
              ...state,
              'phase': 'downloading',
              'message': 'Downloading and verifying...',
              'progress': 0.4,
            };
          }
          return null;
        });
  });
  tearDown(
    () => TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger
        .setMockMethodCallHandler(channel, null),
  );

  Future<void> open(WidgetTester tester, {bool large = false}) async {
    await tester.pumpWidget(
      MaterialApp(
        theme: ThemeData(brightness: Brightness.dark),
        builder:
            large
                ? (context, child) => MediaQuery(
                  data: MediaQuery.of(
                    context,
                  ).copyWith(textScaler: const TextScaler.linear(2)),
                  child: child!,
                )
                : null,
        home: Builder(
          builder:
              (context) => Scaffold(
                body: TextButton(
                  onPressed:
                      () => showDialog<void>(
                        context: context,
                        builder:
                            (_) => AppUpdateDialog(
                              beforeInstall: () async => shutdownAllowed,
                            ),
                      ),
                  child: const Text('Updates'),
                ),
              ),
        ),
      ),
    );
    await tester.tap(find.text('Updates'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 350));
  }

  Future<void> available(WidgetTester tester) async {
    state = {
      ...state,
      'phase': 'available',
      'version': '0.2.1',
      'message': 'A new version is available.',
    };
    await open(tester);
  }

  Future<void> approve(WidgetTester tester) async {
    await tester.tap(find.text('Update now'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Install update'));
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
  }

  Future<void> close(WidgetTester tester) async {
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  }

  testWidgets('checking is explicit and reports confirmed current version', (
    tester,
  ) async {
    await open(tester);
    expect(calls.where((call) => call == 'checkForUpdates'), isEmpty);
    await tester.tap(find.text('Check for updates'));
    await tester.pump();
    state = {...state, 'phase': 'current', 'message': 'You are up to date.'};
    await tester.pump(const Duration(milliseconds: 600));
    expect(find.text('You are up to date.'), findsOneWidget);
    expect(find.text('Installed: 0.2.0'), findsOneWidget);
    await close(tester);
  });
  testWidgets('canceling confirmation does not download or install', (
    tester,
  ) async {
    await available(tester);
    await tester.tap(find.text('Update now'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('Cancel'));
    await tester.pumpAndSettle();
    expect(calls.contains('installUpdate'), isFalse);
    expect(calls.contains('launchInstaller'), isFalse);
    await close(tester);
  });
  testWidgets('verified download waits for audio shutdown before installer', (
    tester,
  ) async {
    await available(tester);
    await approve(tester);
    expect(calls.where((call) => call == 'installUpdate').length, 1);
    expect(calls.contains('launchInstaller'), isFalse);
    state = {...state, 'phase': 'ready', 'message': 'Download verified.'};
    await tester.pump(const Duration(milliseconds: 600));
    await tester.pump();
    expect(calls.where((call) => call == 'launchInstaller').length, 1);
    await tester.pump(const Duration(seconds: 1));
    expect(calls.where((call) => call == 'launchInstaller').length, 1);
    await close(tester);
  });
  testWidgets(
    'unconfirmed shutdown blocks installation with persistent feedback',
    (tester) async {
      shutdownAllowed = false;
      await available(tester);
      await approve(tester);
      state = {...state, 'phase': 'ready'};
      await tester.pump(const Duration(milliseconds: 600));
      await tester.pump();
      expect(calls.contains('launchInstaller'), isFalse);
      expect(
        find.textContaining('Audio shutdown was not confirmed'),
        findsOneWidget,
      );
      await close(tester);
    },
  );
  testWidgets('bridge failure remains visible instead of claiming success', (
    tester,
  ) async {
    rejectRequest = true;
    await open(tester);
    await tester.tap(find.text('Check for updates'));
    await tester.pumpAndSettle();
    expect(find.textContaining('could not be confirmed'), findsOneWidget);
    await tester.pump(const Duration(seconds: 1));
    expect(find.textContaining('could not be confirmed'), findsOneWidget);
    await close(tester);
  });
  testWidgets('update dialogs reflow at 320 pixels and 200 percent text', (
    tester,
  ) async {
    tester.view.physicalSize = const Size(320, 568);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);
    state = {...state, 'phase': 'available', 'version': '0.2.1'};
    await open(tester, large: true);
    await tester.ensureVisible(find.text('Update now'));
    await tester.tap(find.text('Update now'));
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
    await tester.ensureVisible(find.text('Cancel'));
    await tester.tap(find.text('Cancel'));
    await tester.pumpAndSettle();
    await close(tester);
  });
  testWidgets('pending checks retain a meaningful keyboard focus target', (
    tester,
  ) async {
    await open(tester);
    tester
        .widget<FilledButton>(find.byKey(const ValueKey('updateAction')))
        .focusNode!
        .requestFocus();
    await tester.pump();
    await tester.sendKeyEvent(LogicalKeyboardKey.enter);
    await tester.pump();
    await tester.pump(const Duration(milliseconds: 100));
    expect(FocusManager.instance.primaryFocus?.debugLabel, 'Update status');
    state = {...state, 'phase': 'current', 'message': 'You are up to date.'};
    await tester.pump(const Duration(milliseconds: 600));
    expect(FocusManager.instance.primaryFocus?.debugLabel, 'Update status');
    await close(tester);
  });
  testWidgets('a requested session opens required system confirmation once', (
    tester,
  ) async {
    await available(tester);
    await approve(tester);
    state = {...state, 'phase': 'ready'};
    await tester.pump(const Duration(milliseconds: 600));
    await tester.pump();
    state = {
      ...state,
      'phase': 'confirmation',
      'canCancel': true,
      'installRevision': 2,
    };
    await tester.pump(const Duration(milliseconds: 600));
    await tester.pump();
    expect(calls.where((call) => call == 'confirmInstallation').length, 1);
    await tester.pump(const Duration(seconds: 1));
    expect(calls.where((call) => call == 'confirmInstallation').length, 1);
    await close(tester);
  });
  testWidgets(
    'reopened confirmation is explicit and does not resubmit the APK',
    (tester) async {
      state = {
        ...state,
        'phase': 'confirmation',
        'canCancel': true,
        'message': 'Android requires approval.',
      };
      await open(tester);
      expect(calls.contains('confirmInstallation'), isFalse);
      await tester.tap(find.text('Confirm installation'));
      await tester.pump();
      expect(calls.where((call) => call == 'confirmInstallation').length, 1);
      expect(calls.contains('launchInstaller'), isFalse);
      expect(calls.contains('installUpdate'), isFalse);
      await close(tester);
    },
  );
  testWidgets(
    'active installation blocks duplicates and permits cancellation',
    (tester) async {
      state = {
        ...state,
        'phase': 'installing',
        'canCancel': true,
        'message': 'Android is installing...',
      };
      await open(tester);
      expect(
        tester
            .widget<FilledButton>(find.byKey(const ValueKey('updateAction')))
            .onPressed,
        isNull,
      );
      await tester.tap(find.text('Cancel installation'));
      await tester.pump();
      expect(calls.where((call) => call == 'cancelInstallation').length, 1);
      expect(calls.contains('installUpdate'), isFalse);
      await close(tester);
    },
  );
  testWidgets(
    'authoritative session progress retires an obsolete request error',
    (tester) async {
      rejectRequest = true;
      state = {
        ...state,
        'phase': 'confirmation',
        'canCancel': true,
        'installRevision': 1,
      };
      await open(tester);
      await tester.tap(find.text('Confirm installation'));
      await tester.pumpAndSettle();
      expect(find.textContaining('could not be confirmed'), findsOneWidget);
      state = {
        ...state,
        'phase': 'error',
        'canCancel': false,
        'installRevision': 2,
        'message': 'Installation was canceled.',
      };
      await tester.pump(const Duration(milliseconds: 600));
      expect(find.text('Installation was canceled.'), findsOneWidget);
      expect(find.textContaining('could not be confirmed'), findsNothing);
      await close(tester);
    },
  );
  testWidgets(
    'large-text confirmation and cancellation controls remain reachable',
    (tester) async {
      tester.view.physicalSize = const Size(320, 568);
      tester.view.devicePixelRatio = 1;
      addTearDown(tester.view.resetPhysicalSize);
      addTearDown(tester.view.resetDevicePixelRatio);
      state = {
        ...state,
        'phase': 'confirmation',
        'canCancel': true,
        'message':
            'Android requires approval. Your installed app is unchanged.',
      };
      await open(tester, large: true);
      await tester.ensureVisible(find.text('Confirm installation'));
      expect(tester.takeException(), isNull);
      await close(tester);
    },
  );
}
