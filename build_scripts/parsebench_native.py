"""Evaluate the experimental native PDF adapter on original ParseBench-2 PDFs.

Uses the SEC experiment's conversion with generic table-style support and the
official per-document scorers and dimension weights. Development revisions are
compared on the same inputs. Inference does not consult expected answers.
"""
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor, wait, FIRST_COMPLETED
from datetime import datetime, timezone
import csv
import hashlib
import importlib.metadata
import importlib.util
import json
import os
from pathlib import Path
from statistics import mean
import sys
from time import perf_counter
import traceback

PACKAGE = Path(__file__).resolve().parents[1]
ROOT = Path(os.environ.get('DOC2STRUCT_PARSEBENCH_DATA', PACKAGE / 'data/parsebench'))
SEC_ROOT = Path(os.environ.get('DOC2STRUCT_PARSEBENCH_SEC', r'C:\Users\jgfri\Documents\datamule\oss\research\parsebench-sec'))
NATIVE = Path(os.environ['DOC2STRUCT_PARSEBENCH_NATIVE']) if os.environ.get('DOC2STRUCT_PARSEBENCH_NATIVE') else None
RULES_SOURCE = os.environ.get('DOC2STRUCT_PARSEBENCH_RULES', 'PDF:PARSEBENCH')
ALL_SUBSETS = ('text', 'headings', 'strikeout', 'underline', 'tables',
           'complex_table_headers', 'financial_tables', 'insurance_filing_tables',
           'dense_tables', 'charts', 'layout', 'forms', 'fillable_forms')
SUBSETS = tuple(name for name in ALL_SUBSETS if name not in ('charts', 'layout'))
ADAPTER = 'pdf_adapter_experimental'
for variable in ('OMP_NUM_THREADS', 'OPENBLAS_NUM_THREADS', 'MKL_NUM_THREADS'):
    os.environ[variable] = '1'


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def read_cases(subsets):
    for subset in subsets:
        with (ROOT / 'annotations' / (subset + '.jsonl')).open(encoding='utf-8') as stream:
            for line in stream:
                row = json.loads(line)
                for key, fallback in [('metadata', {}), ('test_rules', [])]:
                    value = row.get(key, fallback)
                    row[key] = json.loads(value) if isinstance(value, str) else value
                yield row


def document_path(row):
    relative = row['document']
    return ROOT / 'documents' / (hashlib.sha256(relative.encode()).hexdigest() + Path(relative).suffix)


def artifact_dir(run, row):
    return run / 'cases' / row['subset'] / hashlib.sha256(row['id'].encode()).hexdigest()[:24]


def initialize(run):
    global doc2struct, pdfium, raw, converter, parser, rules, OUTPUT
    OUTPUT = Path(run)
    if NATIVE is not None:
        load_module('doc2struct._native', NATIVE)
    import doc2struct
    import pypdfium2 as pdfium
    import pypdfium2.raw as raw
    converter = load_module('sec_parsebench_conversion', SEC_ROOT / 'src/run_doc2struct.py')
    table_output = load_module('parsebench_table_output', PACKAGE / 'build_scripts/parsebench_output.py')
    converter._table_html = table_output.table_html
    parser = doc2struct.Parser()
    rules = doc2struct.load_rules(RULES_SOURCE)


def text_inventory(content):
    document = pdfium.PdfDocument(content)
    pages = len(document)
    count = 0
    native_text = False
    sizes = []
    try:
        for index in range(pages):
            page = document[index]
            sizes.append(page.get_size())
            text = page.get_textpage()
            try:
                n = text.count_chars()
                count += n
                if not native_text:
                    for i in range(n):
                        cp = raw.FPDFText_GetUnicode(text, i)
                        if cp and not chr(cp).isspace() and raw.FPDFText_GetTextObject(text, i):
                            native_text = True
                            break
            finally:
                text.close(); page.close()
    finally:
        document.close()
    return {'pages': pages, 'characters': count, 'has_native_text': native_text, 'page_sizes': sizes}


