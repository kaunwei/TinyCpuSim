"""NightShift custom acceptance runner for TinyCpuSim.

Integrates with scripts/nightshift_acceptance.py and acceptance.DefaultAcceptanceRunner.
Supports both explicit tdd_cases and automated test execution gates.
"""

from pathlib import Path
import sys

# Ensure project root is in python search path
PROJECT_ROOT = Path(__file__).resolve().parent.parent
if str(PROJECT_ROOT) not in sys.path:
    sys.path.insert(0, str(PROJECT_ROOT))

import acceptance


class TinyCpuSimRunner(acceptance.DefaultAcceptanceRunner):
    """Acceptance runner handling CTest/GTest and Python fixtures in TinyCpuSim."""

    def run_hot(self, args, result, run_verify):
        # If tdd_cases are declared by the agent adapter, enforce strict TDD AST & evidence checking
        if result.get("tdd_cases"):
            return super().run_hot(args, result, run_verify)

        # Fallback when no tdd_cases are declared (e.g. direct trial, maintenance or C++ task)
        work_dir = Path(args.work_dir)
        cfg = acceptance.config(work_dir)
        hot_cmd = acceptance.command(cfg, "hot")
        code, output = run_verify(hot_cmd, str(work_dir), args.worker_port, cfg.get("timeout", 180))
        if code:
            raise acceptance.GateError(f"hot acceptance exited {code}\n{output[-16384:]}")

        evidence = [{"phase": "hot", "command": hot_cmd, "exit": 0, "output": output[-16384:]}]

        if args.verify and args.verify.lower() not in ("none", "report_only"):
            v_code, v_out = run_verify(args.verify, str(work_dir), args.worker_port, cfg.get("timeout", 180))
            evidence.append({"phase": "task", "command": args.verify, "exit": v_code, "output": v_out[-16384:]})
            if v_code:
                raise acceptance.GateError(f"task VERIFY exited {v_code}\n{v_out[-16384:]}")

        return {"cases": [], "evidence": evidence}

    def run_cold(self, root, run_verify):
        return super().run_cold(root, run_verify)


def get_runner(root, cfg):
    return TinyCpuSimRunner()
