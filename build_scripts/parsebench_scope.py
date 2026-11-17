"""doc2struct's ParseBench scope: text, formatting, tables, and forms.

Preserve the official weights within each dimension; average the four equally.
Charts and visual grounding are outside the package's extraction scope. Apply
the same dimension selection to published rows before comparing rankings.

To rescore an existing run without rerunning extraction:
    python build_scripts/parsebench_scope.py --summary RUN/summary.json
An optional --leaderboard CSV recomputes peers over the same four dimensions.
"""
from statistics import mean

DIMENSIONS = ("text", "formatting", "tables", "forms")
COLUMNS = ("Text", "Formatting", "Tables", "Forms")
EXCLUDED_SUBSETS = ("charts", "layout")


def overall(dimensions):
    values = [dimensions.get(name, {}).get("score") for name in DIMENSIONS]
    return mean(values) if all(value is not None for value in values) else None


def published_overall(row):
    try:
        return mean(float(row[name]) for name in COLUMNS) / 100
    except (KeyError, TypeError, ValueError):
        return None


def main():
    import argparse
    import csv
    import json
    from pathlib import Path

    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument('--summary', required=True, type=Path)
    cli.add_argument('--leaderboard', type=Path)
    args = cli.parse_args()
    summary = json.loads(args.summary.read_text(encoding='utf-8'))
    peers = []
    if args.leaderboard:
        with args.leaderboard.open(encoding='utf-8-sig', newline='') as stream:
            peers = [score for row in csv.DictReader(stream)
                     if (score := published_overall(row)) is not None]
    result = {'dimensions': DIMENSIONS, 'excluded_subsets': EXCLUDED_SUBSETS, 'scores': {}}
    for mode, data in summary['scores'].items():
        value = overall(data['dimensions'])
        item = {'overall': value,
                'dimensions': {name: data['dimensions'].get(name, {}).get('score') for name in DIMENSIONS}}
        if args.leaderboard and value is not None:
            item['hypothetical_rank'] = 1 + sum(peer > value for peer in peers)
            item['total_entries_including_doc2struct'] = len(peers) + 1
        result['scores'][mode] = item
    if args.leaderboard:
        result['ranking_note'] = ('Custom four-dimension comparison, not an official submission. '
                                  'Published peers cannot be filtered to native-text inputs.')
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
