#!/usr/bin/env python3
"""
Controlled E2E test using subprocess with pipe-based stdin.
"""
import subprocess, time, os, threading

DIR = '/mnt/c/Users/pkone/OneDrive/Desktop/Os/Socket_Application'
os.chdir(DIR)

# Kill stale instances
subprocess.run(['pkill', '-f', './server'], capture_output=True)
subprocess.run(['pkill', '-f', './client'], capture_output=True)
time.sleep(0.4)

# ── Start server ──
srv = subprocess.Popen(
    ['./server'],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
    bufsize=0
)
time.sleep(1.2)   # let it bind port

# ── Start client ──
cli = subprocess.Popen(
    ['./client'],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.STDOUT,
    text=True,
    bufsize=0
)
cli.stdin.write('127.0.0.1\n9001\n')
cli.stdin.flush()
time.sleep(1.2)   # let client connect

# ── Feed server: ENTER (show menu), then "1" (first .c file) ──
srv.stdin.write('\n')
srv.stdin.flush()
time.sleep(0.5)
srv.stdin.write('1\n')
srv.stdin.flush()

# ── Wait for job to complete (compile + run) ──
time.sleep(4.0)

# Collect output without blocking
srv_out_lines = []
cli_out_lines = []

def drain(proc, lines):
    try:
        while True:
            line = proc.stdout.readline()
            if not line:
                break
            lines.append(line)
    except:
        pass

srv.stdin.close()
cli.stdin.close()
time.sleep(0.5)
srv.terminate()
cli.terminate()
time.sleep(0.5)

try: srv_out_lines = srv.stdout.readlines()
except: pass
try: cli_out_lines = cli.stdout.readlines()
except: pass

srv_out = ''.join(srv_out_lines)
cli_out = ''.join(cli_out_lines)

print("=== SERVER OUTPUT ===")
print(srv_out if srv_out else "(empty)")
print("=== CLIENT OUTPUT ===")
print(cli_out if cli_out else "(empty)")

with open(DIR + '/srv_out.txt', 'w') as f: f.write(srv_out)
with open(DIR + '/cli_out.txt', 'w') as f: f.write(cli_out)

ok = 'Hello from client code' in srv_out
print('✅ PASS' if ok else '❌ FAIL - expected output not in server log')
