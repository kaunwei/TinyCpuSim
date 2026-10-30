#!/usr/bin/env python3
"""Interactive UART Terminal & PTY Bridge for TinyCpuSim Firmware."""

import os
import sys
import time
import fcntl
import signal
import pathlib
import select
import subprocess

def run_interactive_uart(sim_bin: str, elf_path: str, banner_path: str = None, pty_only: bool = False, max_steps: str = None):
    project_root = pathlib.Path(__file__).resolve().parent.parent
    scratch_dir = project_root / "nightshift" / ".scratch"
    scratch_dir.mkdir(parents=True, exist_ok=True)
    pty_file = scratch_dir / f"fw_pty_{os.getpid()}.txt"
    if pty_file.exists():
        pty_file.unlink()

    sim_cmd = [sim_bin, "--firmware", "--uart-pty", "--pty-file", str(pty_file)]
    if max_steps:
        sim_cmd.extend(["--max-steps", str(max_steps)])
    sim_cmd.append(elf_path)

    proc = subprocess.Popen(sim_cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

    def handle_signal(signum, frame):
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=1.0)
            except Exception:
                proc.kill()
        if pty_file.exists():
            pty_file.unlink()
        sys.exit(128 + signum)

    signal.signal(signal.SIGTERM, handle_signal)
    signal.signal(signal.SIGINT, handle_signal)

    slave_path = None
    try:
        # Wait up to 3 seconds for PTY slave path
        deadline = time.time() + 3.0
        while time.time() < deadline:
            if pty_file.exists():
                content = pty_file.read_text().strip()
                if content and os.path.exists(content):
                    slave_path = content
                    break
            if proc.poll() is not None:
                break
            time.sleep(0.05)

        if not slave_path:
            print("[Error] Failed to initialize PTY device for simulator.", file=sys.stderr)
            proc.terminate()
            return 1

        # Announce PTY slave on stderr for automated tools & tests
        print(f"[tinyarmsim] PTY slave: {slave_path}", file=sys.stderr, flush=True)

        print("============================================================")
        print("  Firmware UART Terminal Ready")
        print(f"  PTY Device: {slave_path}")
        print("  To connect from another terminal:")
        print(f"    picocom -b 115200 {slave_path}")
        print(f"    screen {slave_path}")
        if not pty_only:
            print("  (Interactive input enabled in this terminal)")
        print("============================================================")

        if banner_path and os.path.exists(banner_path):
            try:
                banner_text = pathlib.Path(banner_path).read_text().strip()
                if banner_text:
                    print(banner_text)
                    print("------------------------------------------------------------")
            except Exception:
                pass

        if pty_only:
            print("Waiting for external serial terminal connection (Ctrl+C to stop)...")
            return proc.wait()

        # Attach interactive terminal (supports both TTY and piped stdin)
        slave_fd = os.open(slave_path, os.O_RDWR | os.O_NOCTTY)
        flags = fcntl.fcntl(slave_fd, fcntl.F_GETFL)
        fcntl.fcntl(slave_fd, fcntl.F_SETFL, flags | os.O_NONBLOCK)

        stdin_fd = sys.stdin.fileno()
        is_tty = sys.stdin.isatty()
        old_term_settings = None

        if is_tty:
            import termios
            import tty
            old_term_settings = termios.tcgetattr(stdin_fd)
            tty.setraw(stdin_fd)

        try:
            stdin_closed = False
            while proc.poll() is None:
                inputs = [slave_fd]
                if not stdin_closed:
                    inputs.append(stdin_fd)
                rlist, _, _ = select.select(inputs, [], [], 0.05)
                if stdin_fd in rlist:
                    data = os.read(stdin_fd, 256)
                    if not data:
                        stdin_closed = True
                    else:
                        if is_tty and b'\x03' in data:  # Ctrl+C
                            proc.send_signal(signal.SIGINT)
                            break
                        os.write(slave_fd, data)
                if slave_fd in rlist:
                    try:
                        out = os.read(slave_fd, 1024)
                        if out:
                            sys.stdout.buffer.write(out)
                            sys.stdout.buffer.flush()
                        else:
                            break
                    except (BlockingIOError, InterruptedError):
                        pass
        finally:
            if is_tty and old_term_settings:
                import termios
                termios.tcsetattr(stdin_fd, termios.TCSADRAIN, old_term_settings)
            try:
                os.close(slave_fd)
            except Exception:
                pass

        return proc.wait()
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=1.0)
            except subprocess.TimeoutExpired:
                proc.kill()
        if pty_file.exists():
            pty_file.unlink()

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <sim_bin> <elf_path> [banner_path] [--pty-only] [--max-steps N]")
        sys.exit(1)

    sim = sys.argv[1]
    elf = sys.argv[2]
    banner = None
    pty_only = False
    max_steps = None

    idx = 3
    while idx < len(sys.argv):
        arg = sys.argv[idx]
        if arg == "--pty-only":
            pty_only = True
        elif arg == "--max-steps" and idx + 1 < len(sys.argv):
            idx += 1
            max_steps = sys.argv[idx]
        elif not banner and not arg.startswith("-"):
            banner = arg
        idx += 1

    sys.exit(run_interactive_uart(sim, elf, banner, pty_only, max_steps))
