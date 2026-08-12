"""Pure benchmark artifact comparison used by engine.py and synthetic contract tests."""

from __future__ import annotations

import hashlib
import json
import math
from dataclasses import dataclass
from typing import Any, Iterable

WARN_RELATIVE = 0.075
FAIL_RELATIVE = 0.15
CPU_ABSOLUTE_FLOOR_MS = 0.01
GPU_ABSOLUTE_FLOOR_MS = 0.005

REQUIRED_DISTRIBUTIONS = {
    "world": {"query_transform_sprite_ms", "integrate_velocity_ms", "wrap_bounds_ms"},
    "render": {"update_ms", "queue_build_ms", "upload_ms", "cpu_submit_ms", "gpu_pass_ms"},
    "scenario": {
        "frame_cpu_ms",
        "fixed_update_ms",
        "render_extraction_ms",
        "render_queue_build_ms",
        "render_upload_ms",
        "render_submission_cpu_ms",
        "sprite_pass_gpu_ms",
    },
}


@dataclass(frozen=True)
class ComparisonOutcome:
    status: str
    diagnostics: list[dict[str, Any]]
    metrics: list[dict[str, Any]]


def strict_json_loads(source: str) -> Any:
    def reject_constant(value: str) -> None:
        raise ValueError(f"non-finite JSON constant is forbidden: {value}")

    return json.loads(source, parse_constant=reject_constant)


def _is_integer(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _finite_number(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value))


def _fingerprint_errors(fingerprint: Any, path: str) -> list[str]:
    if not isinstance(fingerprint, dict):
        return [f"{path}.fingerprint must be an object"]
    comparison_key = fingerprint.get("comparison_key")
    if not isinstance(comparison_key, dict):
        return [f"{path}.fingerprint.comparison_key must be an object"]
    fingerprint_hash = fingerprint.get("hash")
    if not isinstance(fingerprint_hash, str) or len(fingerprint_hash) != 64:
        return [f"{path}.fingerprint.hash must be a 64-character SHA-256"]
    expected_hash = hashlib.sha256(
        json.dumps(comparison_key, sort_keys=True, separators=(",", ":"), allow_nan=False).encode("utf-8")
    ).hexdigest()
    if fingerprint_hash != expected_hash:
        return [f"{path}.fingerprint.hash does not match comparison_key"]
    return []