def evaluate(row):
    from parse_bench.evaluation.metrics.parse.next_metrics import compute_next_metrics
    from parse_bench.schemas.parse_output import ParseOutput
    from parse_bench.schemas.pipeline_io import InferenceResult, InferenceRequest
    from parse_bench.test_cases.schema import ParseTestCase
    from parse_bench.evaluation.next_score import NEXT_DIMENSIONS
    relevant = {slot.metric for slots in NEXT_DIMENSIONS.values() for slot in slots if slot.subset == row['subset']}
    destination = artifact_dir(OUTPUT, row)
    destination.mkdir(parents=True, exist_ok=True)
    result = {'id': row['id'], 'subset': row['subset'], 'document': row['document'], 'status': 'ok', 'scores': {}}
    try:
        content = document_path(row).read_bytes()
        result['input_sha256'] = hashlib.sha256(content).hexdigest()
        result['bytes'] = len(content)
        raster = Path(row['document']).suffix.lower() != '.pdf'
        inventory = {'pages': 1, 'characters': 0, 'has_native_text': False, 'page_sizes': []} if raster else text_inventory(content)
        result.update(inventory)
        rows = []
        start = perf_counter()
        if inventory['has_native_text']:
            rows = parser.parse_pdf(content, rules=rules, adapter=ADAPTER)
        else:
            result['status'] = 'skipped_raster_input' if raster else 'skipped_no_native_text'
        result['parse_seconds'] = perf_counter() - start
        converted = converter.convert_to_eval(rows, row['id'])
        (destination / 'rows.json').write_text(json.dumps(rows, ensure_ascii=False), encoding='utf-8')
        (destination / 'output.json').write_text(json.dumps(converted, ensure_ascii=False), encoding='utf-8')
        (destination / 'output.md').write_text(converted['markdown'], encoding='utf-8')
        # Pass flags and decoration inventories exactly as the benchmark loader
        # does. Ground truth is used only after inference has been saved.
        metadata = row.get('metadata') or {}
        flags = {name: row.get(name, metadata.get(name, default)) for name, default in (
            ('max_top_title_rows', 1), ('allow_splitting_ambiguous_merged_tables', False),
            ('trm_unsupported', False), ('allow_merged_sub_rows', False))}
        case = ParseTestCase(test_id=row['id'], group=row['subset'], file_path=document_path(row),
            expected_markdown=row.get('expected_markdown'), test_rules=row.get('test_rules') or [],
            metadata=metadata, tags=row.get('tags') or [], **flags)
        now = datetime.now(timezone.utc)
        inference = InferenceResult(request=InferenceRequest(example_id=row['id'],
            source_file_path=str(document_path(row)), product_type='parse'), pipeline_name='doc2struct',
            product_type='parse', raw_output={}, output=ParseOutput.model_validate(converted),
            started_at=now, completed_at=now, latency_in_ms=round(result['parse_seconds'] * 1000))
        start = perf_counter()
        metrics = compute_next_metrics(inference, case)
        result['evaluation_seconds'] = perf_counter() - start
        result['raw_scores'] = {metric.metric_name: metric.value for metric in metrics if metric.metric_name in relevant}
        result['scores'] = {key: (value if result['status'] == 'ok' else 0.0) for key, value in result['raw_scores'].items()}
        result['metric_metadata'] = {metric.metric_name: metric.metadata for metric in metrics if metric.metric_name in relevant}
        # A missing optional formatting metric is not an evaluation failure.
        required = relevant - ({'formatting_v2'} if row['subset'] == 'text' else set())
        missing = required - result['scores'].keys()
        if missing:
            raise RuntimeError('Official scorer emitted no headline metric: ' + ', '.join(sorted(missing)))
        result['tables'] = sum(r['type'] == 'table' for r in rows)
        result['output_characters'] = len(converted['markdown'])
    except Exception:
        result['status'] = 'error'
        result['error'] = traceback.format_exc()
        result['scores'] = {key: 0.0 for key in relevant}
    (destination / 'result.json').write_text(json.dumps(result, ensure_ascii=False, indent=2), encoding='utf-8')
    # The parent only needs small progress records, not detailed scorer metadata.
    return {key: value for key, value in result.items() if key != 'metric_metadata'}


