import socket
import select
import sys

def create_client_socket(host, port):
    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client_socket.connect((host, port))
    return client_socket

def send_message(client_socket, message):
    client_socket.sendall(message.encode('utf-8'))

def main():
    if len(sys.argv) != 3:
        print("Usage: python smallchat-client.py <host> <port>")
        sys.exit(1)

    host = sys.argv[1]
    port = int(sys.argv[2])

    client_socket = create_client_socket(host, port)
    inputs = [client_socket, sys.stdin]

    print(f"Connected to {host}:{port}")

    try:
        while True:
            readable, _, _ = select.select(inputs, [], [])
            
            for sock in readable:
                if sock == client_socket:
                    # Data from server
                    data = sock.recv(1024)
                    if not data:
                        print("Server disconnected")
                        sys.exit(0)
                    else:
                        print(data.decode('utf-8'), end='')
                elif sock == sys.stdin:
                    # User input
                    message = sys.stdin.readline()
                    send_message(client_socket, message)
    except KeyboardInterrupt:
        print("\nClient terminated.")
        sys.exit(0)

if __name__ == "__main__":
    main()