def _validate_suite(document: Any, path: str) -> list[str]:
    errors: list[str] = []
    if not isinstance(document, dict):
        return [f"{path} must be an object"]
    if document.get("schema_version") != 1 or document.get("command") != "benchmark":
        errors.append(f"{path} must be a benchmark schema_version 1 document")
    if document.get("status") not in {"pass", "warn", "fail"}:
        errors.append(f"{path}.status must be pass, warn, or fail")
    if not isinstance(document.get("diagnostics"), list):
        errors.append(f"{path}.diagnostics must be an array")
    metrics = document.get("metrics")
    if not isinstance(metrics, dict):
        return errors + [f"{path}.metrics must be an object"]
    suite_name = metrics.get("suite")
    if not isinstance(suite_name, str) or not suite_name or suite_name == "all":
        return errors + [f"{path}.metrics.suite must name one concrete suite"]
    errors.extend(_fingerprint_errors(document.get("fingerprint"), path))
    results = metrics.get("results")
    if not isinstance(results, list) or not results:
        return errors + [f"{path}.metrics.results must be a nonempty array"]

    identities: set[tuple[str, int]] = set()
    for index, result in enumerate(results):
        result_path = f"{path}.metrics.results[{index}]"
        if not isinstance(result, dict):
            errors.append(f"{result_path} must be an object")
            continue
        benchmark_id = result.get("benchmark_id")
        count = result.get("entity_count", result.get("sprite_count"))
        if not isinstance(benchmark_id, str) or not benchmark_id or not _is_integer(count) or count <= 0:
            errors.append(f"{result_path} has an invalid benchmark identity")
        else:
            identity = (benchmark_id, count)
            if identity in identities:
                errors.append(f"{result_path} duplicates benchmark identity {identity}")
            identities.add(identity)
        if result.get("status") not in {"pass", "fail"}:
            errors.append(f"{result_path}.status must be pass or fail")

        memory = result.get("memory")
        if not isinstance(memory, dict):
            errors.append(f"{result_path}.memory must be an object")
        else:
            for field in ("tracked_cpp_heap_allocations", "capacity_growth_events"):
                value = memory.get(field)
                if not _is_integer(value) or value < 0:
                    errors.append(f"{result_path}.memory.{field} must be a nonnegative integer")
            arena_overflows = memory.get("frame_arena_overflows")
            if arena_overflows is None:
                reason = memory.get("frame_arena_overflows_unavailable_reason")
                if not isinstance(reason, str) or not reason:
                    errors.append(
                        f"{result_path}.memory must explain unavailable frame_arena_overflows"
                    )
            elif not _is_integer(arena_overflows) or arena_overflows < 0:
                errors.append(f"{result_path}.memory.frame_arena_overflows must be nonnegative or null")

        distributions: set[str] = set()
        for name, value in result.items():
            if not isinstance(value, dict) or not ({"sample_count", "median", "p95", "mad"} & value.keys()):
                continue
            distributions.add(name)
            required_fields = ("sample_count", "median", "p95", "mad")
            if not all(field in value for field in required_fields):
                errors.append(f"{result_path}.{name} is an incomplete distribution")
                continue
            if not _is_integer(value["sample_count"]) or value["sample_count"] <= 0:
                errors.append(f"{result_path}.{name}.sample_count must be positive")
            for field in ("median", "p95", "mad"):
                if not _finite_number(value[field]) or float(value[field]) < 0.0:
                    errors.append(f"{result_path}.{name}.{field} must be finite and nonnegative")
        required_distributions = REQUIRED_DISTRIBUTIONS.get(suite_name)
        if required_distributions is not None:
            missing = sorted(required_distributions - distributions)
            if missing:
                errors.append(f"{result_path} is missing required distributions: {missing}")
        elif not distributions:
            errors.append(f"{result_path} must contain at least one timing distribution")

        if suite_name == "scenario" or "scenario_hash" in result or "plan_hash" in result:
            if not _is_integer(result.get("scenario_hash")) or not _is_integer(result.get("plan_hash")):
                errors.append(f"{result_path} must contain integer scenario_hash and plan_hash")
    if document.get("status") == "pass" and any(
        isinstance(result, dict) and result.get("status") != "pass" for result in results
    ):
        errors.append(f"{path} cannot be pass when a child result failed")
    return errors


def validate_benchmark_document(document: Any) -> list[str]:
    if not isinstance(document, dict):
        return ["artifact must be an object"]
    metrics = document.get("metrics")
    if not isinstance(metrics, dict):
        return ["artifact.metrics must be an object"]
    if metrics.get("suite") != "all":
        return _validate_suite(document, "artifact")
    errors: list[str] = []
    if document.get("schema_version") != 1 or document.get("command") != "benchmark":
        errors.append("artifact must be a benchmark schema_version 1 document")
    if document.get("status") not in {"pass", "warn", "fail"}:
        errors.append("artifact.status must be pass, warn, or fail")
    suites = metrics.get("suites")
    if not isinstance(suites, list) or not suites:
        return errors + ["artifact.metrics.suites must be a nonempty array"]
    names: set[str] = set()
    for index, suite in enumerate(suites):
        errors.extend(_validate_suite(suite, f"artifact.metrics.suites[{index}]"))
        name = suite.get("metrics", {}).get("suite") if isinstance(suite, dict) else None
        if isinstance(name, str):
            if name in names:
                errors.append(f"artifact.metrics.suites[{index}] duplicates suite {name}")
            names.add(name)
    if document.get("status") == "pass" and any(
        not isinstance(suite, dict) or suite.get("status") != "pass" for suite in suites
    ):
        errors.append("artifact cannot be pass when a child suite failed")
    return errors