def summarize(run, selected):
    from parse_bench.evaluation.next_score import NEXT_DIMENSIONS, combine_next_score
    scope = load_module('parsebench_scope', PACKAGE / 'build_scripts/parsebench_scope.py')
    cases = []
    for row in read_cases(selected):
        path = artifact_dir(run, row) / 'result.json'
        if path.exists():
            result = json.loads(path.read_text(encoding='utf-8'))
            result.pop('metric_metadata', None)
            cases.append(result)
    subsets = {}
    for subset in selected:
        values = [case for case in cases if case['subset'] == subset]
        slots = {slot.metric for slots in NEXT_DIMENSIONS.values() for slot in slots if slot.subset == subset}
        summary = {'total': len(values), 'status': dict(Counter(case['status'] for case in values)), 'metrics': {}}
        for metric in slots:
            inclusive = [case['scores'][metric] for case in values if metric in case['scores']]
            filtered = [case['scores'][metric] for case in values if not case['status'].startswith('skipped_') and metric in case['scores']]
            summary['metrics'][metric] = {'including_skipped': mean(inclusive) if inclusive else None,
                'excluding_skipped': mean(filtered) if filtered else None,
                'including_count': len(inclusive), 'excluding_count': len(filtered)}
        subsets[subset] = summary
    scores = {}
    for mode in ['including_skipped', 'excluding_skipped']:
        metrics = {subset: {'avg_' + metric: values[mode] for metric, values in summary['metrics'].items()}
                   for subset, summary in subsets.items()}
        counts = {subset: summary['total'] if mode == 'including_skipped' else summary['total'] - sum(n for status, n in summary['status'].items() if status.startswith('skipped_')) for subset, summary in subsets.items()}
        scores[mode] = combine_next_score(metrics, counts)
        scores[mode]['in_scope_score'] = scope.overall(scores[mode]['dimensions'])
    leaderboard = list(csv.DictReader((ROOT / 'leaderboard-v2.csv').read_text(encoding='utf-8').splitlines()))
    ranks = {}
    columns = {'text': 'Text', 'formatting': 'Formatting', 'tables': 'Tables', 'charts': 'Charts', 'layout': 'Visual_Grounding', 'forms': 'Forms'}
    for mode, score in scores.items():
        values = {'Overall': score['in_scope_score'], **{columns[key]: value['score'] for key, value in score['dimensions'].items()}}
        peer_rows = [{**row, 'Overall': (str(scope.published_overall(row)*100) if scope.published_overall(row) is not None else '')} for row in leaderboard]
        ranks[mode] = {column: {'hypothetical_rank': 1 + sum(float(row[column]) > value * 100 for row in peer_rows if row.get(column) not in (None, '', '-', '—')),
                              'score': value, 'published_entries': sum(row.get(column) not in (None, '', '-', '—') for row in peer_rows)}
                       for column, value in values.items() if value is not None}
    import doc2struct
    from doc2struct._engine import _native
    native_path = Path(_native.__file__)
    result = {'dataset': json.loads((ROOT / 'manifest.json').read_text(encoding='utf-8')),
        'adapter': ADAPTER, 'native_library': str(native_path), 'native_sha256': hashlib.sha256(native_path.read_bytes()).hexdigest(),
        'parse_bench_version': importlib.metadata.version('parse-bench'),
        'conversion_source': str(SEC_ROOT / 'src/run_doc2struct.py'),
        'table_conversion_sha256': hashlib.sha256((PACKAGE / 'build_scripts/parsebench_output.py').read_bytes()).hexdigest(),
        'conversion_sha256': hashlib.sha256((SEC_ROOT / 'src/run_doc2struct.py').read_bytes()).hexdigest(),
        'rules_source': RULES_SOURCE,
        'rules': doc2struct.load_rules(RULES_SOURCE).to_dict(),
        'skip_policy': 'Raster inputs or PDFs without native text objects: exclude from filtered scores; score zero in inclusive scores. Parser/scorer errors count as zero in both modes. No score-based exclusions.',
        'layout_support': 'Instruction source bounds are not converted to benchmark layout predictions; the official layout scorer receives empty layout predictions.',
        'scoring_scope': {'dimensions': list(scope.DIMENSIONS), 'excluded_subsets': list(scope.EXCLUDED_SUBSETS), 'overall': 'Equal mean of the four in-scope dimensions; official within-dimension weights preserved. Published rankings recomputed on the same four dimensions.'},
        'cases': len(cases), 'status': dict(Counter(case['status'] for case in cases)),
        'subsets': subsets, 'scores': scores, 'leaderboard_comparison': ranks,
        'leaderboard_sha256': hashlib.sha256((ROOT / 'leaderboard-v2.csv').read_bytes()).hexdigest()}
    (run / 'summary.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
    (run / 'case_index.json').write_text(json.dumps(cases, ensure_ascii=False, indent=2), encoding='utf-8')
    print(json.dumps({'cases': result['cases'], 'status': result['status'],
        'scores': {mode: {'overall': score['in_scope_score'], **{name: value['score'] for name, value in score['dimensions'].items()}}
                   for mode, score in scores.items()}, 'leaderboard_comparison': ranks}, indent=2), flush=True)


def main():
    global NATIVE,RULES_SOURCE,ROOT,SEC_ROOT
    cli = argparse.ArgumentParser()
    cli.add_argument('--run-name', default='integrated')
    cli.add_argument('--data-root', type=Path, default=ROOT)
    cli.add_argument('--parsebench-sec-root', type=Path, default=SEC_ROOT)
    cli.add_argument('--workers', type=int, default=4)
    cli.add_argument('--subset', choices=ALL_SUBSETS, action='append')
    cli.add_argument('--limit', type=int)
    cli.add_argument('--summarize', action='store_true')
    cli.add_argument('--available-only', action='store_true')
    cli.add_argument('--retry-errors', action='store_true')
    cli.add_argument('--native-library', type=Path)
    cli.add_argument('--rules')
    args = cli.parse_args()
    ROOT=args.data_root.resolve();os.environ['DOC2STRUCT_PARSEBENCH_DATA']=str(ROOT)
    SEC_ROOT=args.parsebench_sec_root.resolve();os.environ['DOC2STRUCT_PARSEBENCH_SEC']=str(SEC_ROOT)
    if args.native_library:
        NATIVE=args.native_library.resolve();os.environ['DOC2STRUCT_PARSEBENCH_NATIVE']=str(NATIVE)
    if args.rules:
        RULES_SOURCE=args.rules;os.environ['DOC2STRUCT_PARSEBENCH_RULES']=RULES_SOURCE
    if NATIVE is not None:
        load_module('doc2struct._native', NATIVE)
    run = ROOT / 'runs' / args.run_name
    run.mkdir(parents=True, exist_ok=True)
    selected = args.subset or SUBSETS
    if args.summarize:
        summarize(run, selected); return
    def needs_evaluation(row):
        path = artifact_dir(run, row) / 'result.json'
        return not path.exists() or (args.retry_errors and json.loads(path.read_text(encoding='utf-8'))['status'] == 'error')
    tasks = (row for row in read_cases(selected) if needs_evaluation(row)
             and (not args.available_only or document_path(row).exists()))
    completed = submitted = 0
    with ProcessPoolExecutor(max_workers=args.workers, initializer=initialize, initargs=(str(run),), max_tasks_per_child=80) as pool:
        pending = {}
        exhausted = False
        while pending or not exhausted:
            while not exhausted and len(pending) < args.workers * 2:
                if args.limit and submitted >= args.limit:
                    exhausted = True; break
                try:
                    row = next(tasks)
                except StopIteration:
                    exhausted = True; break
                if not document_path(row).exists():
                    raise FileNotFoundError('PDF not downloaded: ' + row['document'])
                pending[pool.submit(evaluate, row)] = row['id']; submitted += 1
            done, _ = wait(pending, timeout=30, return_when=FIRST_COMPLETED)
            if not done:
                print('Waiting for:', list(pending.values()), flush=True)
            for future in done:
                name = pending.pop(future)
                value = future.result(); completed += 1
                if completed % 25 == 0 or value['status'] == 'error':
                    print(completed, name, value['status'], value.get('scores'), flush=True)
                    if value['status'] == 'error': print(value['error'], flush=True)
    summarize(run, selected)


if __name__ == '__main__':
    main()
