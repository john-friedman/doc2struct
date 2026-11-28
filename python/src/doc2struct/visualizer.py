"""visualizer.py - open tokens or normalizer rows in the browser."""
import os
import webbrowser
from html import escape

from .html_token_normalizer import BOLD, CENTERED, ITALIC, UNDERLINE
from .html_tokenizer import END, START, TEXT

_CSS = """<html><head><meta charset="utf-8"><style>
body{font-family:Arial,sans-serif;max-width:1200px;margin:0 auto;padding:20px}
span{display:inline-block;padding:5px 8px;margin:3px 0;border-radius:4px;white-space:pre-wrap}
.tok{font-family:monospace;font-size:13px;padding:2px 5px;margin:1px;border-radius:3px}
.start{background:#C8E6C9}.end{background:#FFCDD2}.text{background:#E8EAF6}
.first{background:#DCEDC8}.rest{background:#F9FBE7}.single{background:#E8EAF6}
.table-wrapper{text-align:center;margin:15px 0}
.table-container{display:inline-block;border-radius:8px;padding:8px;box-shadow:0 2px 5px rgba(0,0,0,.1);background:#FFECB3}
table{border-collapse:collapse;background:white}
td{border:2px solid #ddd;padding:10px;text-align:left;vertical-align:top}
td.empty{background:#fafafa}
.meta{font-size:11px;color:#888}
.row-meta{font:11px monospace;color:#777;text-align:right;margin:2px 0}
.row-meta a{color:inherit;text-decoration:none}
.st:target,.table-wrapper:target{outline:2px solid #5C6BC0;scroll-margin-top:16px}
.outline{background:#F5F5F5;border:1px solid #ddd;padding:8px 12px;margin-bottom:20px}
.outline nav{max-height:280px;overflow:auto;margin-top:6px}
.outline a{display:block;padding:3px 0;color:#3949AB;text-decoration:none}
.st{margin:4px 0;padding:2px 8px;border-left:3px solid transparent}
.st-title{font-weight:bold;background:#DCEDC8;border-left-color:#7CB342;padding:6px 8px;margin-top:12px}
.st-text{background:#E8EAF6}
.st-textsmall{background:#F5F5F5;font-size:12px;color:#555}
.st-redundant{background:#ECEFF1;font-size:12px;color:#607D8B;border-left-color:#90A4AE}
.st-list{background:#F3E5F5;border-left-color:#AB47BC;padding:8px}
.list-meta{display:flex;align-items:baseline;gap:8px;margin-bottom:6px}
.list-meta .tag{color:#7B1FA2}
.list-meta .badge{margin-left:auto}
.list-title{font-weight:bold;margin-bottom:6px}
.st-listitem{display:flex;align-items:baseline;gap:8px;padding:3px 0}
.st-listitem .listitem-text{flex:1;min-width:0;white-space:pre-wrap;overflow-wrap:anywhere}
.st-listitem .badge{flex:none;margin-left:auto;white-space:nowrap}
.st-image{background:#FFF3E0;font-style:italic}
.badge{font-family:monospace;font-size:11px;background:#fff;border:1px solid #bbb;border-radius:3px;padding:0 4px;margin-left:8px;font-weight:normal;color:#555}
.cleaned{background:#B2DFDB}.dirty{background:#FFECB3}
.tbl{display:inline-block;max-width:100%;margin:10px 0}
.tbl-title{font-weight:bold;margin:0 0 4px}
.tbl-pre{font-style:italic;color:#555;margin:0 0 6px}
.tbl-foot{font-size:12px;color:#444;margin:6px 0 0}
.tbl-foot b{margin-right:4px}
.tbl-post{font-size:12px;color:#555;margin:6px 0 0}
.tbl-cleanup{background:#F5F5F5;color:#555;padding:6px 8px;margin:6px 0;font-size:12px}
.tbl-cleanup.review{background:#FFF3E0;color:#8D4B00;border-left:3px solid #FF9800}
.tbl-cleanup ul{margin:4px 0;padding-left:20px}
.tag{font-family:monospace;font-size:10px;background:#fff;border:1px solid #bbb;border-radius:3px;padding:0 4px;margin-right:6px;color:#666;font-weight:normal;font-style:normal}
</style></head><body>"""


def _open(html, filename, open_browser):
    with open(filename, 'w', encoding='utf-8') as f:
        f.write(html)
    if open_browser:
        webbrowser.open('file://' + os.path.abspath(filename))