def baseline_eligible(document: Any) -> bool:
    if validate_benchmark_document(document) or not isinstance(document, dict) or document.get("status") != "pass":
        return False
    suites = _suite_documents(document)
    for suite in suites:
        fingerprint = suite.get("fingerprint", {})
        instrumentation = fingerprint.get("instrumentation")
        comparison_instrumentation = fingerprint.get("comparison_key", {}).get("instrumentation")
        if not isinstance(instrumentation, dict) or instrumentation != comparison_instrumentation:
            return False
        if (
            instrumentation.get("preset") != "benchmark"
            or instrumentation.get("optimized") is not True
            or instrumentation.get("vulkan_validation") is not False
            or instrumentation.get("synchronization_validation") is not False
            or instrumentation.get("address_sanitizer") is not False
            or instrumentation.get("undefined_behavior_sanitizer") is not False
            or instrumentation.get("allocation_tracking") is not True
        ):
            return False
    return True


def _diagnostic(
    code: str,
    severity: str,
    message: str,
    context: dict[str, Any],
) -> dict[str, Any]:
    return {
        "code": code,
        "severity": severity,
        "subsystem": "benchmark",
        "message": message,
        "context": context,
        "suggestions": [],
        "source": None,
    }


def _suite_documents(document: dict[str, Any]) -> list[dict[str, Any]]:
    metrics = document.get("metrics", {})
    if metrics.get("suite") == "all":
        suites = metrics.get("suites", [])
        return [item for item in suites if isinstance(item, dict)]
    return [document]


def _suite_map(document: dict[str, Any]) -> dict[str, dict[str, Any]]:
    result: dict[str, dict[str, Any]] = {}
    for suite in _suite_documents(document):
        name = str(suite.get("metrics", {}).get("suite", ""))
        if name:
            result[name] = suite
    return result


def _result_key(result: dict[str, Any]) -> tuple[str, int]:
    count = result.get("entity_count", result.get("sprite_count", 0))
    return str(result.get("benchmark_id", "")), int(count)


def _distribution_items(result: dict[str, Any]) -> Iterable[tuple[str, dict[str, Any]]]:
    for name, value in result.items():
        if isinstance(value, dict) and {"median", "p95", "mad", "sample_count"}.issubset(value):
            yield name, value


def _memory_violation(result: dict[str, Any]) -> tuple[str, int] | None:
    memory = result.get("memory", {})
    if not isinstance(memory, dict):
        return None
    for field in ("tracked_cpp_heap_allocations", "frame_arena_overflows", "capacity_growth_events"):
        raw_value = memory.get(field)
        if raw_value is None:
            continue
        value = int(raw_value)
        if value != 0:
            return field, value
    return None


