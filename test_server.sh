#!/bin/bash
# ToastStunt test server management script

set -e

TEST_DIR="/root/src/toaststunt/test"
SERVER_PID_FILE="$TEST_DIR/.server.pid"
DEFAULT_PORT=9898

# Function to show usage
usage() {
    echo "Usage: $0 {start|stop|restart|status|test} [port]"
    echo "Commands:"
    echo "  start   - Start test server (default port: $DEFAULT_PORT)"
    echo "  stop    - Stop test server"
    echo "  restart - Restart test server"  
    echo "  status  - Check server status"
    echo "  test    - Run AST tests (starts server if needed)"
    echo "  logs    - Show recent server logs"
    exit 1
}

# Function to start server
start_server() {
    local port=${1:-$DEFAULT_PORT}
    
    if [ -f "$SERVER_PID_FILE" ] && kill -0 "$(cat $SERVER_PID_FILE)" 2>/dev/null; then
        echo "Server already running (PID: $(cat $SERVER_PID_FILE))"
        return 0
    fi
    
    echo "Starting ToastStunt test server on port $port..."
    cd "$TEST_DIR"
    
    # Log binary hash for verification
    local binary_hash=$(sha256sum ./moo | cut -d' ' -f1)
    echo "Binary hash: $binary_hash" > server.log
    echo "Start time: $(date)" >> server.log
    echo "Port: $port" >> server.log
    echo "==================" >> server.log
    
    nohup ./moo Test.db /dev/null $port >> server.log 2>&1 &
    local server_pid=$!
    echo $server_pid > "$SERVER_PID_FILE"
    
    # Wait a moment and check if server started successfully
    sleep 2
    if kill -0 $server_pid 2>/dev/null; then
        echo "Server started successfully (PID: $server_pid)"
        echo "Logs: tail -f $TEST_DIR/server.log"
    else
        echo "Failed to start server"
        rm -f "$SERVER_PID_FILE"
        return 1
    fi
}

# Function to stop server
stop_server() {
    if [ -f "$SERVER_PID_FILE" ]; then
        local pid=$(cat "$SERVER_PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            echo "Stopping server (PID: $pid)..."
            kill "$pid"
            rm -f "$SERVER_PID_FILE"
            echo "Server stopped"
        else
            echo "Server not running (stale PID file)"
            rm -f "$SERVER_PID_FILE"
        fi
    else
        echo "Server not running"
    fi
}

# Function to check server status
check_status() {
    if [ -f "$SERVER_PID_FILE" ]; then
        local pid=$(cat "$SERVER_PID_FILE")
        if kill -0 "$pid" 2>/dev/null; then
            echo "Server running (PID: $pid)"
            return 0
        else
            echo "Server not running (stale PID file)"
            rm -f "$SERVER_PID_FILE"
            return 1
        fi
    else
        echo "Server not running"
        return 1
    fi
}

# Function to run tests
run_tests() {
    echo "Ensuring server is running..."
    if ! check_status > /dev/null 2>&1; then
        start_server
        sleep 3  # Give server time to fully start
    fi
    
    echo "Running AST tests..."
    cd "$TEST_DIR"
    PATH="$HOME/.rbenv/bin:$PATH" make test_ast
}

# Function to show logs
show_logs() {
    if [ -f "$TEST_DIR/server.log" ]; then
        echo "=== Recent server logs ==="
        tail -n 50 "$TEST_DIR/server.log"
    else
        echo "No server log file found"
    fi
}

# Main script logic
case "${1:-}" in
    start)
        start_server "$2"
        ;;
    stop)
        stop_server
        ;;
    restart)
        stop_server
        sleep 1
        start_server "$2"
        ;;
    status)
        check_status
        ;;
    test)
        run_tests
        ;;
    logs)
        show_logs
        ;;
    *)
        usage
        ;;
esac