def visualize_tokens(tokens, filename='tokens_visualization.html', open_browser=True):
    out = [_CSS]
    for kind, a, b in tokens:
        if kind == START:
            attrs = ''.join(f' {k}="{v}"' for k, v in (b or {}).items())
            out.append(f"<span class='tok start'>&lt;{escape(a)}{escape(attrs)}&gt;</span>")
        elif kind == END:
            out.append(f"<span class='tok end'>&lt;/{escape(a)}&gt;</span>")
        else:
            out.append(f"<span class='tok text'>{escape(a)}</span>")
    out.append('</body></html>')
    _open(''.join(out), filename, open_browser)


def _style(r):
    s = []
    f = r['flags']
    if f & BOLD: s.append('font-weight:bold')
    if f & ITALIC: s.append('font-style:italic')
    if f & UNDERLINE: s.append('text-decoration:underline')
    if r['font_size']: s.append(f"font-size:{r['font_size']}px")
    if r['indent']: s.append(f"margin-left:{r['indent']}px")
    return ';'.join(s)


def _run(r, cls):
    if r['src'] is not None:
        return f"<span class='{cls}'><img src='{escape(r['src'])}' alt='{escape(r['text'] or '')}' style='max-width:300px'></span>"
    text = escape(r['text'] or '')
    if r['href'] is not None and r['href'] != '':
        text = f"<a href='{escape(r['href'])}'>{text}</a>"
    return f"<span class='{cls}' style='{_style(r)}'>{text}</span>"


def _line(runs):
    center = 'text-align:center;' if runs[0]['flags'] & CENTERED else ''
    out = [f"<div style='{center}'>"]
    for i, r in enumerate(runs):
        cls = 'single' if len(runs) == 1 else ('first' if i == 0 else 'rest')
        out.append(_run(r, cls))
    out.append('</div>')
    return ''.join(out)


def _table(runs):
    cells, order = {}, []
    for r in runs:
        k = (r['row'], r['col'])
        if k not in cells:
            rs, cs = (int(x) for x in (r['span'] or '1x1').split('x'))
            cells[k] = {'rs': rs, 'cs': cs, 'lines': {}}
            order.append(k)
        cells[k]['lines'].setdefault(r['line'], []).append(r)
    rows = {}
    for k in order:
        rows.setdefault(k[0], []).append(k)
    out = ["<div class='table-wrapper'><div class='table-container'><table>"]
    for y in sorted(rows):
        out.append('<tr>')
        for k in sorted(rows[y], key=lambda k: k[1]):
            c = cells[k]
            has = any((r['text'] or r['src'] is not None) for ls in c['lines'].values() for r in ls)
            out.append(f"<td rowspan={c['rs']} colspan={c['cs']}{'' if has else ' class=empty'}>")
            out.append(''.join(_line(c['lines'][n]) for n in sorted(c['lines'])) if has else '')
            out.append('</td>')
        out.append('</tr>')
    out.append('</table></div></div>')
    return ''.join(out)


def visualize_instructions(rows, filename='instructions_visualization.html', open_browser=True):
    out = [_CSS]
    i, n = 0, len(rows)
    while i < n:
        r = rows[i]
        j = i
        if r['row'] is not None:
            while j < n and rows[j]['row'] is not None and rows[j]['block'] == r['block']:
                j += 1
            out.append(_table(rows[i:j]))
        else:
            while j < n and rows[j]['row'] is None and rows[j]['line'] == r['line']:
                j += 1
            out.append(_line(rows[i:j]))
        i = j
    out.append('</body></html>')
    _open(''.join(out), filename, open_browser)


def _row_anchor(r):
    return f" id='{escape('row-' + str(r['id']))}'" if r.get('id') is not None else ''


def _row_meta(r):
    if r.get('id') is None:
        return ''
    anchor = escape('row-' + str(r['id']))
    label = escape(f"#{r['id']} · {r['type']} · level {r['level']}")
    return f"<div class='row-meta'><a href='#{anchor}'>{label}</a></div>"


