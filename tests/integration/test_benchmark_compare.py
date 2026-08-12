from __future__ import annotations

import copy
import hashlib
import json
import math
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

from benchmark_compare import (  # noqa: E402
    baseline_eligible,
    compare_documents,
    strict_json_loads,
    validate_benchmark_document,
)


def artifact(median: float = 1.0, mad: float = 0.01) -> dict[str, object]:
    instrumentation = {
        "preset": "benchmark",
        "optimized": True,
        "vulkan_validation": False,
        "synchronization_validation": False,
        "address_sanitizer": False,
        "undefined_behavior_sanitizer": False,
        "allocation_tracking": True,
    }
    comparison_key = {"suite": "synthetic", "cpu": "same", "instrumentation": instrumentation}
    fingerprint_hash = hashlib.sha256(
        json.dumps(comparison_key, sort_keys=True, separators=(",", ":")).encode("utf-8")
    ).hexdigest()
    return {
        "schema_version": 1,
        "command": "benchmark",
        "status": "pass",
        "diagnostics": [],
        "fingerprint": {
            "hash": fingerprint_hash,
            "comparison_key": comparison_key,
            "instrumentation": instrumentation,
            "eligible_for_baseline": True,
        },
        "metrics": {
            "suite": "synthetic",
            "results": [
                {
                    "benchmark_id": "synthetic_v1",
                    "entity_count": 1000,
                    "status": "pass",
                    "frame_cpu_ms": {
                        "sample_count": 100,
                        "median": median,
                        "p95": median * 1.1,
                        "p99": median * 1.2,
                        "mad": mad,
                    },
                    "memory": {
                        "tracked_cpp_heap_allocations": 0,
                        "capacity_growth_events": 0,
                        "frame_arena_overflows": 0,
                    },
                }
            ],
        },
    }


def main() -> int:
    baseline = artifact()
    assert compare_documents(copy.deepcopy(baseline), baseline).status == "pass"
    assert compare_documents(artifact(1.05), baseline).status == "pass"

    warning = compare_documents(artifact(1.10), baseline)
    assert warning.status == "warn"
    assert any(item["code"] == "PERF_REGRESSION" for item in warning.diagnostics)

    failure = compare_documents(artifact(1.20), baseline)
    assert failure.status == "fail"
    assert any(item["severity"] == "error" for item in failure.diagnostics)

    noisy = compare_documents(artifact(1.20), artifact(1.0, 0.20))
    assert noisy.status == "warn"
    assert any(item["code"] == "PERF_NOISE_TOO_HIGH" for item in noisy.diagnostics)

    incompatible = artifact()
    incompatible["fingerprint"]["comparison_key"]["cpu"] = "different"  # type: ignore[index]
    result = compare_documents(incompatible, baseline)
    assert result.status == "incomparable"
    assert result.diagnostics[0]["code"] == "PERF_BASELINE_INCOMPARABLE"

    allocation = artifact()
    allocation["metrics"]["results"][0]["memory"]["tracked_cpp_heap_allocations"] = 1  # type: ignore[index]
    result = compare_documents(allocation, baseline)
    assert result.status == "fail"
    assert any(item["code"] == "MEM_FRAME_HEAP_ALLOCATION" for item in result.diagnostics)

    adversarial: list[dict[str, object]] = []
    empty_results = artifact()
    empty_results["metrics"]["results"] = []  # type: ignore[index]
    adversarial.append(empty_results)

    duplicate_result = artifact()
    duplicate_result["metrics"]["results"].append(  # type: ignore[index]
        copy.deepcopy(duplicate_result["metrics"]["results"][0])  # type: ignore[index]
    )
    adversarial.append(duplicate_result)

    missing_memory = artifact()
    del missing_memory["metrics"]["results"][0]["memory"]  # type: ignore[index]
    adversarial.append(missing_memory)

    missing_distribution = artifact()
    del missing_distribution["metrics"]["results"][0]["frame_cpu_ms"]  # type: ignore[index]
    adversarial.append(missing_distribution)

    zero_samples = artifact()
    zero_samples["metrics"]["results"][0]["frame_cpu_ms"]["sample_count"] = 0  # type: ignore[index]
    adversarial.append(zero_samples)

    non_finite = artifact()
    non_finite["metrics"]["results"][0]["frame_cpu_ms"]["median"] = math.nan  # type: ignore[index]
    adversarial.append(non_finite)

    child = artifact()
    duplicate_suite = {
        "schema_version": 1,
        "command": "benchmark",
        "status": "pass",
        "diagnostics": [],
        "metrics": {"suite": "all", "suites": [child, copy.deepcopy(child)]},
    }
    adversarial.append(duplicate_suite)

    top_level_lie = artifact()
    top_level_lie["metrics"]["results"][0]["status"] = "fail"  # type: ignore[index]
    adversarial.append(top_level_lie)

    for malformed in adversarial:
        assert validate_benchmark_document(malformed)
        assert compare_documents(malformed, baseline).status == "incomparable"

    forged_eligibility = artifact()
    forged_eligibility["fingerprint"]["instrumentation"]["preset"] = "dev"  # type: ignore[index]
    assert not baseline_eligible(forged_eligibility)
    assert baseline_eligible(baseline)

    try:
        strict_json_loads('{"value":NaN}')
    except ValueError:
        pass
    else:
        raise AssertionError("strict JSON parser accepted NaN")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
