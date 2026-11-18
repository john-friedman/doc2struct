"""
rules - every judgment the pipeline makes, as data.

    rules = load_rules('SEC:10-K')                     # a document type in a bundled rule pack
    rules = load_rules('path/to/my_rules.json')        # your own file
    rules = load_rules('SEC:10-K', extend={...})       # a bundled document type plus extra rules
    rules = load_rules({...})                          # a dict

A rule pack (presets/*.json) is one file for a family of documents:

    {"namespace": "SEC",
     "rule_sets": {"10k": {"headers": [...]}, ...},     # named rule sets; anything a Rules dict can hold
     "document_types": {"10-K": "10k", "10-K/A": "10k", ...}} # document type -> rule set

Document types match case-insensitively. A rule set lists only what differs from the defaults (presets/defaults.json).
Stage code contains no rule data.

header_styles is an any-of policy shared by rules without an explicit requires
field. It defaults to bold, all_caps, or proper_case. A header's requires list
overrides that policy and requires every listed style; requires=[] accepts an
unstyled heading. header_styles=[] disables the shared policy entirely.
"""
import json
from pathlib import Path
from typing import Optional, Union

from .. import _native

from .types import (AdapterRules, HTMLAdapterRules, PDFAdapterRules, BBAdapterRules, TextAdapterRules, CustomColumn, HeaderRule,
                    ListRules, RemoveRules, Rules, TableRules, TextRules, HeadingRules)

_PACKS = None


def _packs() -> dict:
    """{NAMESPACE: pack dict}, read once from presets/*.json"""
    global _PACKS
    if _PACKS is None:
        _PACKS = {}
        for raw in _native.preset_packs():
            pack = json.loads(raw)
            ns = pack['namespace'].upper()
            pack['_document_types_upper'] = {k.upper(): v for k, v in pack['document_types'].items()}
            _PACKS[ns] = pack
    return _PACKS


def list_namespaces():
    return sorted(_packs())


def list_document_types(namespace: str):
    pack = _packs().get(namespace.upper())
    if pack is None:
        raise KeyError(f'unknown namespace {namespace!r}; known: {list_namespaces()}')
    return list(pack['document_types'])


def _from_pack(namespace: str, document_type: str) -> Rules:
    pack = _packs()[namespace.upper()]
    set_name = pack['_document_types_upper'].get(document_type.strip().upper())
    if set_name is None:
        raise KeyError(f'no document type {document_type!r} in {namespace.upper()}; known document types: {list_document_types(namespace)}')
    name = f'{namespace.upper()}:{document_type.strip().upper()}'
    return Rules.from_dict(json.loads(_native.resolve_rules(name, True)))



def _read(source) -> dict:
    if isinstance(source, dict):
        return source
    p = Path(source)
    if not p.exists():
        raise FileNotFoundError(f'no rules file {str(source)!r}; bundled rules are named NAMESPACE:DOCUMENT_TYPE, '
                                f'namespaces: {list_namespaces()}')
    with open(p, encoding='utf-8') as f:
        return json.load(f)


def _resolve(source) -> Rules:
    if isinstance(source, Rules):
        return source
    if isinstance(source, str) and ':' in source:
        ns, document_type = source.split(':', 1)
        if ns.strip().upper() in _packs():
            return _from_pack(ns.strip(), document_type)
    return Rules.from_dict(json.loads(_native.resolve_rules(
        json.dumps(_read(source), ensure_ascii=False, allow_nan=False), False)))


def merge(base: Rules, extra: Rules) -> Rules:
    """Append headers/columns; override nondefault adapter fields and rule sections."""
    return Rules.from_dict(json.loads(_native.merge_rules(
        json.dumps(base.to_dict(), allow_nan=False), json.dumps(extra.to_dict(), allow_nan=False))))


def load_rules(source: Union[str, Path, dict, Rules], extend: Optional[Union[str, Path, dict, Rules]] = None) -> Rules:
    rules = _resolve(source if not isinstance(source, Path) else str(source))
    if extend is not None:
        rules = merge(rules, _resolve(extend if not isinstance(extend, Path) else str(extend)))
    return rules


__all__ = ['load_rules', 'merge', 'list_namespaces', 'list_document_types', 'Rules', 'HeaderRule', 'RemoveRules', 'TableRules', 'TextRules', 'HeadingRules', 'ListRules',
           'CustomColumn', 'AdapterRules', 'HTMLAdapterRules', 'PDFAdapterRules', 'TextAdapterRules']
