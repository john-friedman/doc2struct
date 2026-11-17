"""Run original ParseBench pages through external Tesseract and parse_bb.

Uses the official installed parse-bench scorers. Charts and layout are never
scheduled or included in the overall score, including for published peers.
OCR output is cached independently of doc2struct and the evaluation results.
"""
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor, wait, FIRST_COMPLETED
from datetime import datetime, timezone
import csv
import hashlib
import importlib.metadata
import importlib.util
import io
import json
import math
import os
from pathlib import Path
from statistics import mean
import subprocess
import sys
from time import perf_counter
import traceback


SCRIPTS = Path(__file__).resolve().parent
ROOT = SCRIPTS.parent / 'data/parsebench_bb'
PIPELINE = "doc2struct-tesseract-bb"
DIMENSIONS = ("text", "formatting", "tables", "forms")
COLUMNS = ("Text", "Formatting", "Tables", "Forms")
SUBSETS = ("text", "headings", "strikeout", "underline", "tables",
           "complex_table_headers", "financial_tables", "insurance_filing_tables",
           "dense_tables", "forms", "fillable_forms")
LEADERBOARD_URL = "https://raw.githubusercontent.com/run-llama/ParseBench/main/leaderboard-v2.csv"
TSV_READER = "literal-tabs-v1"
for variable in ("OMP_NUM_THREADS", "OMP_THREAD_LIMIT", "OPENBLAS_NUM_THREADS", "MKL_NUM_THREADS"):
    os.environ[variable] = "1"


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def save_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + f".{os.getpid()}.tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(path)


def load_module(name, path, *, package=False):
    kwargs = {"submodule_search_locations": [str(Path(path).parent)]} if package else {}
    spec = importlib.util.spec_from_file_location(name, path, **kwargs)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def read_cases(data_root, selected):
    for subset in selected:
        path = data_root / "annotations" / (subset + ".jsonl")
        for line in path.read_text(encoding="utf-8").splitlines():
            if not line.strip():
                continue
            row = json.loads(line)
            for key, default in (("metadata", {}), ("test_rules", [])):
                value = row.get(key, default)
                row[key] = json.loads(value) if isinstance(value, str) else value
            row["subset"] = subset
            yield row


def source_path(data_root, row):
    relative = row["document"]
    return data_root / "documents" / (hashlib.sha256(relative.encode()).hexdigest() + Path(relative).suffix)


def case_directory(run, row):
    return run / "cases" / row["subset"] / hashlib.sha256(row["id"].encode()).hexdigest()[:24]


def initialize(config):
    global CONFIG, doc2struct, pdfium, Image, convert_to_parsebench, RULES, RELEVANT
    CONFIG = config
    package = Path(config["package"])
    # Pin the production package and extension recorded for this run.
    load_module("doc2struct._native", config["native"])
    doc2struct = load_module("doc2struct", package / "__init__.py", package=True)
    if not hasattr(doc2struct, "parse_bb"):
        raise RuntimeError("Rebuild doc2struct with bounding-box adapter support")
    import pypdfium2 as pdfium
    from PIL import Image
    from parse_bench.evaluation.next_score import NEXT_DIMENSIONS
    converter = load_module("bb_parsebench_converter", SCRIPTS / "parsebench_converter.py")
    convert_to_parsebench = converter.convert_to_parsebench
    RULES = doc2struct.load_rules(config["rules"])
    RELEVANT = {
        subset: {slot.metric for name in DIMENSIONS for slot in NEXT_DIMENSIONS[name]
                 if slot.subset == subset} for subset in SUBSETS
    }


