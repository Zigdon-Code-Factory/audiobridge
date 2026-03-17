import http.server, os, sys

os.chdir(r"C:\Users\Zack\Desktop\Projects\Personal\audiobridge\flutter_app\build\app\outputs\flutter-apk")

handler = http.server.SimpleHTTPRequestHandler
server = http.server.HTTPServer(("0.0.0.0", 8080), handler)
print(f"Serving on http://192.168.0.78:8080/app-release.apk")
print("Press Ctrl+C to stop")
server.serve_forever()
