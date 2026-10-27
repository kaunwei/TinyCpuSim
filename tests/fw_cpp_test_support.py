import pathlib
import subprocess
import tempfile


_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent


def run_cpp(source: str, timeout: float = 5) -> subprocess.CompletedProcess[str]:
    """Compile C++20 source and run the resulting binary under a timeout.

    Writes source and binary inside a TemporaryDirectory, compiles with
    /usr/bin/c++ -std=c++20 -I<repo>/include (compiler timeout 30, check=True,
    capture_output=True, text=True), then runs the executable under timeout
    with captured text and real exit code. Automatic cleanup is guaranteed.
    """
    include_dir = str(_REPO_ROOT / "include")
    with tempfile.TemporaryDirectory() as tmpdir:
        src_file = pathlib.Path(tmpdir) / "main.cpp"
        bin_file = pathlib.Path(tmpdir) / "app"

        src_file.write_text(source, encoding="utf-8")

        subprocess.run(
            [
                "/usr/bin/c++",
                "-std=c++20",
                f"-I{include_dir}",
                str(src_file),
                "-o",
                str(bin_file),
            ],
            capture_output=True,
            text=True,
            timeout=30,
            check=True,
        )

        return subprocess.run(
            [str(bin_file)],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