def read_tsv(text, width, height, dpi):
    words = []
    # Tesseract writes literal text after the last tab, without CSV escaping.
    # A recognized quotation mark must never consume subsequent TSV records.
    for row in csv.DictReader(io.StringIO(text), delimiter="\t", quoting=csv.QUOTE_NONE):
        if row.get("level") != "5" or not (row.get("text") or "").strip():
            continue
        left, top, w, h = (int(row[key]) for key in ("left", "top", "width", "height"))
        if w <= 0 or h <= 0:
            continue
        words.append({"text": row["text"], "bbox": [left, top, left + w, top + h],
                      "confidence": float(row["conf"]) / 100})
    return {"width": width, "height": height, "dpi": dpi, "words": words}


def ocr_image(image, cache, index, dpi):
    image_path = cache / f"page_{index:04d}.{os.getpid()}.png"
    image.save(image_path, format="PNG", dpi=(dpi, dpi))
    try:
        start = perf_counter()
        completed = subprocess.run([
            CONFIG["tesseract"], str(image_path), "stdout", "-l", CONFIG["ocr"]["language"],
            "--dpi", str(max(1, round(dpi))), "--oem", str(CONFIG["ocr"]["oem"]),
            "--psm", str(CONFIG["ocr"]["psm"]), "tsv",
        ], stdin=subprocess.DEVNULL, capture_output=True, encoding="utf-8", errors="replace",
            timeout=CONFIG["ocr"]["timeout_seconds"], check=True)
        elapsed = perf_counter() - start
        (cache / f"page_{index:04d}.tsv").write_text(completed.stdout, encoding="utf-8")
        if completed.stderr.strip():
            (cache / f"page_{index:04d}.stderr.txt").write_text(completed.stderr, encoding="utf-8")
        return read_tsv(completed.stdout, image.width, image.height, dpi), elapsed
    finally:
        image_path.unlink(missing_ok=True)


def ocr_document(path, row):
    input_sha = sha256(path)
    key = hashlib.sha256(json.dumps({"source_sha256": input_sha, "document": row["document"],
        "ocr": CONFIG["ocr"], "renderer": CONFIG["renderer"]}, sort_keys=True).encode()).hexdigest()
    cache = Path(CONFIG["cache"]) / key
    cache.mkdir(parents=True, exist_ok=True)
    cached = cache / ("ocr." + TSV_READER + ".json")
    if cached.exists():
        value = json.loads(cached.read_text(encoding="utf-8"))
        return value["pages"], {**value["timings"], "cache_hit": True}, input_sha, key
    legacy = cache / "ocr.json"
    if legacy.exists():
        # Preserve the initial run and its old boxes. Recover corrected boxes
        # from the exact saved OCR output, without re-rendering or re-running OCR.
        value = json.loads(legacy.read_text(encoding="utf-8"))
        value["pages"] = [read_tsv((cache / f"page_{i:04d}.tsv").read_text(encoding="utf-8"),
            page["width"], page["height"], page["dpi"]) for i, page in enumerate(value["pages"])]
        value["tsv_reader"] = TSV_READER
        save_json(cached, value)
        return value["pages"], {**value["timings"], "cache_hit": True}, input_sha, key
    pages, render_seconds, ocr_seconds = [], 0.0, 0.0
    if path.suffix.lower() == ".pdf":
        document = pdfium.PdfDocument(path)
        try:
            document.init_forms()  # Render visible form values; never read PDF text.
            for index in range(len(document)):
                page = document[index]
                bitmap = image = None
                try:
                    start = perf_counter()
                    scale = CONFIG["ocr"]["dpi"] / 72
                    width, height = page.get_size()
                    pixels = width * height * scale * scale
                    if pixels > CONFIG["ocr"]["max_pixels"]:
                        scale *= math.sqrt(CONFIG["ocr"]["max_pixels"] / pixels)
                    dpi = 72 * scale
                    bitmap = page.render(scale=scale, may_draw_forms=True)
                    image = bitmap.to_pil()
                    render_seconds += perf_counter() - start
                    result, elapsed = ocr_image(image, cache, index, dpi)
                    pages.append(result)
                    ocr_seconds += elapsed
                finally:
                    if image is not None:
                        image.close()
                    if bitmap is not None:
                        bitmap.close()
                    page.close()
        finally:
            document.close()
    else:
        with Image.open(path) as original:
            image = original.convert("RGB")
            try:
                result, elapsed = ocr_image(image, cache, 0, CONFIG["ocr"]["dpi"])
                pages.append(result)
                ocr_seconds += elapsed
            finally:
                image.close()
    timings = {"render_seconds": render_seconds, "ocr_seconds": ocr_seconds}
    save_json(cached, {"source_sha256": input_sha, "ocr": CONFIG["ocr"], "tsv_reader": TSV_READER,
                       "pages": pages, "timings": timings})
    return pages, {**timings, "cache_hit": False}, input_sha, key


