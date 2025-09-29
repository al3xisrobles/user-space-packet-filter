# send_md.py
import socket, struct, time, random

# Wire: <u32 instr_id, u8 instr_type, u8 side, f32 px, f32 qty>  -> little-endian
PKT_FMT = "<IBBff"   # sizes: 4 + 1 + 1 + 4 + 4 = 14

def send_tick(sock, host, port, instr_id, instr_type, side, px, qty):
    payload = struct.pack(PKT_FMT, instr_id, instr_type, side, float(px), float(qty))
    sock.sendto(payload, (host, port))

if __name__ == "__main__":
    HOST = "EC2_PRIVATE_IP"  # e.g., "10.0.1.23"
    PORT =  5001             # must match your PacketFilter udp_port
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

    instr_types = [0,1,2]  # 0=UNDERLYING,1=OPTION,2=FUTURE
    while True:
        it  = random.choice(instr_types)
        px  = random.uniform(95.0, 105.0)
        qty = random.choice([0.1, 1.0, 5.0])
        side= random.choice([0,1])
        send_tick(sock, HOST, PORT, 12345, it, side, px, qty)
        time.sleep(0.01)
