#!/usr/bin/env bash
# Behavioral tests for mini-redis C++ transpilation.
# Mirrors the Rust tests in tests/server.rs.
#
# Each test starts a fresh server on a random port, sends raw RESP commands
# via python (for precise binary control), and compares byte-level responses.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVER="$SCRIPT_DIR/build/mini-redis-server"
PASS=0
FAIL=0
TOTAL=0

# Pick a base port
BASE_PORT=17000

start_server() {
    local port=$1
    "$SERVER" --port "$port" >/dev/null 2>/dev/null &
    local pid=$!
    sleep 0.3
    echo "$pid"
}

stop_server() {
    local pid=$1
    kill "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
}

run_test() {
    local name=$1
    local port=$2
    shift 2
    TOTAL=$((TOTAL + 1))

    local pid
    pid=$(start_server "$port")

    local result
    if result=$("$@" 2>&1); then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        echo "    $result"
        FAIL=$((FAIL + 1))
    fi

    stop_server "$pid"
}

echo "=== mini-redis C++ behavioral tests ==="
echo ""

# ---------- Test 1: key_value_get_set ----------
test_kv_get_set() {
    local port=$1
    python3 -c "
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', $port))

# GET hello (missing)
s.sendall(b'*2\r\n\$3\r\nGET\r\n\$5\r\nhello\r\n')
r = s.recv(1024)
assert r == b'\$-1\r\n', f'Expected nil, got {r!r}'

# SET hello world
s.sendall(b'*3\r\n\$3\r\nSET\r\n\$5\r\nhello\r\n\$5\r\nworld\r\n')
r = s.recv(1024)
assert r == b'+OK\r\n', f'Expected +OK, got {r!r}'

# GET hello (present)
s.sendall(b'*2\r\n\$3\r\nGET\r\n\$5\r\nhello\r\n')
r = s.recv(1024)
assert r == b'\$5\r\nworld\r\n', f'Expected world, got {r!r}'

s.close()
print('ok')
"
}
run_test "key_value_get_set" $((BASE_PORT + 1)) test_kv_get_set $((BASE_PORT + 1))

# ---------- Test 2: ping_pong ----------
test_ping_pong() {
    local port=$1
    python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', $port))

# PING (no arg)
s.sendall(b'*1\r\n\$4\r\nPING\r\n')
r = s.recv(1024)
assert r == b'+PONG\r\n', f'Expected PONG, got {r!r}'

# PING with message
s.sendall(b'*2\r\n\$4\r\nPING\r\n\$5\r\nhello\r\n')
r = s.recv(1024)
assert r == b'\$5\r\nhello\r\n', f'Expected hello, got {r!r}'

s.close()
print('ok')
"
}
run_test "ping_pong" $((BASE_PORT + 2)) test_ping_pong $((BASE_PORT + 2))

# ---------- Test 3: unknown_command ----------
test_unknown_command() {
    local port=$1
    python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', $port))

# FOO (unknown)
s.sendall(b'*2\r\n\$3\r\nFOO\r\n\$5\r\nhello\r\n')
r = s.recv(1024)
assert r == b\"-ERR unknown command 'foo'\r\n\", f'Expected error, got {r!r}'

s.close()
print('ok')
"
}
run_test "unknown_command" $((BASE_PORT + 3)) test_unknown_command $((BASE_PORT + 3))

# ---------- Test 4: pub_sub basic ----------
test_pub_sub() {
    local port=$1
    python3 -c "
import socket, time
# Publisher
pub = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
pub.connect(('127.0.0.1', $port))

# Publish with no subscribers
pub.sendall(b'*3\r\n\$7\r\nPUBLISH\r\n\$5\r\nhello\r\n\$5\r\nworld\r\n')
r = pub.recv(1024)
assert r == b':0\r\n', f'Expected :0, got {r!r}'

# Subscriber
sub = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sub.connect(('127.0.0.1', $port))
sub.sendall(b'*2\r\n\$9\r\nSUBSCRIBE\r\n\$5\r\nhello\r\n')
r = sub.recv(1024)
assert r == b'*3\r\n\$9\r\nsubscribe\r\n\$5\r\nhello\r\n:1\r\n', f'Expected subscribe confirm, got {r!r}'

# Publish with one subscriber
time.sleep(0.1)
pub.sendall(b'*3\r\n\$7\r\nPUBLISH\r\n\$5\r\nhello\r\n\$5\r\nworld\r\n')
r = pub.recv(1024)
assert r == b':1\r\n', f'Expected :1, got {r!r}'

# Subscriber should receive the message
time.sleep(0.1)
r = sub.recv(1024)
assert r == b'*3\r\n\$7\r\nmessage\r\n\$5\r\nhello\r\n\$5\r\nworld\r\n', f'Expected message, got {r!r}'

pub.close()
sub.close()
print('ok')
"
}
run_test "pub_sub" $((BASE_PORT + 4)) test_pub_sub $((BASE_PORT + 4))

