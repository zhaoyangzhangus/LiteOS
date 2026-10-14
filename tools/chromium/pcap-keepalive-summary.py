"""Report real guest IPv4 keepalive probes without logging TCP payloads."""
import argparse
import ast
import json
from pathlib import Path
import struct

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--capture-file', type=Path,
                    default=root/'build/chrome-network.pcap')
args = parser.parse_args()
tree = ast.parse((root/'tools/chromium/pcap-startup-summary.py').read_text())
decoder = next(node for node in tree.body if isinstance(node, ast.FunctionDef)
               and node.name == 'tcp_payload')
namespace = {'struct': struct}
exec(compile(ast.Module(body=[decoder], type_ignores=[]), '<tcp-decoder>', 'exec'), namespace)
decode = namespace['tcp_payload']
flows = {}
probes = []
with args.capture_file.open('rb') as capture:
    header = capture.read(24)
    assert len(header) == 24
    endian = '<' if header[:4] == b'\xd4\xc3\xb2\xa1' else '>'
    assert struct.unpack(endian+'I', header[:4])[0] == 0xa1b2c3d4
    assert struct.unpack(endian+'I', header[20:24])[0] == 1
    while record := capture.read(16):
        if len(record) != 16:
            break  # The live capture may still be writing its final record.
        seconds, micros, length, _ = struct.unpack(endian+'IIII', record)
        packet = capture.read(length)
        if len(packet) != length:
            break
        decoded = decode(packet)
        if decoded is None:
            continue
        src, dst, payload, seq, ack, flags = decoded
        source, destination = packet[26:30], packet[30:34]
        key = (source, src, destination, dst)
        reverse = (destination, dst, source, src)
        stamp = seconds+micros/1000000
        if flags & 2:
            flows[key] = {'next': (seq+1+len(payload)) & 0xffffffff,
                          'activity': stamp, 'pending': None}
            continue
        state = flows.get(key)
        if flags == 16 and not payload and state and seq == (state['next']-1) & 0xffffffff:
            probe = {'source_port': src, 'destination_port': dst,
                     'idle_seconds': stamp-state['activity'],
                     'timestamp': stamp, 'reply_seconds': None}
            probes.append(probe)
            state['pending'] = probe
            continue
        peer = flows.get(reverse)
        if flags & 16 and peer and ack == peer['next']:
            if peer['pending'] is not None:
                peer['pending']['reply_seconds'] = stamp-peer['pending']['timestamp']
                peer['pending'] = None
            peer['activity'] = stamp
        if state:
            consumed = len(payload)+bool(flags & 1)
            end = (seq+consumed) & 0xffffffff
            if consumed and ((end-state['next']) & 0xffffffff) < 0x80000000:
                state['next'] = end
            state['activity'] = stamp
        if flags & 5:
            flows.pop(key, None)
            flows.pop(reverse, None)
print(json.dumps({'ipv4_probes': probes,
                  'acknowledged': sum(p['reply_seconds'] is not None for p in probes)}, indent=2))