def evaluate(row):
    from parse_bench.evaluation.metrics.parse.next_metrics import compute_next_metrics
    from parse_bench.schemas.parse_output import ParseOutput
    from parse_bench.schemas.pipeline_io import InferenceResult, InferenceRequest
    from parse_bench.test_cases.schema import ParseTestCase
    destination = case_directory(Path(CONFIG["run"]), row)
    destination.mkdir(parents=True, exist_ok=True)
    result = {"id": row["id"], "subset": row["subset"], "document": row["document"],
              "status": "ok", "scores": {}, "stage": "ocr"}
    started = perf_counter()
    try:
        path = source_path(Path(CONFIG["data_root"]), row)
        pages, timings, input_sha, cache_key = ocr_document(path, row)
        result.update(timings)
        result.update({"input_sha256": input_sha, "ocr_cache_key": cache_key,
                       "pages": len(pages), "words": sum(len(p["words"]) for p in pages)})
        result["stage"] = "parse_bb"
        start = perf_counter()
        rows = doc2struct.parse_bb(pages, rules=RULES)
        result["parse_seconds"] = perf_counter() - start
        result["stage"] = "conversion"
        converted = convert_to_parsebench(rows, row["id"])
        save_json(destination / "rows.json", rows)
        save_json(destination / "output.json", converted)
        (destination / "output.md").write_text(converted["markdown"], encoding="utf-8")
        # Expected answers enter only after inference and conversion are saved.
        result["stage"] = "evaluation"
        metadata = row.get("metadata") or {}
        flags = {name: row.get(name, metadata.get(name, default)) for name, default in (
            ("max_top_title_rows", 1), ("allow_splitting_ambiguous_merged_tables", False),
            ("trm_unsupported", False), ("allow_merged_sub_rows", False))}
        case = ParseTestCase(test_id=row["id"], group=row["subset"], file_path=path,
            expected_markdown=row.get("expected_markdown"), test_rules=row.get("test_rules") or [],
            metadata=metadata, tags=row.get("tags") or [], **flags)
        now = datetime.now(timezone.utc)
        inference = InferenceResult(request=InferenceRequest(example_id=row["id"],
            source_file_path=str(path), product_type="parse"), pipeline_name=PIPELINE,
            product_type="parse", raw_output={}, output=ParseOutput.model_validate(converted),
            started_at=now, completed_at=now,
            latency_in_ms=round((perf_counter() - started) * 1000))
        start = perf_counter()
        metrics = compute_next_metrics(inference, case)
        result["evaluation_seconds"] = perf_counter() - start
        relevant = RELEVANT[row["subset"]]
        result["scores"] = {m.metric_name: m.value for m in metrics if m.metric_name in relevant}
        result["metric_metadata"] = {m.metric_name: m.metadata for m in metrics if m.metric_name in relevant}
        required = relevant - ({"formatting_v2"} if row["subset"] == "text" else set())
        if required - result["scores"].keys():
            raise RuntimeError("Official scorer omitted required metric: " + str(required - result["scores"].keys()))
        result["tables"] = sum(r["type"] == "table" for r in rows)
        result["output_characters"] = len(converted["markdown"])
        result["stage"] = "complete"
    except Exception:
        result["status"] = "error"
        result["error"] = traceback.format_exc()
        result["scores"] = {name: 0.0 for name in RELEVANT[row["subset"]]}
    result["total_seconds"] = perf_counter() - started
    save_json(destination / "result.json", result)
    return {key: value for key, value in result.items() if key != "metric_metadata"}


