import socket

PICO_IP = "192.168.4.1"
PORT = 5000

with socket.create_connection((PICO_IP, PORT), timeout=5) as client:
    client.sendall(b"get_number\n")
    response = client.recv(64).decode().strip()

number = int(response)
print("Pico W response:", number)
print("Wi-Fi communication succeeded!")