def _st_table(r):
    """A table with its title, preamble, footnotes and postamble, in document order."""
    info = r.get('cleaned')
    issues = [s for s in info if s.startswith('unresolved_') or s == 'ambiguous_text_attachment'] if isinstance(info, list) else []
    cls = 'dirty' if info is False or issues else 'cleaned'
    out = [f"<div class='table-wrapper'{_row_anchor(r)} style='margin-left:{r['level'] * 24}px'><div class='tbl'>", _row_meta(r)]
    if r.get('text'):
        out.append(f"<div class='tbl-title'><span class='tag'>title</span>{escape(r['text'])}</div>")
    if r.get('preamble'):
        out.append(f"<div class='tbl-pre'><span class='tag'>preamble</span>{escape(r['preamble'])}</div>")
    if isinstance(info, list):
        state = ' review' if issues else ''
        summary = f"cleanup: {len(info) - len(issues)} changes, {len(issues)} issues" if info else 'cleanup: no recorded changes or issues'
        out.append(f"<details class='tbl-cleanup{state}'><summary>{summary}</summary><ul>")
        for entry in info:
            out.append(f"<li>{escape(entry.replace('_', ' '))}</li>")
        out.append('</ul></details>')
    out.append(f"<div class='table-container {cls}'><table>")
    for row in r['matrix']:
        out.append('<tr>' + ''.join(f'<td>{escape(c)}</td>' for c in row) + '</tr>')
    out.append('</table></div>')
    for fid, ftext in (r.get('footnotes') or ()):
        out.append(f"<div class='tbl-foot'><span class='tag'>footnote</span><b>{escape(fid)}</b>{escape(ftext)}</div>")
    if r.get('postamble'):
        out.append(f"<div class='tbl-post'><span class='tag'>postamble</span>{escape(r['postamble'])}</div>")
    out.append('</div></div>')
    return ''.join(out)


def _st_list(r):
    """A grouped list: matrix rows are [relative depth, original item text]."""
    matrix = r['matrix'] or []
    meta = f"level {r['level']} · {len(matrix)} items"
    if r.get('id') is not None:
        meta += f" · id {r['id']}"
    out = [
        f"<div class='st st-list'{_row_anchor(r)} style='margin-left:{r['level'] * 24}px'>",
        _row_meta(r),
        f"<div class='list-meta'><span class='tag'>list</span>"
        f"<span class='badge'>{escape(meta)}</span></div>",
    ]
    if r.get('text'):
        out.append(f"<div class='list-title'>{escape(r['text'])}</div>")
    for depth, text in matrix:
        out.append(
            f"<div class='st-listitem' style='margin-left:{int(depth) * 24}px'>"
            f"<span class='listitem-text'>{escape(text)}</span>"
            f"<span class='badge'>depth {escape(depth)}</span></div>"
        )
    out.append('</div>')
    return ''.join(out)


def visualize_standardized_text(rows, filename='standardized_text_visualization.html', open_browser=True):
    """Render standardized rows, including grouped lists with relative item depths."""
    rows = list(rows)
    out = [_CSS]
    sections = [r for r in rows if r['type'] == 'title' and r.get('id') is not None
                and r.get('class') not in (None, 'predicted header')]
    if sections:
        out.append(f"<details class='outline'><summary>Recognized sections ({len(sections)})</summary><nav>")
        for r in sections:
            label = r['text'] or r.get('standardized_title') or ''
            if len(label) > 120:
                label = label[:117] + '…'
            out.append(f"<a href='#row-{escape(str(r['id']))}' style='margin-left:{r['level'] * 12}px'>{escape(label)}</a>")
        out.append('</nav></details>')
    for r in rows:
        kind = r['type']
        if kind == 'table':
            out.append(_st_table(r))
            continue
        if kind == 'list':
            out.append(_st_list(r))
            continue
        pad = f"margin-left:{r['level'] * 24}px"
        text = escape(r['text'] or '')
        if kind == 'title':
            badge = ' '.join(x for x in (r.get('class'), r.get('standardized_title')) if x)
            badge = f"<span class='badge'>{escape(badge)}</span>" if badge else ''
            out.append(f"<div class='st st-title'{_row_anchor(r)} style='{pad}'>{_row_meta(r)}{text}{badge}</div>")
        elif kind == 'image':
            out.append(f"<div class='st st-image'{_row_anchor(r)} style='{pad}'>{_row_meta(r)}[image] {text}</div>")
        else:
            out.append(f"<div class='st st-{kind}'{_row_anchor(r)} style='{pad}'>{_row_meta(r)}{text}</div>")
    out.append('</body></html>')
    _open(''.join(out), filename, open_browser)
