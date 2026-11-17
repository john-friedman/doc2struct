"""Compare native PDF adapters on the SEC ParseBench corpus and local PDFs.

Run with parsebench-sec's virtual environment (it supplies parse-bench).
Evaluation reuses that repository's rules and metrics, with generic HTML matrix
rendering that preserves cell styles, and includes PDF table cases. Artifacts
stay under --output in this repository.
An optional --native-library loads a local CMake build without reinstalling it.
"""
import argparse
from collections import defaultdict
import gc
import hashlib
import importlib.util
import json
from pathlib import Path
from statistics import mean, median
import sys
import tarfile
from time import perf_counter_ns


ROOT = Path(__file__).resolve().parents[1]
ADAPTERS = ("pdf_adapter", "pdf_adapter_experimental")


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def pdf_documents(directory):
    for path in sorted(directory.rglob("*")):
        if not path.is_file():
            continue
        if path.suffix.lower() == ".pdf":
            yield str(path), path.read_bytes()
        elif path.name.lower().endswith((".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tar.xz")):
            with tarfile.open(path, "r:*") as archive:
                for member in archive:
                    if member.isfile() and Path(member.name).suffix.lower() == ".pdf":
                        with archive.extractfile(member) as stream:
                            yield f"{path}!{member.name}", stream.read()


