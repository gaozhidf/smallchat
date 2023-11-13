import socket
import select

def create_server_socket(host, port):
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server_socket.bind((host, port))
    server_socket.listen(5)
    return server_socket

def accept_client(server_socket):
    client_socket, client_address = server_socket.accept()
    print(f"Accepted connection from {client_address}")
    return client_socket

def handle_client_data(client_socket, data):
    # Handle client data as needed
    print(f"Received data from client: {data.decode('utf-8')}")
    # Here, you can implement the logic to process or respond to the client data.

def main():
    host = "127.0.0.1"
    port = 12345

    server_socket = create_server_socket(host, port)
    inputs = [server_socket]
    clients = {}

    print(f"Server listening on {host}:{port}")

    while True:
        readable, _, _ = select.select(inputs, [], [])
        
        for sock in readable:
            if sock == server_socket:
                # New client connection
                client_socket = accept_client(server_socket)
                inputs.append(client_socket)
                clients[client_socket] = b""  # Initialize client data buffer
            else:
                # Existing client data
                data = sock.recv(1024)
                if not data:
                    # Client disconnected
                    print(f"Client {sock.getpeername()} disconnected")
                    inputs.remove(sock)
                    del clients[sock]
                else:
                    # Handle client data
                    clients[sock] += data
                    if b'\n' in clients[sock]:
                        # End of message reached, process the complete message
                        message, remaining = clients[sock].split(b'\n', 1)
                        handle_client_data(sock, message)
                        clients[sock] = remaining

if __name__ == "__main__":
    main()
