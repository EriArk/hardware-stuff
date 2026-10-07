"""Check the local narration worker without generating audio."""
import socket

with socket.socket(socket.AF_UNIX) as client:
    client.settimeout(5)
    client.connect("/run/speech/speech.sock")
    client.sendall(b"GET /health HTTP/1.0\r\n\r\n")
    response = client.makefile("rb")
    with response:
        assert response.readline(1024).split()[1:2] == [b"200"]