def summarize(config, selected):
    from parse_bench.evaluation.next_score import NEXT_DIMENSIONS, combine_next_score
    run, data_root = Path(config["run"]), Path(config["data_root"])
    expected = list(read_cases(data_root, selected))
    cases = []
    for row in expected:
        path = case_directory(run, row) / "result.json"
        if path.exists():
            value = json.loads(path.read_text(encoding="utf-8"))
            value.pop("metric_metadata", None)
            cases.append(value)
    groups = defaultdict(list)
    for case in cases:
        groups[case["subset"]].append(case)
    subsets, aggregates = {}, {}
    for subset in selected:
        relevant = {slot.metric for name in DIMENSIONS for slot in NEXT_DIMENSIONS[name] if slot.subset == subset}
        values = groups[subset]
        metrics = {}
        for metric in sorted(relevant):
            scored = [case["scores"][metric] for case in values if metric in case["scores"]]
            metrics[metric] = {"score": mean(scored) if scored else None, "count": len(scored)}
        subsets[subset] = {"cases": len(values), "status": dict(Counter(v["status"] for v in values)),
                           "metrics": metrics}
        aggregates[subset] = {"avg_" + metric: details["score"] for metric, details in metrics.items()}
    combined = combine_next_score(aggregates, {s: len(groups[s]) for s in selected})
    dimensions = {name: combined["dimensions"][name] for name in DIMENSIONS}
    complete = len(cases) == len(expected) and set(selected) == set(SUBSETS)
    overall = mean(v["score"] for v in dimensions.values()) if complete and all(v["score"] is not None for v in dimensions.values()) else None
    leaderboard_path = ROOT / "leaderboard-v2.csv"
    peers = list(csv.DictReader(leaderboard_path.read_text(encoding="utf-8-sig").splitlines()))
    ranking = []
    for peer in peers:
        try:
            scores = {column: float(peer[column]) for column in COLUMNS}
        except (ValueError, KeyError, TypeError):
            continue
        ranking.append({"Provider": peer["Provider"], "Category": peer["Category"],
                        "Overall": mean(scores.values()), **scores,
                        "Scope": "published full benchmark; four dimensions recomputed"})
    if overall is not None:
        ranking.append({"Provider": PIPELINE, "Category": "Document Parsers", "Overall": overall * 100,
                        **{column: dimensions[name]["score"] * 100 for name, column in zip(DIMENSIONS, COLUMNS)},
                        "Scope": "local original ParseBench-2 pages; charts/layout excluded"})
    ranking.sort(key=lambda value: (-value["Overall"], value["Provider"]))
    for i, row in enumerate(ranking):
        row["Rank"] = 1 + sum(other["Overall"] > row["Overall"] for other in ranking)
    rank = next((row["Rank"] for row in ranking if row["Provider"] == PIPELINE), None)
    result = {"pipeline": PIPELINE, "complete": complete,
        "dataset": json.loads((data_root / "manifest.json").read_text(encoding="utf-8")),
        "configuration": config, "expected_cases": len(expected), "completed_cases": len(cases),
        "status": dict(Counter(c["status"] for c in cases)), "subsets": subsets,
        "scoring_scope": {"dimensions": list(DIMENSIONS), "excluded_subsets": ["charts", "layout"],
            "overall": "Equal mean of Text, Formatting, Tables, Forms; official within-dimension weights retained.",
            "failures": "Parser, OCR, and scorer errors count as zero. No native-text or score-based exclusions.",
            "missing_optional_metrics": "Text cases without applicable formatting annotations do not enter formatting_v2's mean."},
        "dimensions": dimensions, "overall": overall, "rank": rank, "ranking_entries": len(ranking),
        "leaderboard": {"source": LEADERBOARD_URL, "sha256": sha256(leaderboard_path),
            "comparison": "All peers re-ranked on the same four columns; published six-dimension Overall is ignored."},
        "totals": {"pages": sum(c.get("pages", 0) for c in cases), "words": sum(c.get("words", 0) for c in cases),
            **{key: sum(c.get(key, 0) for c in cases) for key in ("render_seconds", "ocr_seconds", "parse_seconds", "evaluation_seconds")}},
        "errors": [{key: c.get(key) for key in ("id", "subset", "stage", "error")} for c in cases if c["status"] == "error"]}
    save_json(run / "summary.json", result)
    save_json(run / "case_index.json", cases)
    fields = ["Rank", "Provider", "Category", "Overall", *COLUMNS, "Scope"]
    with (run / "ranking.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(ranking)
    if complete:
        export = ROOT / "results" / PIPELINE
        export.mkdir(parents=True, exist_ok=True)
        save_json(export / "summary.json", result)
        save_json(export / "case_index.json", cases)
        (export / "ranking.csv").write_bytes((run / "ranking.csv").read_bytes())
        with (export / "subset_scores.csv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(["subset", "metric", "score_percent", "scored_cases", "total_cases", "errors"])
            for subset, details in subsets.items():
                for metric, value in details["metrics"].items():
                    writer.writerow([subset, metric, None if value["score"] is None else value["score"] * 100,
                        value["count"], details["cases"], details["status"].get("error", 0)])
        lines = [PIPELINE, f"Cases: {len(cases)}; status: {result['status']}",
            "Original benchmark documents; every page rasterized before OCR. English Tesseract TSV only.",
            "No embedded PDF text, font metadata, expected answers, or SEC replacements used for inference.",
            "Charts and layout excluded. Overall and peer ranks use Text, Formatting, Tables, Forms only.",
            "Official within-dimension weights are unchanged.", ""]
        lines += [f"{name.title()}: {details['score'] * 100:.4f}%" for name, details in dimensions.items()]
        lines += [f"Overall: {overall * 100:.4f}%", f"Rank: {rank}/{len(ranking)}", "",
                  "Published provider scores are rounded aggregates from the linked leaderboard; this is a local comparison, not an official submission."]
        (export / "REPORT.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(json.dumps({"complete": complete, "cases": len(cases), "status": result["status"],
        "dimensions": {name: value["score"] for name, value in dimensions.items()},
        "overall": overall, "rank": rank, "ranking_entries": len(ranking)}, indent=2), flush=True)
    return result


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--data-root", type=Path, default=SCRIPTS.parent / "data/parsebench")
    cli.add_argument("--run-dir", type=Path, default=ROOT / "runs" / "integrated")
    cli.add_argument("--native-library", type=Path)
    cli.add_argument("--cache-dir", type=Path, default=ROOT / "cache")
    cli.add_argument("--rules", default="BB:PARSEBENCH", help="Bundled preset or rules JSON file")
    cli.add_argument("--tesseract", default=r"C:\Program Files\Tesseract-OCR\tesseract.exe")
    cli.add_argument("--workers", type=int, default=8)
    cli.add_argument("--dpi", type=int, default=300)
    cli.add_argument("--psm", type=int, default=3)
    cli.add_argument("--language", default="eng")
    cli.add_argument("--subset", choices=SUBSETS, action="append")
    cli.add_argument("--limit", type=int)
    cli.add_argument("--retry-errors", action="store_true")
    cli.add_argument("--summarize", action="store_true")
    args = cli.parse_args()
    if args.workers < 1 or args.dpi < 1:
        cli.error("workers and dpi must be positive")
    if args.native_library:
        load_module("doc2struct._native", args.native_library.resolve())
    import doc2struct
    from doc2struct._engine import _native
    if not hasattr(doc2struct, "parse_bb"):
        cli.error("Rebuild doc2struct with bounding-box adapter support")
    package = Path(doc2struct.__file__).resolve().parent
    native = Path(_native.__file__).resolve()
    rules = doc2struct.load_rules(args.rules).to_dict()
    rules_bytes = json.dumps(rules, sort_keys=True, ensure_ascii=False).encode("utf-8")
    run, data_root = args.run_dir.resolve(), args.data_root.resolve()
    run.mkdir(parents=True, exist_ok=True)
    version = subprocess.run([args.tesseract, "--version"], capture_output=True, text=True,
                              check=True).stdout.splitlines()[0]
    config = {"run": str(run), "data_root": str(data_root), "cache": str(args.cache_dir.resolve()),
        "package": str(package), "native": str(native), "native_sha256": sha256(native),
        "rules": rules, "rules_source": args.rules, "rules_sha256": hashlib.sha256(rules_bytes).hexdigest(),
        "converter_sha256": sha256(SCRIPTS / "parsebench_converter.py"),
        "runner_sha256": sha256(Path(__file__)), "parse_bench_version": importlib.metadata.version("parse-bench"),
        "renderer": {"name": "pypdfium2", "version": importlib.metadata.version("pypdfium2")},
        "tesseract": args.tesseract, "tsv_reader": TSV_READER,
        "ocr": {"version": version, "dpi": args.dpi, "language": args.language,
            "oem": 1, "psm": args.psm, "timeout_seconds": 120, "max_pixels": 40_000_000},
        "created_at": datetime.now(timezone.utc).isoformat()}
    config_path = run / "configuration.json"
    if config_path.exists():
        previous = json.loads(config_path.read_text(encoding="utf-8"))
        for key in ("native_sha256", "rules_sha256", "converter_sha256", "runner_sha256", "ocr", "renderer"):
            if previous[key] != config[key]:
                cli.error(f"Existing run has different {key}; use a new --run-dir")
        config["created_at"] = previous["created_at"]
    else:
        save_json(config_path, config)
    selected = args.subset or list(SUBSETS)
    if args.summarize:
        summarize(config, selected)
        return
    tasks = []
    for row in read_cases(data_root, selected):
        if not source_path(data_root, row).is_file():
            raise FileNotFoundError(source_path(data_root, row))
        artifact = case_directory(run, row) / "result.json"
        if artifact.exists() and not (args.retry_errors and json.loads(artifact.read_text(encoding="utf-8"))["status"] == "error"):
            continue
        tasks.append(row)
    if args.limit is not None:
        tasks = tasks[:args.limit]
    print(f"Pipeline: {PIPELINE}; pending cases: {len(tasks)}; workers: {args.workers}; {version}", flush=True)
    started, completed = perf_counter(), 0
    iterator = iter(tasks)
    with ProcessPoolExecutor(max_workers=args.workers, initializer=initialize,
                             initargs=(config,), max_tasks_per_child=80) as pool:
        pending, exhausted = {}, False
        while pending or not exhausted:
            while not exhausted and len(pending) < args.workers * 2:
                try:
                    row = next(iterator)
                except StopIteration:
                    exhausted = True
                    break
                pending[pool.submit(evaluate, row)] = row["id"]
            if not pending:
                break
            done, _ = wait(pending, timeout=20, return_when=FIRST_COMPLETED)
            if not done:
                print(f"Waiting; {completed}/{len(tasks)} completed; elapsed {perf_counter() - started:.0f}s", flush=True)
            for future in done:
                name = pending.pop(future)
                value = future.result()
                completed += 1
                if completed % 25 == 0 or value["status"] == "error" or completed == len(tasks):
                    print(f"{completed}/{len(tasks)} {name} {value['status']} {value.get('scores')} elapsed={perf_counter() - started:.1f}s", flush=True)
                    if value["status"] == "error":
                        print(value["error"], flush=True)
    summarize(config, selected)


if __name__ == "__main__":
    main()