# ---------- Test 5: send_error_get_set_after_subscribe ----------
test_error_after_subscribe() {
    local port=$1
    python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', $port))

# Subscribe
s.sendall(b'*2\r\n\$9\r\nsubscribe\r\n\$5\r\nhello\r\n')
r = s.recv(1024)
assert r == b'*3\r\n\$9\r\nsubscribe\r\n\$5\r\nhello\r\n:1\r\n', f'Expected subscribe, got {r!r}'

# SET (invalid in subscribe mode)
s.sendall(b'*3\r\n\$3\r\nSET\r\n\$5\r\nhello\r\n\$5\r\nworld\r\n')
r = s.recv(1024)
assert r == b\"-ERR unknown command 'set'\r\n\", f'Expected error for SET, got {r!r}'

# GET (invalid in subscribe mode)
s.sendall(b'*2\r\n\$3\r\nGET\r\n\$5\r\nhello\r\n')
r = s.recv(1024)
assert r == b\"-ERR unknown command 'get'\r\n\", f'Expected error for GET, got {r!r}'

s.close()
print('ok')
"
}
run_test "error_after_subscribe" $((BASE_PORT + 5)) test_error_after_subscribe $((BASE_PORT + 5))

# ---------- Test 6: manage_subscription (subscribe + unsubscribe) ----------
test_manage_subscription() {
    local port=$1
    python3 -c "
import socket, time

pub = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
pub.connect(('127.0.0.1', $port))

sub = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sub.connect(('127.0.0.1', $port))

# Subscribe to hello
sub.sendall(b'*2\r\n\$9\r\nSUBSCRIBE\r\n\$5\r\nhello\r\n')
r = sub.recv(1024)
assert r == b'*3\r\n\$9\r\nsubscribe\r\n\$5\r\nhello\r\n:1\r\n', f'Sub hello failed: {r!r}'

# Subscribe to foo
sub.sendall(b'*2\r\n\$9\r\nSUBSCRIBE\r\n\$3\r\nfoo\r\n')
time.sleep(0.1)
r = sub.recv(1024)
assert r == b'*3\r\n\$9\r\nsubscribe\r\n\$3\r\nfoo\r\n:2\r\n', f'Sub foo failed: {r!r}'

# Unsubscribe from hello
sub.sendall(b'*2\r\n\$11\r\nUNSUBSCRIBE\r\n\$5\r\nhello\r\n')
time.sleep(0.1)
r = sub.recv(1024)
assert r == b'*3\r\n\$11\r\nunsubscribe\r\n\$5\r\nhello\r\n:1\r\n', f'Unsub hello failed: {r!r}'

# Publish to hello (should get 0 subscribers)
pub.sendall(b'*3\r\n\$7\r\nPUBLISH\r\n\$5\r\nhello\r\n\$5\r\nworld\r\n')
r = pub.recv(1024)
assert r == b':0\r\n', f'Expected 0 subs for hello, got {r!r}'

# Publish to foo (should get 1 subscriber)
pub.sendall(b'*3\r\n\$7\r\nPUBLISH\r\n\$3\r\nfoo\r\n\$3\r\nbar\r\n')
r = pub.recv(1024)
assert r == b':1\r\n', f'Expected 1 sub for foo, got {r!r}'

# Subscriber should receive the foo message
time.sleep(0.1)
r = sub.recv(1024)
assert r == b'*3\r\n\$7\r\nmessage\r\n\$3\r\nfoo\r\n\$3\r\nbar\r\n', f'Expected foo message, got {r!r}'

# Unsubscribe from all
sub.sendall(b'*1\r\n\$11\r\nunsubscribe\r\n')
time.sleep(0.1)
r = sub.recv(1024)
assert r == b'*3\r\n\$11\r\nunsubscribe\r\n\$3\r\nfoo\r\n:0\r\n', f'Unsub all failed: {r!r}'

pub.close()
sub.close()
print('ok')
"
}
run_test "manage_subscription" $((BASE_PORT + 6)) test_manage_subscription $((BASE_PORT + 6))

# ---------- Test 7: SET with EX (expiration) ----------
test_set_with_expiry() {
    local port=$1
    python3 -c "
import socket, time
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', $port))

# SET hello world EX 1
s.sendall(b'*5\r\n\$3\r\nSET\r\n\$5\r\nhello\r\n\$5\r\nworld\r\n+EX\r\n:1\r\n')
r = s.recv(1024)
assert r == b'+OK\r\n', f'Expected +OK, got {r!r}'

# GET hello (should exist)
s.sendall(b'*2\r\n\$3\r\nGET\r\n\$5\r\nhello\r\n')
r = s.recv(1024)
assert r == b'\$5\r\nworld\r\n', f'Expected world, got {r!r}'

# Wait for expiry
time.sleep(1.5)

# GET hello (should be expired)
s.sendall(b'*2\r\n\$3\r\nGET\r\n\$5\r\nhello\r\n')
r = s.recv(1024)
assert r == b'\$-1\r\n', f'Expected nil (expired), got {r!r}'

s.close()
print('ok')
"
}
run_test "set_with_expiry" $((BASE_PORT + 7)) test_set_with_expiry $((BASE_PORT + 7))