def compare_documents(current: dict[str, Any], baseline: dict[str, Any]) -> ComparisonOutcome:
    diagnostics: list[dict[str, Any]] = []
    comparisons: list[dict[str, Any]] = []
    current_errors = validate_benchmark_document(current)
    baseline_errors = validate_benchmark_document(baseline)
    if current_errors or baseline_errors:
        diagnostics.append(
            _diagnostic(
                "PERF_BASELINE_INCOMPARABLE",
                "error",
                "Current or baseline benchmark artifact failed strict validation",
                {"current_errors": current_errors, "baseline_errors": baseline_errors},
            )
        )
        return ComparisonOutcome("incomparable", diagnostics, comparisons)
    current_suites = _suite_map(current)
    baseline_suites = _suite_map(baseline)
    if current_suites.keys() != baseline_suites.keys():
        diagnostics.append(
            _diagnostic(
                "PERF_BASELINE_INCOMPARABLE",
                "error",
                "Current and baseline artifacts contain different benchmark suites",
                {"current_suites": sorted(current_suites), "baseline_suites": sorted(baseline_suites)},
            )
        )
        return ComparisonOutcome("incomparable", diagnostics, comparisons)

    overall = "pass"
    for suite_name in sorted(current_suites):
        current_suite = current_suites[suite_name]
        baseline_suite = baseline_suites[suite_name]
        current_key = current_suite.get("fingerprint", {}).get("comparison_key")
        baseline_key = baseline_suite.get("fingerprint", {}).get("comparison_key")
        if not isinstance(current_key, dict) or current_key != baseline_key:
            diagnostics.append(
                _diagnostic(
                    "PERF_BASELINE_INCOMPARABLE",
                    "error",
                    "Benchmark environment or instrumentation fingerprint does not match the baseline",
                    {"suite": suite_name, "current": current_key, "baseline": baseline_key},
                )
            )
            return ComparisonOutcome("incomparable", diagnostics, comparisons)

        current_results = {
            _result_key(item): item
            for item in current_suite.get("metrics", {}).get("results", [])
            if isinstance(item, dict)
        }
        baseline_results = {
            _result_key(item): item
            for item in baseline_suite.get("metrics", {}).get("results", [])
            if isinstance(item, dict)
        }
        if current_results.keys() != baseline_results.keys():
            diagnostics.append(
                _diagnostic(
                    "PERF_BASELINE_INCOMPARABLE",
                    "error",
                    "Current and baseline artifacts contain different benchmark result identities",
                    {
                        "suite": suite_name,
                        "current_results": sorted(current_results),
                        "baseline_results": sorted(baseline_results),
                    },
                )
            )
            return ComparisonOutcome("incomparable", diagnostics, comparisons)

        for result_key in sorted(current_results):
            current_result = current_results[result_key]
            baseline_result = baseline_results[result_key]
            violation = _memory_violation(current_result)
            if violation is not None:
                field, value = violation
                diagnostics.append(
                    _diagnostic(
                        "MEM_FRAME_HEAP_ALLOCATION" if field == "tracked_cpp_heap_allocations" else "MEM_CAPACITY_GROWTH",
                        "error",
                        "A correctness/memory invariant failed before statistical comparison",
                        {"suite": suite_name, "benchmark_id": result_key[0], "count": result_key[1], field: value},
                    )
                )
                overall = "fail"
            if current_result.get("status") != "pass":
                diagnostics.append(
                    _diagnostic(
                        "PERF_REGRESSION",
                        "error",
                        "Current benchmark result failed a correctness invariant",
                        {"suite": suite_name, "benchmark_id": result_key[0], "count": result_key[1]},
                    )
                )
                overall = "fail"

            if (
                ("scenario_hash" in current_result or "scenario_hash" in baseline_result)
                and (
                    current_result.get("scenario_hash") != baseline_result.get("scenario_hash")
                    or current_result.get("plan_hash") != baseline_result.get("plan_hash")
                )
            ):
                diagnostics.append(
                    _diagnostic(
                        "PERF_BASELINE_INCOMPARABLE",
                        "error",
                        "Scenario or execution-plan hash differs from the baseline",
                        {"suite": suite_name, "benchmark_id": result_key[0], "count": result_key[1]},
                    )
                )
                return ComparisonOutcome("incomparable", diagnostics, comparisons)

            current_distributions = dict(_distribution_items(current_result))
            baseline_distributions = dict(_distribution_items(baseline_result))
            if current_distributions.keys() != baseline_distributions.keys():
                diagnostics.append(
                    _diagnostic(
                        "PERF_BASELINE_INCOMPARABLE",
                        "error",
                        "Current and baseline results contain different timing distributions",
                        {
                            "suite": suite_name,
                            "benchmark_id": result_key[0],
                            "count": result_key[1],
                            "current": sorted(current_distributions),
                            "baseline": sorted(baseline_distributions),
                        },
                    )
                )
                return ComparisonOutcome("incomparable", diagnostics, comparisons)
            for metric_name, current_distribution in current_distributions.items():
                baseline_distribution = baseline_distributions.get(metric_name)
                assert baseline_distribution is not None
                baseline_median = float(baseline_distribution["median"])
                current_median = float(current_distribution["median"])
                baseline_mad = float(baseline_distribution["mad"])
                if baseline_median <= 0.0:
                    continue
                absolute_delta = current_median - baseline_median
                relative_delta = absolute_delta / baseline_median
                absolute_floor = GPU_ABSOLUTE_FLOOR_MS if "gpu" in metric_name else CPU_ABSOLUTE_FLOOR_MS
                noisy = baseline_mad > max(baseline_median * WARN_RELATIVE, absolute_floor)
                metric_status = "pass"
                if noisy:
                    metric_status = "warn"
                    if overall == "pass":
                        overall = "warn"
                    diagnostics.append(
                        _diagnostic(
                            "PERF_NOISE_TOO_HIGH",
                            "warning",
                            "Baseline dispersion is too high for a strong regression conclusion",
                            {
                                "suite": suite_name,
                                "benchmark_id": result_key[0],
                                "count": result_key[1],
                                "metric": metric_name,
                                "baseline_mad_ms": baseline_mad,
                            },
                        )
                    )
                elif relative_delta >= FAIL_RELATIVE and absolute_delta > max(3.0 * baseline_mad, absolute_floor):
                    metric_status = "fail"
                    overall = "fail"
                    diagnostics.append(
                        _diagnostic(
                            "PERF_REGRESSION",
                            "error",
                            "Comparable benchmark median crossed the provisional failure threshold",
                            {
                                "suite": suite_name,
                                "benchmark_id": result_key[0],
                                "count": result_key[1],
                                "metric": metric_name,
                                "relative_delta": relative_delta,
                                "absolute_delta_ms": absolute_delta,
                            },
                        )
                    )
                elif relative_delta >= WARN_RELATIVE and absolute_delta > max(2.0 * baseline_mad, absolute_floor):
                    metric_status = "warn"
                    if overall == "pass":
                        overall = "warn"
                    diagnostics.append(
                        _diagnostic(
                            "PERF_REGRESSION",
                            "warning",
                            "Comparable benchmark median crossed the provisional warning threshold",
                            {
                                "suite": suite_name,
                                "benchmark_id": result_key[0],
                                "count": result_key[1],
                                "metric": metric_name,
                                "relative_delta": relative_delta,
                                "absolute_delta_ms": absolute_delta,
                            },
                        )
                    )
                comparisons.append(
                    {
                        "suite": suite_name,
                        "benchmark_id": result_key[0],
                        "count": result_key[1],
                        "metric": metric_name,
                        "status": metric_status,
                        "baseline_median_ms": baseline_median,
                        "current_median_ms": current_median,
                        "relative_delta": relative_delta,
                        "absolute_delta_ms": absolute_delta,
                        "baseline_mad_ms": baseline_mad,
                    }
                )
    return ComparisonOutcome(overall, diagnostics, comparisons)


def attach_comparison(current: dict[str, Any], baseline: dict[str, Any]) -> tuple[dict[str, Any], int]:
    outcome = compare_documents(current, baseline)
    metrics = current.setdefault("metrics", {})
    metrics["comparison"] = {
        "status": outcome.status,
        "warn_relative_threshold": WARN_RELATIVE,
        "fail_relative_threshold": FAIL_RELATIVE,
        "cpu_absolute_floor_ms": CPU_ABSOLUTE_FLOOR_MS,
        "gpu_absolute_floor_ms": GPU_ABSOLUTE_FLOOR_MS,
        "results": outcome.metrics,
    }
    current.setdefault("diagnostics", []).extend(outcome.diagnostics)
    if outcome.status == "incomparable":
        current["status"] = "incomparable"
        return current, 3
    if outcome.status == "fail":
        current["status"] = "fail"
        return current, 1
    if outcome.status == "warn" and current.get("status") == "pass":
        current["status"] = "warn"
    return current, 0
