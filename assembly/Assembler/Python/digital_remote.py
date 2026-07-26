#!/usr/bin/env python3
"""
digital_remote.py - Digital (hneemann) TCP remote interface, shared by
asm_machinecode.py's --digital flag.
"""

# --------------------------------------------------------------------------
# Digital (hneemann) TCP remote interface.
#
# Protocol (reverse-engineered from hneemann/Assembler's RemoteInterface.java):
#   - Connect to 127.0.0.1:41114 (must be enabled in Digital's Settings -
#     the remote server is OFF by default in current versions).
#   - Every message (both directions) uses Java's DataOutputStream.writeUTF
#     wire format: a 2-byte big-endian length prefix (byte length of the
#     UTF-8 payload, not character count) followed by the UTF-8 bytes.
#   - Commands: "start:<hexfilepath>", "debug:<hexfilepath>", "run",
#     "step", "stop". start/debug tell Digital to load that hex file into
#     the circuit's program memory; start also begins free-running clocking,
#     debug does not (leaves you to step/run manually from Digital's GUI).
#   - Response: "ok", "ok:<hex address>" (run/step), or an error string.
# --------------------------------------------------------------------------
class DigitalRemoteError(Exception):
    pass


def digital_send(command, host='127.0.0.1', port=41114, timeout=5.0):
    import socket

    def recv_exact(sock, n):
        buf = b''
        while len(buf) < n:
            chunk = sock.recv(n - len(buf))
            if not chunk:
                raise DigitalRemoteError(
                    "connection closed while waiting for Digital's response")
            buf += chunk
        return buf

    try:
        with socket.create_connection((host, port), timeout=timeout) as s:
            payload = command.encode('utf-8')
            s.sendall(len(payload).to_bytes(2, 'big') + payload)
            resp_len = int.from_bytes(recv_exact(s, 2), 'big')
            response = recv_exact(s, resp_len).decode('utf-8')
    except OSError as e:
        raise DigitalRemoteError(
            f"could not reach Digital at {host}:{port} - is Digital running "
            f"with the circuit open and remote control enabled in "
            f"Settings? ({e})")

    if not (response == 'ok' or response.startswith('ok:')):
        raise DigitalRemoteError(f"Digital reported an error: {response}")
    return response