# ---------- Test 8: multiple pub/sub subscribers ----------
test_multiple_subscribers() {
    local port=$1
    python3 -c "
import socket, time

pub = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
pub.connect(('127.0.0.1', $port))

sub1 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sub1.connect(('127.0.0.1', $port))

sub2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sub2.connect(('127.0.0.1', $port))

# Sub1 subscribes to hello
sub1.sendall(b'*2\r\n\$9\r\nSUBSCRIBE\r\n\$5\r\nhello\r\n')
r = sub1.recv(1024)
assert b'subscribe' in r, f'Sub1 subscribe failed: {r!r}'

# Sub2 subscribes to hello and foo
sub2.sendall(b'*3\r\n\$9\r\nSUBSCRIBE\r\n\$5\r\nhello\r\n\$3\r\nfoo\r\n')
time.sleep(0.1)
r = sub2.recv(1024)
assert b'subscribe' in r, f'Sub2 subscribe failed: {r!r}'

# Publish to hello (2 subscribers)
time.sleep(0.1)
pub.sendall(b'*3\r\n\$7\r\nPUBLISH\r\n\$5\r\nhello\r\n\$5\r\njazzy\r\n')
r = pub.recv(1024)
assert r == b':2\r\n', f'Expected :2, got {r!r}'

# Publish to foo (1 subscriber)
pub.sendall(b'*3\r\n\$7\r\nPUBLISH\r\n\$3\r\nfoo\r\n\$3\r\nbar\r\n')
r = pub.recv(1024)
assert r == b':1\r\n', f'Expected :1, got {r!r}'

# Sub1 receives hello message
time.sleep(0.1)
r = sub1.recv(1024)
assert b'jazzy' in r, f'Sub1 should get jazzy: {r!r}'

# Sub2 receives both messages
time.sleep(0.1)
r = sub2.recv(4096)
assert b'jazzy' in r, f'Sub2 should get jazzy: {r!r}'
assert b'bar' in r, f'Sub2 should get bar: {r!r}'

pub.close()
sub1.close()
sub2.close()
print('ok')
"
}
run_test "multiple_subscribers" $((BASE_PORT + 8)) test_multiple_subscribers $((BASE_PORT + 8))

# ---------- Test 9: SET overwrite ----------
test_set_overwrite() {
    local port=$1
    python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', $port))

# SET key1 value1
s.sendall(b'*3\r\n\$3\r\nSET\r\n\$4\r\nkey1\r\n\$6\r\nvalue1\r\n')
r = s.recv(1024)
assert r == b'+OK\r\n', f'Expected +OK, got {r!r}'

# SET key1 value2 (overwrite)
s.sendall(b'*3\r\n\$3\r\nSET\r\n\$4\r\nkey1\r\n\$6\r\nvalue2\r\n')
r = s.recv(1024)
assert r == b'+OK\r\n', f'Expected +OK, got {r!r}'

# GET key1 (should be value2)
s.sendall(b'*2\r\n\$3\r\nGET\r\n\$4\r\nkey1\r\n')
r = s.recv(1024)
assert r == b'\$6\r\nvalue2\r\n', f'Expected value2, got {r!r}'

s.close()
print('ok')
"
}
run_test "set_overwrite" $((BASE_PORT + 9)) test_set_overwrite $((BASE_PORT + 9))

# ---------- Test 10: multiple commands on same connection ----------
test_pipelining() {
    local port=$1
    python3 -c "
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.connect(('127.0.0.1', $port))

# PING + SET + GET in sequence
s.sendall(b'*1\r\n\$4\r\nPING\r\n')
r = s.recv(1024)
assert r == b'+PONG\r\n', f'Expected PONG, got {r!r}'

s.sendall(b'*3\r\n\$3\r\nSET\r\n\$3\r\nabc\r\n\$3\r\nxyz\r\n')
r = s.recv(1024)
assert r == b'+OK\r\n', f'Expected +OK, got {r!r}'

s.sendall(b'*2\r\n\$3\r\nGET\r\n\$3\r\nabc\r\n')
r = s.recv(1024)
assert r == b'\$3\r\nxyz\r\n', f'Expected xyz, got {r!r}'

s.close()
print('ok')
"
}
run_test "pipelining" $((BASE_PORT + 10)) test_pipelining $((BASE_PORT + 10))

echo ""
echo "=== Results: $PASS/$TOTAL passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