def speed(doc2struct, documents, rules, repeats, experimental_rules=None):
    """Time parsing and Python export; exclude file I/O, metrics, and destruction."""
    parsers = {adapter: doc2struct.Parser() for adapter in ADAPTERS}
    rounds = {adapter: [] for adapter in ADAPTERS}
    settings = {adapter: experimental_rules if adapter == ADAPTERS[1] and experimental_rules else rules
                for adapter in ADAPTERS}
    enabled = gc.isenabled()
    try:
        # Warm both adapters and configuration on the complete identical corpus.
        for adapter, parser in parsers.items():
            for _, content in documents:
                result = parser.parse_pdf(content, rules=settings[adapter], adapter=adapter)
                del result
        gc.disable()
        for repeat in range(repeats):
            for adapter in ADAPTERS[::1 if repeat % 2 == 0 else -1]:
                elapsed = 0
                for _, content in documents:
                    start = perf_counter_ns()
                    result = parsers[adapter].parse_pdf(content, rules=settings[adapter], adapter=adapter)
                    elapsed += perf_counter_ns() - start
                    del result
                rounds[adapter].append(elapsed / 1e9)
    finally:
        if enabled:
            gc.enable()
        for parser in parsers.values():
            parser.close()
    size = sum(len(content) for _, content in documents)
    return {
        "documents": len(documents), "bytes": size,
        "input_sha256": hashlib.sha256(b"".join(hashlib.sha256(b).digest() for _, b in documents)).hexdigest(),
        "repeats": repeats, "rules": "SEC:10-K" if rules == "SEC:10-K" else "parsebench-sec/rules/doc2struct.json",
        "experimental_rules": experimental_rules,
        "timing": "One thread; parse and Python export only; preloaded inputs; warmed; alternating adapter order.",
        "adapters": {
            adapter: {"seconds": values, "median_seconds": median(values),
                      "median_mb_per_second": size / 1e6 / median(values)}
            for adapter, values in rounds.items()
        },
    }


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--parsebench-root", required=True, type=Path)
    cli.add_argument("--native-library", type=Path)
    cli.add_argument("--experimental-rules", help="Override only the experimental adapter's rules, e.g. PDF:PARSEBENCH")
    cli.add_argument("--speed-directory", type=Path, default=ROOT / "data" / "documents1")
    cli.add_argument("--output", type=Path, default=ROOT / "data" / "pdf_adapter_experimental")
    cli.add_argument("--repeats", type=int, default=5)
    cli.add_argument("--skip-full-speed", action="store_true", help="Only time the ParseBench PDFs")
    args = cli.parse_args()
    if args.repeats < 1:
        cli.error("--repeats must be positive")
    if args.native_library:
        load_module("doc2struct._native", args.native_library.resolve())
    import doc2struct
    from doc2struct._engine import _native
    evaluator = load_module("pdf_adapter_parsebench_evaluator", args.parsebench_root / "src" / "run_doc2struct.py")
    table_output = load_module("parsebench_table_output", ROOT / "build_scripts/parsebench_output.py")
    evaluator._table_html = table_output.table_html
    rules = doc2struct.load_rules(args.parsebench_root / "rules" / "doc2struct.json")
    print(f"Python: {sys.executable}\nNative library: {_native.__file__}", flush=True)
    args.output.mkdir(parents=True, exist_ok=True)
    cases, documents, grouped = [], [], defaultdict(lambda: defaultdict(list))
    for path in sorted((args.parsebench_root / "data").glob("*/*/expected.json")):
        descriptor = json.loads(path.read_text(encoding="utf-8"))
        source = path.parent / descriptor["document"]
        if source.suffix.lower() != ".pdf":
            continue
        content = source.read_bytes()
        name = path.parent.relative_to(args.parsebench_root / "data").as_posix()
        documents.append((name, content))
        metric = descriptor.get("metadata", {}).get("primary_metric") or {
            "headings": "formatting_v2", "text": "text_next"
        }.get(name.split("/")[0], "table_next")
        case = {"case": name, "metric": metric, "input_sha256": hashlib.sha256(content).hexdigest(), "adapters": {}}
        for adapter in ADAPTERS:
            selected_rules = args.experimental_rules if adapter == ADAPTERS[1] and args.experimental_rules else rules
            rows = doc2struct.parse_pdf(content, rules=selected_rules, adapter=adapter)
            converted = evaluator.convert_to_eval(rows, descriptor["id"])
            score, _ = evaluator.evaluate(converted, descriptor, metric)
            case["adapters"][adapter] = {"score": score, "tables": sum(r["type"] == "table" for r in rows)}
            grouped[name.split("/")[0]][adapter].append(score)
            destination = args.output / "cases" / name / f"{adapter}.json"
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_text(json.dumps({"score": score, "rows": rows, "markdown": converted["markdown"]}, ensure_ascii=False, indent=2), encoding="utf-8")
        cases.append(case)
        print(f"{name}: " + " -> ".join(f"{case['adapters'][a]['score']:.2%}" for a in ADAPTERS), flush=True)
    summary = {
        "python": sys.executable, "native_library": str(_native.__file__),
        "native_sha256": hashlib.sha256(Path(_native.__file__).read_bytes()).hexdigest(),
        "table_conversion_sha256": hashlib.sha256((ROOT / "build_scripts/parsebench_output.py").read_bytes()).hexdigest(),
        "experimental_rules": args.experimental_rules,
        "cases": cases,
        "scores_by_subset": {
            subset: {"cases": len(values[ADAPTERS[0]]), **{a: mean(values[a]) for a in ADAPTERS}}
            for subset, values in grouped.items()
        },
    }
    table_cases = [case for case in cases if case["metric"] == "table_next"]
    summary["pdf_tables"] = {
        "cases": len(table_cases),
        **{a: mean(case["adapters"][a]["score"] for case in table_cases) for a in ADAPTERS},
        "improved": sum(case["adapters"][ADAPTERS[1]]["score"] > case["adapters"][ADAPTERS[0]]["score"] + 1e-9 for case in table_cases),
        "regressed": sum(case["adapters"][ADAPTERS[1]]["score"] < case["adapters"][ADAPTERS[0]]["score"] - 1e-9 for case in table_cases),
    }
    # Save accuracy before potentially longer throughput runs.
    output = args.output / "comparison.json"
    output.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print("Accuracy:", json.dumps(summary["scores_by_subset"], indent=2), flush=True)
    summary["speed_parsebench"] = speed(doc2struct, documents, rules, args.repeats, args.experimental_rules)
    print("ParseBench speed:", json.dumps(summary["speed_parsebench"], indent=2), flush=True)
    full_pdfs = [] if args.skip_full_speed else list(pdf_documents(args.speed_directory))
    if full_pdfs:
        print(f"Timing {len(full_pdfs)} full PDFs ({sum(len(b) for _, b in full_pdfs)/1e6:.1f} MB)", flush=True)
        summary["speed_full_documents"] = speed(doc2struct, full_pdfs, "SEC:10-K", args.repeats, args.experimental_rules)
        print("Full PDF speed:", json.dumps(summary["speed_full_documents"], indent=2), flush=True)
    output.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"Saved {output}", flush=True)


if __name__ == "__main__":
    main()
