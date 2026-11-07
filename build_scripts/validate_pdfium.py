"""Validate a bundled private PDFium runtime before building a wheel."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path

BRIDGE_ABI = 2


def validate(binary, *, load=True, licenses=None):
    binary = Path(binary).resolve()
    metadata = json.loads(binary.with_name('version.json').read_text(encoding='utf-8'))
    if metadata.get('bridge_abi') != BRIDGE_ABI:
        raise ValueError(f'PDFium bridge ABI must be {BRIDGE_ABI}')
    if metadata.get('sha256') != hashlib.sha256(binary.read_bytes()).hexdigest():
        raise ValueError('PDFium binary does not match version.json')
    licenses = Path(licenses) if licenses is not None else binary.parent / 'licenses'
    for name in ('pdfium.txt', 'third_party.txt'):
        if not (licenses / name).is_file():
            raise ValueError('Missing PDFium redistribution notice: ' + name)
    if load:
        runtime = ctypes.CDLL(str(binary))
        runtime.DS_PDF_BridgeVersion.restype = ctypes.c_uint
        if runtime.DS_PDF_BridgeVersion() != BRIDGE_ABI:
            raise ValueError('Unsupported PDFium bridge ABI')
        getattr(runtime, 'DS_PDF_LoadTextPage')
    return metadata


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('library', type=Path)
    parser.add_argument('--no-load', action='store_true', help='Validate cross-compiled binaries without executing them')
    parser.add_argument('--licenses', type=Path, help='Override the accompanying licenses directory')
    args = parser.parse_args()
    validate(args.library, load=not args.no_load, licenses=args.licenses)
