import concurrent.futures
import dataclasses
import os
import pathlib
import subprocess
import tempfile
import time
import unittest

from tests.fw_cpp_test_support import (
    CompileError,
    CppBuildConfig,
    get_or_compile_cpp,
    run_cpp,
)


class TestFwCppSupport(unittest.TestCase):
    def test_run_cpp_success_and_exit_code(self):
        source = """#include <iostream>

int main() {
    std::cout << "ok" << std::endl;
    return 7;
}
"""
        proc = run_cpp(source)
        self.assertIn("ok", proc.stdout)
        self.assertEqual(proc.returncode, 7)

    def test_run_cpp_syntax_error(self):
        source = """#include <iostream>

int main() {
    this is a syntax error!
    return 0;
}
"""
        with self.assertRaises(subprocess.CalledProcessError) as ctx:
            run_cpp(source)
        self.assertTrue(
            "syntax error" in ctx.exception.stderr or "error:" in ctx.exception.stderr or ctx.exception.returncode != 0
        )
        self.assertTrue(len(ctx.exception.stderr) > 0 or len(ctx.exception.stdout) > 0)
        # Verify CompileError exposes compiler stderr in str(e)
        self.assertIn("syntax error", str(ctx.exception))

    def test_run_cpp_timeout_expired(self):
        source = """int main() {
    while (true) {}
    return 0;
}
"""
        with self.assertRaises(subprocess.TimeoutExpired):
            run_cpp(source, timeout=0.2)

    def test_cache_hit_and_cold_warm_counts(self):
        with tempfile.TemporaryDirectory() as tmp_cache:
            cfg = CppBuildConfig(cache_dir=pathlib.Path(tmp_cache))
            source = """#include <iostream>
int main() {
    std::cout << "counter_test" << std::endl;
    return 0;
}
"""
            # Cold build
            with get_or_compile_cpp(source, config=cfg) as (bin_path1, from_cache1):
                self.assertFalse(from_cache1)
                self.assertTrue(bin_path1.exists())

            # Warm build
            with get_or_compile_cpp(source, config=cfg) as (bin_path2, from_cache2):
                self.assertTrue(from_cache2)
                self.assertEqual(bin_path1, bin_path2)

            # Execution produces expected output on both calls
            p1 = run_cpp(source, config=cfg)
            self.assertEqual(p1.returncode, 0)
            self.assertIn("counter_test", p1.stdout)

            p2 = run_cpp(source, config=cfg)
            self.assertEqual(p2.returncode, 0)
            self.assertIn("counter_test", p2.stdout)

    def test_source_invalidation(self):
        with tempfile.TemporaryDirectory() as tmp_cache:
            cfg = CppBuildConfig(cache_dir=pathlib.Path(tmp_cache))
            source_v1 = """#include <iostream>
int main() {
    std::cout << "v1" << std::endl;
    return 0;
}
"""
            source_v2 = """#include <iostream>
int main() {
    std::cout << "v2" << std::endl;
    return 0;
}
"""
            with get_or_compile_cpp(source_v1, config=cfg) as ((bin1, cached1)):
                pass
            self.assertFalse(cached1)

            with get_or_compile_cpp(source_v2, config=cfg) as ((bin2, cached2)):
                pass
            self.assertFalse(cached2)
            self.assertNotEqual(bin1, bin2)

            p1 = run_cpp(source_v1, config=cfg)
            p2 = run_cpp(source_v2, config=cfg)
            self.assertIn("v1", p1.stdout)
            self.assertIn("v2", p2.stdout)

    def test_header_invalidation(self):
        with tempfile.TemporaryDirectory() as tmp_dir, tempfile.TemporaryDirectory() as tmp_cache:
            inc_dir = pathlib.Path(tmp_dir) / "include"
            inc_dir.mkdir()
            custom_header = inc_dir / "magic.hpp"
            custom_header.write_text("#define MAGIC_VAL 10\n", encoding="utf-8")

            cfg = CppBuildConfig(
                cache_dir=pathlib.Path(tmp_cache),
                include_dirs=(str(inc_dir),),
            )

            source = """#include <iostream>
#include "magic.hpp"
int main() {
    std::cout << MAGIC_VAL << std::endl;
    return 0;
}
"""
            with get_or_compile_cpp(source, config=cfg) as ((bin1, cached1)):
                pass
            self.assertFalse(cached1)
            p1 = run_cpp(source, config=cfg)
            self.assertIn("10", p1.stdout)

            # Invalidate header
            time.sleep(0.01)  # Ensure nanosecond mtime delta
            custom_header.write_text("#define MAGIC_VAL 20\n", encoding="utf-8")

            with get_or_compile_cpp(source, config=cfg) as ((bin2, cached2)):
                pass
            self.assertFalse(cached2)
            self.assertNotEqual(bin1, bin2)
            p2 = run_cpp(source, config=cfg)
            self.assertIn("20", p2.stdout)

    def test_config_invalidation(self):
        with tempfile.TemporaryDirectory() as tmp_cache:
            cfg1 = CppBuildConfig(
                cache_dir=pathlib.Path(tmp_cache),
                flags=("-std=c++20", "-DVALUE=1"),
            )
            cfg2 = CppBuildConfig(
                cache_dir=pathlib.Path(tmp_cache),
                flags=("-std=c++20", "-DVALUE=2"),
            )
            source = """#include <iostream>
int main() {
    std::cout << VALUE << std::endl;
    return 0;
}
"""
            with get_or_compile_cpp(source, config=cfg1) as ((bin1, cached1)):
                pass
            self.assertFalse(cached1)
            p1 = run_cpp(source, config=cfg1)
            self.assertIn("1", p1.stdout)

            with get_or_compile_cpp(source, config=cfg2) as ((bin2, cached2)):
                pass
            self.assertFalse(cached2)
            self.assertNotEqual(bin1, bin2)
            p2 = run_cpp(source, config=cfg2)
            self.assertIn("2", p2.stdout)

    def test_external_header_bypasses_cache(self):
        with tempfile.TemporaryDirectory() as tmp_cache:
            cfg = CppBuildConfig(cache_dir=pathlib.Path(tmp_cache))
            source = """#include <iostream>
#include "/nonexistent/external_header.hpp"
int main() { return 0; }
"""
            # Should fail compilation and bypass cache without creating artifacts in cache_dir
            with self.assertRaises(subprocess.CalledProcessError):
                with get_or_compile_cpp(source, config=cfg) as _build:
                    pass

            cached_bins = list(pathlib.Path(tmp_cache).glob("bin_*"))
            self.assertEqual(len(cached_bins), 0)

    def test_never_cache_failures(self):
        with tempfile.TemporaryDirectory() as tmp_cache:
            cfg = CppBuildConfig(cache_dir=pathlib.Path(tmp_cache))
            bad_source = "int main() { invalid_token; }"

            with self.assertRaises(CompileError) as ctx:
                run_cpp(bad_source, config=cfg)
            self.assertIn("invalid_token", ctx.exception.stderr)
            self.assertIn("Compiler stderr:", str(ctx.exception))

            # Cache directory must contain no binary artifacts for failed compiles
            cached_bins = list(pathlib.Path(tmp_cache).glob("bin_*"))
            self.assertEqual(len(cached_bins), 0)

    def test_timeout_on_cached_binary(self):
        with tempfile.TemporaryDirectory() as tmp_cache:
            cfg = CppBuildConfig(cache_dir=pathlib.Path(tmp_cache))
            source = """#include <chrono>
#include <thread>
int main() {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return 0;
}
"""
            # Cold compile and execute with sufficient timeout
            p = run_cpp(source, timeout=5.0, config=cfg)
            self.assertEqual(p.returncode, 0)

            # Warm execution should hit timeout when timeout is short
            with self.assertRaises(subprocess.TimeoutExpired):
                run_cpp(source, timeout=0.1, config=cfg)

    def test_concurrent_same_key_safety(self):
        with tempfile.TemporaryDirectory() as tmp_cache:
            cfg = CppBuildConfig(cache_dir=pathlib.Path(tmp_cache))
            source = """#include <iostream>
int main() {
    std::cout << "parallel_safe" << std::endl;
    return 0;
}
"""
            num_workers = 6

            def worker_fn():
                with get_or_compile_cpp(source, config=cfg) as (binary, hit):
                    return binary, hit

            with concurrent.futures.ThreadPoolExecutor(max_workers=num_workers) as executor:
                futures = [executor.submit(worker_fn) for _ in range(num_workers)]
                results = [f.result() for f in futures]

            bins = [r[0] for r in results]
            from_caches = [r[1] for r in results]

            # All workers should get the exact same binary path
            self.assertEqual(len(set(bins)), 1)
            # Exactly one worker should have performed compilation (from_cache == False), others True
            self.assertEqual(from_caches.count(False), 1)
            self.assertEqual(from_caches.count(True), num_workers - 1)

            # Ensure run_cpp works on the resulting binary
            res = run_cpp(source, config=cfg)
            self.assertEqual(res.returncode, 0)
            self.assertIn("parallel_safe", res.stdout)

    def test_invalidation_macro_include(self):
        with tempfile.TemporaryDirectory() as root_str:
            root = pathlib.Path(root_str)
            inc = root / "macro"
            inc.mkdir()
            header = inc / "value.hpp"
            header.write_text("#define VALUE 11\n")
            cfg = CppBuildConfig(include_dirs=(str(inc),), cache_dir=root / "cache-macro")
            program = '#define CHOSEN "value.hpp"\n#include CHOSEN\nint main(){return VALUE;}\n'

            first = run_cpp(program, timeout=5, config=cfg)
            self.assertEqual(first.returncode, 11)

            header.write_text("#define VALUE 22\n")
            cached = run_cpp(program, timeout=5, config=cfg)
            fresh = run_cpp(program, timeout=5, config=dataclasses.replace(cfg, cache_dir=None))
            self.assertEqual(cached.returncode, 22)
            self.assertEqual(fresh.returncode, 22)

    def test_invalidation_unresolved_transitive(self):
        with tempfile.TemporaryDirectory() as root_str:
            root = pathlib.Path(root_str)
            inc = root / "transitive"
            inc.mkdir()
            external = root / "external"
            external.mkdir()
            (inc / "outer.hpp").write_text("#include <hidden.hpp>\n")
            header = external / "hidden.hpp"
            header.write_text("#define VALUE 11\n")
            cfg = CppBuildConfig(
                flags=("-std=c++20", f"-I{external}"),
                include_dirs=(str(inc),),
                cache_dir=root / "cache-transitive",
            )
            program = '#include "outer.hpp"\nint main(){return VALUE;}\n'

            first = run_cpp(program, timeout=5, config=cfg)
            self.assertEqual(first.returncode, 11)

            header.write_text("#define VALUE 22\n")
            cached = run_cpp(program, timeout=5, config=cfg)
            fresh = run_cpp(program, timeout=5, config=dataclasses.replace(cfg, cache_dir=None))
            self.assertEqual(cached.returncode, 22)
            self.assertEqual(fresh.returncode, 22)

    def test_invalidation_standard_header_shadow(self):
        with tempfile.TemporaryDirectory() as root_str:
            root = pathlib.Path(root_str)
            inc = root / "shadow"
            inc.mkdir()
            header = inc / "vector"
            header.write_text("#define VALUE 11\n")
            cfg = CppBuildConfig(include_dirs=(str(inc),), cache_dir=root / "cache-shadow")
            program = "#include <vector>\nint main(){return VALUE;}\n"

            first = run_cpp(program, timeout=5, config=cfg)
            self.assertEqual(first.returncode, 11)

            header.write_text("#define VALUE 22\n")
            cached = run_cpp(program, timeout=5, config=cfg)
            fresh = run_cpp(program, timeout=5, config=dataclasses.replace(cfg, cache_dir=None))
            self.assertEqual(cached.returncode, 22)
            self.assertEqual(fresh.returncode, 22)

    def test_invalidation_same_size_and_mtime(self):
        with tempfile.TemporaryDirectory() as root_str:
            root = pathlib.Path(root_str)
            inc = root / "stamp"
            inc.mkdir()
            header = inc / "value.hpp"
            header.write_text("#define VALUE 11\n")
            stamp = header.stat()

            cfg = CppBuildConfig(include_dirs=(str(inc),), cache_dir=root / "cache-stamp")
            program = '#include "value.hpp"\nint main(){return VALUE;}\n'

            first = run_cpp(program, timeout=5, config=cfg)
            self.assertEqual(first.returncode, 11)

            header.write_text("#define VALUE 22\n")
            os.utime(header, ns=(stamp.st_atime_ns, stamp.st_mtime_ns))

            cached = run_cpp(program, timeout=5, config=cfg)
            fresh = run_cpp(program, timeout=5, config=dataclasses.replace(cfg, cache_dir=None))
            self.assertEqual(cached.returncode, 22)
            self.assertEqual(fresh.returncode, 22)

    def test_invalidation_compiler_path_same_version(self):
        with tempfile.TemporaryDirectory() as root_str:
            root = pathlib.Path(root_str)
            compilers = []
            for value in [11, 22]:
                compiler = root / f"compiler-{value}"
                compiler.write_text(
                    f'#!/bin/sh\nif [ "$1" = "--version" ]; then echo same-version; exit 0; fi\nexec /usr/bin/c++ -DVALUE={value} "$@"\n'
                )
                compiler.chmod(0o755)
                compilers.append(compiler)

            first_config = CppBuildConfig(
                compiler=str(compilers[0]),
                include_dirs=(),
                cache_dir=root / "cache-compiler",
            )
            second_config = dataclasses.replace(first_config, compiler=str(compilers[1]))
            program = "int main(){return VALUE;}\n"

            first = run_cpp(program, timeout=5, config=first_config)
            self.assertEqual(first.returncode, 11)

            cached = run_cpp(program, timeout=5, config=second_config)
            fresh = run_cpp(
                program, timeout=5, config=dataclasses.replace(second_config, cache_dir=None)
            )
            self.assertEqual(cached.returncode, 22)
            self.assertEqual(fresh.returncode, 22)

    def test_invalidation_relative_leading_hyphen_header(self):
        with tempfile.TemporaryDirectory() as root_str:
            root = pathlib.Path(root_str)
            cwd = root / "cwd"
            cwd.mkdir()
            h = cwd / "-value.hpp"
            h.write_text("#define VALUE 11\n")
            previous = pathlib.Path.cwd()
            try:
                os.chdir(cwd)
                program = '#include "-value.hpp"\nint main(){return VALUE;}\n'
                cfg = CppBuildConfig(include_dirs=(".",), cache_dir=root / "cache-hyphen")

                first = run_cpp(program, timeout=5, config=cfg)
                self.assertEqual(first.returncode, 11)

                h.write_text("#define VALUE 22\n")
                cached = run_cpp(program, timeout=5, config=cfg)
                fresh = run_cpp(program, timeout=5, config=dataclasses.replace(cfg, cache_dir=None))
                self.assertEqual(cached.returncode, 22)
                self.assertEqual(fresh.returncode, 22)
            finally:
                os.chdir(previous)

    def test_invalidation_compiler_selected_via_PATH(self):
        with tempfile.TemporaryDirectory() as root_str:
            root = pathlib.Path(root_str)
            dirs = []
            for value in [11, 22]:
                folder = root / f"compiler{value}"
                folder.mkdir()
                wrapper = folder / "review-cxx"
                wrapper.write_text(
                    f'#!/bin/sh\nif [ "$1" = "--version" ]; then echo same-version; exit 0; fi\nexec /usr/bin/c++ -DVALUE={value} "$@"\n'
                )
                wrapper.chmod(0o755)
                dirs.append(folder)

            oldpath = os.environ.get("PATH", "")
            try:
                cfg = CppBuildConfig(
                    compiler="review-cxx",
                    include_dirs=(),
                    cache_dir=root / "cache-PATH",
                )
                program = "int main(){return VALUE;}\n"

                os.environ["PATH"] = str(dirs[0]) + os.pathsep + oldpath
                first = run_cpp(program, timeout=5, config=cfg)
                self.assertEqual(first.returncode, 11)

                os.environ["PATH"] = str(dirs[1]) + os.pathsep + oldpath
                cached = run_cpp(program, timeout=5, config=cfg)
                fresh = run_cpp(program, timeout=5, config=dataclasses.replace(cfg, cache_dir=None))
                self.assertEqual(cached.returncode, 22)
                self.assertEqual(fresh.returncode, 22)
            finally:
                os.environ["PATH"] = oldpath


    def test_bypass_artifacts_cleanup_across_public_run_outcomes(self):
        from unittest import mock
        with tempfile.TemporaryDirectory(prefix="cpp-owned-spool-") as directory:
            root = pathlib.Path(directory)
            spool = root / "spool"
            spool.mkdir()
            cfg = CppBuildConfig(cache_dir=None, include_dirs=())
            with mock.patch.object(tempfile, "tempdir", str(spool)):
                result = run_cpp("int main(){return 7;}", config=cfg)
                self.assertEqual(result.returncode, 7)
                self.assertEqual(list(spool.iterdir()), [])
                with self.assertRaises(CompileError):
                    run_cpp("int main(){invalid_token;}", config=cfg)
                self.assertEqual(list(spool.iterdir()), [])
                with self.assertRaises(subprocess.TimeoutExpired):
                    run_cpp("int main(){for(;;){}}", timeout=0.05, config=cfg)
                self.assertEqual(list(spool.iterdir()), [])
                compiler = root / "no-dependencies-cxx"
                compiler.write_text('#!/bin/sh\nfor arg do\n if [ "$arg" = "-M" ]; then exit 1; fi\ndone\nexec /usr/bin/c++ "$@"\n')
                compiler.chmod(0o755)
                bypass = dataclasses.replace(cfg, compiler=str(compiler), cache_dir=root / "cache")
                result = run_cpp("int main(){return 0;}", config=bypass)
                self.assertEqual(result.returncode, 0)
                self.assertEqual(list(spool.iterdir()), [])
                self.assertFalse((root / "cache").exists())

    def test_resolver_context_owns_bypass_and_preserves_cold_cache(self):
        with tempfile.TemporaryDirectory() as directory:
            cfg = CppBuildConfig(cache_dir=None, include_dirs=())
            with self.assertRaisesRegex(RuntimeError, "caller failed"):
                with get_or_compile_cpp("int main(){return 0;}", cfg) as (binary, hit):
                    self.assertFalse(hit)
                    self.assertTrue(binary.exists())
                    owned_directory = binary.parent
                    raise RuntimeError("caller failed")
            self.assertFalse(owned_directory.exists())
            cached_cfg = dataclasses.replace(cfg, cache_dir=pathlib.Path(directory) / "cache")
            with get_or_compile_cpp("int main(){return 0;}", cached_cfg) as (cold, hit):
                self.assertFalse(hit)
                self.assertTrue(cold.exists())
            self.assertTrue(cold.exists())
            with get_or_compile_cpp("int main(){return 0;}", cached_cfg) as (warm, hit):
                self.assertTrue(hit)
                self.assertEqual(warm, cold)

    def test_same_key_compilation_across_real_processes(self):
        import json
        import sys
        with tempfile.TemporaryDirectory(prefix="cpp-real-processes-") as directory:
            root = pathlib.Path(directory)
            compiler = root / "counting-cxx"
            compile_log = root / "compiles"
            compiler.write_text(
                '#!/usr/bin/python3\nimport os,sys,time\n'
                'if "--version" not in sys.argv and "-M" not in sys.argv:\n'
                ' with open(' + repr(str(compile_log)) + ',"a") as f:f.write("compile\\n")\n'
                ' time.sleep(.15)\n'
                'os.execv("/usr/bin/c++",["c++",*sys.argv[1:]])\n'
            )
            compiler.chmod(0o755)
            worker = root / "worker.py"
            worker.write_text(
                'import importlib.util,json,os,pathlib,subprocess,sys,time\n'
                'spec=importlib.util.spec_from_file_location("owned",sys.argv[1]);m=importlib.util.module_from_spec(spec);sys.modules[spec.name]=m;spec.loader.exec_module(m)\n'
                'pathlib.Path(sys.argv[5]).write_text("ready")\n'
                'deadline=time.monotonic()+5\n'
                'while not pathlib.Path(sys.argv[4]).exists():\n'
                ' if time.monotonic()>=deadline:raise TimeoutError("start barrier")\n'
                ' time.sleep(.005)\n'
                'cfg=m.CppBuildConfig(compiler=sys.argv[3],cache_dir=pathlib.Path(sys.argv[2]),include_dirs=(),lock_timeout=3)\n'
                'with m.get_or_compile_cpp("int main(){return 0;}",cfg) as (binary,hit):\n'
                ' p=subprocess.run([str(binary)],timeout=2)\n'
                ' print(json.dumps({"pid":os.getpid(),"hit":hit,"returncode":p.returncode}),flush=True)\n'
            )
            gate = root / "go"
            cache = root / "cache"
            helper = pathlib.Path(__file__).with_name("fw_cpp_test_support.py")
            peers = []
            try:
                for index in range(4):
                    peers.append(subprocess.Popen(
                        [sys.executable, str(worker), str(helper), str(cache), str(compiler), str(gate), str(root / f"ready{index}")],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                    ))
                deadline = time.monotonic() + 5
                while len(list(root.glob("ready*"))) < 4:
                    if time.monotonic() >= deadline:
                        self.fail("child readiness deadline expired")
                    time.sleep(.005)
                gate.write_text("go")
                observations = []
                for peer in peers:
                    out, err = peer.communicate(timeout=8)
                    self.assertEqual(peer.returncode, 0, out + err)
                    observations.append(json.loads(out))
                self.assertEqual(len({item["pid"] for item in observations}), 4)
                self.assertEqual(sum(not item["hit"] for item in observations), 1)
                self.assertTrue(all(item["returncode"] == 0 for item in observations))
                self.assertEqual(compile_log.read_text().splitlines(), ["compile"])
                self.assertEqual(len(list(cache.glob("bin_*"))), 1)
            finally:
                for peer in peers:
                    if peer.poll() is None:
                        peer.kill()
                    peer.communicate(timeout=3)


if __name__ == "__main__":
    unittest.main()
